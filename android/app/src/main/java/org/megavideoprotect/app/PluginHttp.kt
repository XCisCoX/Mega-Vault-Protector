package org.megavideoprotect.app

import org.json.JSONObject
import java.io.Closeable
import java.io.IOException
import java.net.InetAddress
import java.net.URI
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext

class PluginLimitException(message: String) : IOException(message)

/** No cookies, cache, persisted URLs, logging, custom trust or global TLS settings. */
class PluginHttp(private val manifest: PluginManifest) : Closeable {
    @Volatile private var cancelled = false
    @Volatile private var connection: HttpsURLConnection? = null
    private val tls = SSLContext.getInstance("TLS").apply { init(null, null, null) }.socketFactory
    override fun close() { cancelled = true; connection?.disconnect() }
    private fun checkActive() { if (cancelled || Thread.currentThread().isInterrupted) throw IOException("Request cancelled") }
    private fun connect(url: String): HttpsURLConnection {
        var at = manifest.checkUrl(url)
        repeat(5) {
            checkActive()
            val addresses = InetAddress.getAllByName(at.host)
            require(addresses.isNotEmpty() && addresses.none { ip ->
                ip.isAnyLocalAddress || ip.isLoopbackAddress || ip.isLinkLocalAddress || ip.isSiteLocalAddress || ip.isMulticastAddress ||
                    (ip.address.size == 16 && (ip.address[0].toInt() and 0xfe) == 0xfc)
            }) { "Local/private plugin network destinations are forbidden" }
            val c = at.toURL().openConnection() as HttpsURLConnection
            connection = c
            c.sslSocketFactory = tls
            // Use the platform's default hostname verifier, not a process-global override.
            c.hostnameVerifier = javax.net.ssl.HostnameVerifier { hostname, session ->
                javax.net.ssl.HttpsURLConnection.getDefaultHostnameVerifier().verify(hostname, session)
            }
            c.instanceFollowRedirects = false; c.useCaches = false
            c.connectTimeout = 15000; c.readTimeout = 20000
            c.setRequestProperty("User-Agent", "MegaVaultProtect/2.0.3 (music plugin)")
            c.setRequestProperty("Accept-Encoding", "identity")
            c.setRequestProperty("Cache-Control", "no-store")
            checkActive()
            val status = c.responseCode
            if (status in listOf(301,302,303,307,308)) {
                val next = c.getHeaderField("Location") ?: error("Missing redirect location")
                c.disconnect()
                at = manifest.checkUrl(at.resolve(next).toASCIIString())
            } else {
                require(status == 200) { "Provider request failed ($status)" }
                return c
            }
        }
        throw IOException("Too many provider redirects")
    }
    fun json(path: String): JSONObject {
        val bytes = request(manifest.baseUrl.trimEnd('/') + path, 1024 * 1024)
        try { return JSONObject(bytes.toString(Charsets.UTF_8)) } finally { bytes.fill(0) }
    }
    fun image(url: String): ByteArray = request(url,1024 * 1024)
    private fun request(url: String, cap: Int): ByteArray {
        val c = connect(url)
        try { return readBody(c,cap,false,::checkActive) }
        finally { c.disconnect(); connection = null }
    }
    fun audio(path: String, cap: Int): ByteArray {
        val c = connect(manifest.baseUrl.trimEnd('/') + path)
        try { return readBody(c,cap,true,::checkActive) }
        finally { c.disconnect(); connection = null }
    }
    companion object {
        /** Separately testable bounded body reader; production calls it only after policy + TLS. */
        internal fun readBody(c: HttpsURLConnection, cap: Int, audio: Boolean, active: () -> Unit = {}): ByteArray {
            require(cap in 1..(128 * 1024 * 1024))
            val length = c.contentLengthLong
            if (length > cap) throw PluginLimitException("This track exceeds the ${cap / 1024 / 1024} MiB memory limit. Increase the plugin limit to 64 or 128 MiB, or choose a shorter track.")
            // Audio without a declared length cannot reserve an exact bounded buffer.
            require(!audio || length > 0) { "Audio provider must declare a bounded content length" }
            val buffer = ByteArray(if (length in 1..cap.toLong()) length.toInt() else cap)
            var count = 0
            val start = System.nanoTime()
            try {
                c.inputStream.use { input ->
                    while (count < buffer.size) {
                        active()
                        if ((System.nanoTime() - start) / 1000000 > 120000) throw IOException("Provider response timed out")
                        val n = input.read(buffer,count,minOf(65536,buffer.size-count))
                        if (n < 0) break
                        count += n
                    }
                    active()
                    if (input.read() != -1) throw PluginLimitException("Response exceeds the configured memory limit")
                }
                require(length < 0 || count.toLong() == length) { "Truncated provider response" }
                if (audio) require(PluginAudio.isFlac(c.contentType.orEmpty(),buffer)) { "Provider response is not verified FLAC audio" }
                if (count == buffer.size) return buffer
                return buffer.copyOf(count).also { buffer.fill(0) }
            } catch (e: Throwable) { buffer.fill(0); throw e }
        }
    }
}
