package org.megavideoprotect.app

import okhttp3.*
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.*
import java.net.*
import java.security.Principal
import java.security.cert.Certificate
import javax.net.SocketFactory
import javax.net.ssl.*

/** Explicit routes only. Never uses ProxySelector, java.net.Authenticator, cookies or global TLS defaults. */
object ProxyTransport {
    fun open(url: URL, proxy: ProxyProfile?): HttpURLConnection {
        require(url.protocol in listOf("http", "https") && url.userInfo == null) { "Unsupported network URL" }
        return Connection(url, proxy)
    }

    private fun builder(proxy: ProxyProfile?): OkHttpClient.Builder {
        val b = OkHttpClient.Builder().proxy(Proxy.NO_PROXY).authenticator(Authenticator.NONE)
            .proxyAuthenticator(Authenticator.NONE).retryOnConnectionFailure(false)
            .connectionPool(ConnectionPool(0, 1, java.util.concurrent.TimeUnit.SECONDS))
        if (proxy?.kind == ProxyKind.HTTP) {
            b.proxy(Proxy(Proxy.Type.HTTP, InetSocketAddress(proxy.host, proxy.port)))
            if (proxy.username.isNotEmpty()) b.proxyAuthenticator { route, response ->
                if (route?.proxy?.type() != Proxy.Type.HTTP || response.request.header("Proxy-Authorization") != null) null
                else response.request.newBuilder().header("Proxy-Authorization", Credentials.basic(proxy.username, proxy.password, Charsets.UTF_8)).build()
            }
        } else if (proxy != null) {
            // Synthetic InetAddress retains the hostname without ever resolving the destination locally.
            b.dns(object : Dns { override fun lookup(hostname: String) = listOf(InetAddress.getByAddress(hostname, byteArrayOf(0, 0, 0, 1))) })
            b.socketFactory(SocksFactory(proxy))
        }
        return b
    }

    /** SOCKS is implemented here rather than delegated to Android's unsupported SOCKS authenticator path. */
    private class SocksFactory(private val p: ProxyProfile) : SocketFactory() {
        override fun createSocket(): Socket = object : Socket(Proxy.NO_PROXY) {
            override fun connect(endpoint: SocketAddress, timeout: Int) {
                val target = endpoint as InetSocketAddress
                try {
                    super.connect(InetSocketAddress(p.host, p.port), timeout)
                    val oldTimeout = soTimeout
                    soTimeout = if (timeout > 0) timeout else 15000
                    val i = DataInputStream(getInputStream()); val o = getOutputStream()
                    val auth = p.username.isNotEmpty()
                    o.write(byteArrayOf(5, 1, if (auth) 2 else 0)); o.flush()
                    if (i.readUnsignedByte() != 5 || i.readUnsignedByte() != if (auth) 2 else 0) throw IOException("SOCKS5 authentication method rejected")
                    if (auth) {
                        val u = p.username.toByteArray(Charsets.UTF_8); val pw = p.password.toByteArray(Charsets.UTF_8)
                        try {
                            o.write(1); o.write(u.size); o.write(u); o.write(pw.size); o.write(pw); o.flush()
                        } finally { u.fill(0); pw.fill(0) }
                        if (i.readUnsignedByte() != 1 || i.readUnsignedByte() != 0) throw IOException("SOCKS5 authentication failed")
                    }
                    val host = target.hostString.toByteArray(Charsets.UTF_8)
                    if (host.size !in 1..255) throw IOException("SOCKS5 destination hostname too long")
                    o.write(byteArrayOf(5, 1, 0, 3, host.size.toByte())); o.write(host)
                    o.write(target.port ushr 8); o.write(target.port and 255); o.flush()
                    if (i.readUnsignedByte() != 5 || i.readUnsignedByte() != 0 || i.readUnsignedByte() != 0) throw IOException("SOCKS5 connection rejected")
                    val length = when (i.readUnsignedByte()) { 1 -> 4; 4 -> 16; 3 -> i.readUnsignedByte(); else -> throw IOException("Invalid SOCKS5 response") }
                    i.readFully(ByteArray(length + 2)); soTimeout = oldTimeout
                } catch (t: Throwable) { close(); throw t }
            }
            override fun connect(endpoint: SocketAddress) = connect(endpoint, 15000)
        }
        override fun createSocket(host: String, port: Int): Socket = createSocket().apply { connect(InetSocketAddress.createUnresolved(host, port)) }
        override fun createSocket(host: InetAddress, port: Int): Socket = createSocket(host.hostName, port)
        override fun createSocket(host: String, port: Int, local: InetAddress, localPort: Int): Socket = createSocket().apply { bind(InetSocketAddress(local, localPort)); connect(InetSocketAddress.createUnresolved(host, port)) }
        override fun createSocket(host: InetAddress, port: Int, local: InetAddress, localPort: Int): Socket = createSocket(host.hostName, port, local, localPort)
    }

    /** URLConnection facade keeps existing streaming reads and peer-specific TLS setters intact. */
    private class Connection(url: URL, private val proxyProfile: ProxyProfile?) : HttpsURLConnection(url) {
        private val headers = linkedMapOf<String, MutableList<String>>()
        private var output: ByteArrayOutputStream? = null
        private var response: Response? = null
        private var call: Call? = null
        private var client: OkHttpClient? = null
        private var closed = false
        private val trustManager = (TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).apply { init(null as java.security.KeyStore?) }).trustManagers.filterIsInstance<X509TrustManager>().first()
        private var tlsFactory: SSLSocketFactory = SSLContext.getInstance("TLS").apply { init(null, arrayOf(trustManager), null) }.socketFactory
        private var verifier: HostnameVerifier? = null
        override fun setSSLSocketFactory(factory: SSLSocketFactory) { check(!connected); tlsFactory = factory }
        override fun getSSLSocketFactory() = tlsFactory
        override fun setHostnameVerifier(value: HostnameVerifier) { check(!connected); verifier = value }
        override fun getHostnameVerifier(): HostnameVerifier = verifier ?: super.getHostnameVerifier()
        override fun connect() {
            if (response != null) return
            if (closed) throw IOException("Connection closed")
            val b = builder(proxyProfile).connectTimeout(connectTimeout.toLong(), java.util.concurrent.TimeUnit.MILLISECONDS)
                .readTimeout(readTimeout.toLong(), java.util.concurrent.TimeUnit.MILLISECONDS)
                .writeTimeout((readTimeout.takeIf { it > 0 } ?: 30000).toLong(), java.util.concurrent.TimeUnit.MILLISECONDS)
                .followRedirects(instanceFollowRedirects).followSslRedirects(false)
                .sslSocketFactory(tlsFactory, trustManager)
            verifier?.let { b.hostnameVerifier(it) }
            val c = b.build(); client = c
            val request = Request.Builder().url(url)
            headers.forEach { (key, values) -> values.forEach { request.addHeader(key, it) } }
            // Proxy credentials can only be attached by the scoped HTTP proxy authenticator.
            request.removeHeader("Proxy-Authorization")
            val body = if (doOutput || method in listOf("POST", "PUT", "PATCH")) (output?.toByteArray() ?: ByteArray(0)).toRequestBody() else null
            val actualMethod = if (doOutput && method == "GET") "POST" else method
            val executing = c.newCall(request.method(actualMethod, body).build()); call = executing
            connected = true
            response = executing.execute()
            output?.reset(); output = null
        }
        private fun result(): Response { connect(); return response ?: throw IOException("No response") }
        override fun getOutputStream(): OutputStream { check(!connected); doOutput = true; return output ?: ByteArrayOutputStream().also { output = it } }
        override fun getInputStream(): InputStream {
            val r = result()
            if (r.code >= 400) throw FileNotFoundException("HTTP ${r.code}")
            return r.body?.byteStream() ?: ByteArrayInputStream(ByteArray(0))
        }
        override fun getErrorStream(): InputStream? = if (result().code >= 400) result().body?.byteStream() else null
        override fun getResponseCode() = result().code
        override fun getResponseMessage() = result().message
        override fun getHeaderField(name: String?): String? = if (name == null) "HTTP/1.1 ${result().code} ${result().message}" else result().header(name)
        override fun getHeaderFields(): Map<String, List<String>> = result().headers.toMultimap()
        override fun getContentLengthLong() = result().body?.contentLength() ?: -1
        override fun getContentLength() = contentLengthLong.let { if (it > Int.MAX_VALUE) -1 else it.toInt() }
        override fun getContentType(): String? = getHeaderField("Content-Type")
        override fun setRequestProperty(key: String, value: String) { check(!connected); headers.keys.firstOrNull { it.equals(key,true) }?.let { headers.remove(it) }; headers[key] = mutableListOf(value) }
        override fun addRequestProperty(key: String, value: String) { check(!connected); val actual = headers.keys.firstOrNull { it.equals(key,true) } ?: key; headers.getOrPut(actual) { mutableListOf() }.add(value) }
        override fun getRequestProperty(key: String): String? = headers.entries.firstOrNull { it.key.equals(key,true) }?.value?.lastOrNull()
        override fun getRequestProperties(): Map<String,List<String>> = headers.mapValues { it.value.toList() }
        override fun disconnect() { closed = true; call?.cancel(); response?.close(); client?.connectionPool?.evictAll(); client?.dispatcher?.executorService?.shutdown(); output?.reset(); output = null }
        override fun usingProxy() = proxyProfile != null
        override fun getCipherSuite(): String = result().handshake?.cipherSuite?.javaName ?: throw IllegalStateException("Not TLS")
        override fun getLocalCertificates(): Array<Certificate>? = result().handshake?.localCertificates?.toTypedArray()
        override fun getServerCertificates(): Array<Certificate> = result().handshake?.peerCertificates?.toTypedArray() ?: throw SSLPeerUnverifiedException("Not TLS")
        override fun getPeerPrincipal(): Principal = result().handshake?.peerPrincipal ?: throw SSLPeerUnverifiedException("Not TLS")
        override fun getLocalPrincipal(): Principal? = result().handshake?.localPrincipal
    }
}
