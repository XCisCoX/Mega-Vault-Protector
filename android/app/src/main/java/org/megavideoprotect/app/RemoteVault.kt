package org.megavideoprotect.app

import android.net.Uri
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.InetAddress
import java.net.Socket
import java.net.URL
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

/**
 * The vault shared from the PC. The QR code is `mvpvault://host:port?fp=sha256`.
 * `fp` is the TLS certificate. The phone still has to send the vault password,
 * and that password only goes out after the certificate matches.
 */
object RemoteVault {
    @Volatile var baseUrl: String? = null
        private set
    @Volatile var token: String? = null
        private set

    @Volatile var lastFailure: String? = null
        private set

    /** Cloud login uses the same pinned TLS session as PC share, and can delete. */
    @Volatile var cloud = false
        private set

    @Volatile var syncSpeed = "normal"

    @Volatile private var cloudModel = ""
    @Volatile private var cloudKind = ""

    @Volatile private var sslFactory: SSLSocketFactory? = null
    @Volatile private var hostVerifier: HostnameVerifier? = null


    fun connected(): Boolean = !baseUrl.isNullOrBlank() && !token.isNullOrBlank()

    /** Address shown in the gallery footer, without the session token. */
    fun label(): String? {
        val base = baseUrl ?: return null
        val prefix = if (cloud) "Cloud · " else "PC · "
        return prefix + base.removePrefix("https://")
    }

    fun fileUrl(id: Long): String {
        val base = baseUrl ?: return ""
        return "$base/v1/file?id=$id"
    }

    /** Pins this connection to the certificate from the scanned code. */
    fun applySecurity(connection: HttpsURLConnection) {
        sslFactory?.let { connection.sslSocketFactory = it }
        hostVerifier?.let { connection.hostnameVerifier = it }
    }

    fun disconnect() {
        baseUrl = null
        token = null
        cloud = false
        cloudModel = ""
        cloudKind = ""
        sslFactory = null
        hostVerifier = null

    }

    /**
     * Null when the PC accepted the password. Otherwise a sentence for the
     * login screen. The password is not kept after this returns.
     */
    fun login(raw: String, password: String, model: String = "", kind: String = ""): String? {
        cloud = false
        cloudModel = model.trim().replace("\r", " ").replace("\n", " ").take(64)
        cloudKind = kind.trim().take(16)
        if (password.isEmpty()) return "Enter the vault password."
        val text = raw.trim()
        val uri = runCatching { Uri.parse(text) }.getOrNull()
            ?: return "That code is not a PC share."
        if (uri.scheme != "mvpvault") return "That code is not a PC share."
        val host = uri.host ?: return "That code has no address."
        val port = uri.port
        if (port <= 0) return "That code has no port."
        val fingerprint = uri.getQueryParameter("fp")?.trim()?.lowercase().orEmpty()
        if (fingerprint.length != 64 || fingerprint.any { it !in '0'..'9' && it !in 'a'..'f' }) {
            return "That code has no certificate. Share again on the PC and scan the new one."
        }
        val base = "https://$host:$port"
        val factory = pinnedFactory(fingerprint)
        val verifier = HostnameVerifier { hostname, _ -> hostname == host }
        val body = password.toByteArray(Charsets.UTF_8)
        val conn = (AppProxy.open(URL("$base/v1/login")) as HttpsURLConnection).apply {
            sslSocketFactory = factory
            hostnameVerifier = verifier
            requestMethod = "POST"
            doOutput = true
            setRequestProperty("Content-Type", "text/plain; charset=utf-8")
            if (cloudModel.isNotEmpty()) setRequestProperty("X-Model", cloudModel)
            if (cloudKind.isNotEmpty()) setRequestProperty("X-Kind", cloudKind)
            setFixedLengthStreamingMode(body.size)
            connectTimeout = 8000
            readTimeout = 120000
            instanceFollowRedirects = false
        }
        return try {
            conn.outputStream.use { it.write(body) }
            val code = conn.responseCode
            when (code) {
                401 -> "Wrong password."
                200 -> {
                    val json = conn.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8)
                    val session = JSONObject(json).optString("token")
                    if (session.isBlank()) {
                        "The PC did not accept the password."
                    } else {
                        sslFactory = factory
                        hostVerifier = verifier
                        // PC trust is scoped to PC connections (applySecurity/get/login).
                        // Never change process defaults: sync v2 and updates require normal CA/hostname checks.
                        baseUrl = base
                        token = session
                        cloud = cloudKind == "phone" || cloudKind == "pc"
                        null
                    }
                }
                else -> "The PC refused the sign-in. Start sharing on the PC and try again."
            }
        } catch (error: Exception) {
            val detail = error.message.orEmpty()
            if (detail.contains("certificate", ignoreCase = true)) {
                "The PC certificate does not match the code. Scan it again."
            } else {
                "Could not reach the PC. Same Wi-Fi, and allow the app through the firewall."
            }
        } finally {
            body.fill(0.toByte())
            conn.disconnect()
        }
    }

    fun getText(path: String): String? = getBytes(path)?.let { String(it, Charsets.UTF_8) }

    fun getBytes(path: String): ByteArray? {
        val base = baseUrl ?: return null
        val secret = token ?: return null
        return get(base, secret, path)
    }

    fun videoSize(id: Long): Long {
        val text = getText("/v1/size?id=$id") ?: return -1L
        return text.trim().toLongOrNull() ?: -1L
    }

    fun readRange(id: Long, offset: Long, size: Int): ByteArray? {
        val ask = size.coerceIn(1, 1 shl 20)
        return getBytes("/v1/read?id=$id&offset=$offset&size=$ask")
    }

    /** Whole plaintext, for a photo. Null when it is missing or larger than `maxBytes`. */
    fun readAll(id: Long, maxBytes: Int): ByteArray? {
        val size = videoSize(id)
        if (size <= 0L || size > maxBytes) return null
        val out = ByteArray(size.toInt())
        var at = 0
        while (at < out.size) {
            val n = minOf(1 shl 20, out.size - at)
            val chunk = readRange(id, at.toLong(), n) ?: return null
            if (chunk.isEmpty()) return null
            chunk.copyInto(out, at, 0, minOf(chunk.size, n))
            at += chunk.size
            if (chunk.size < n) break
        }
        return if (at == out.size) out else null
    }

    private fun get(base: String, secret: String, path: String): ByteArray? {
        val conn = (AppProxy.open(URL(base + path)) as HttpsURLConnection).apply {
            sslFactory?.let { sslSocketFactory = it }
            hostVerifier?.let { hostnameVerifier = it }
            requestMethod = "GET"
            setRequestProperty("Authorization", "Bearer $secret")
            if (cloudModel.isNotEmpty()) setRequestProperty("X-Model", cloudModel)
            if (cloudKind.isNotEmpty()) setRequestProperty("X-Kind", cloudKind)
            if (cloud && (path.contains("/v1/file") || path.contains("/v1/read"))) {
                setRequestProperty("X-Sync-Speed", syncSpeed)
            }
            connectTimeout = 20000
            readTimeout = 60000
            instanceFollowRedirects = false
        }
        return try {
            val code = conn.responseCode
            if (code != 200) {
                lastFailure = "PC replied $code"
                return null
            }
            lastFailure = null
            conn.inputStream.use { input ->
                val out = ByteArrayOutputStream()
                input.copyTo(out)
                out.toByteArray()
            }
        } catch (error: Exception) {
            lastFailure = error.message ?: error.javaClass.simpleName
            null
        } finally {
            conn.disconnect()
        }
    }

    /** Null when the cloud vault removed the file. */
    fun remove(path: String): String? {
        val base = baseUrl ?: return "Not signed in."
        val secret = token ?: return "Not signed in."
        val conn = (AppProxy.open(URL(base + path)) as HttpsURLConnection).apply {
            sslFactory?.let { sslSocketFactory = it }
            hostVerifier?.let { hostnameVerifier = it }
            requestMethod = "POST"
            doOutput = true
            setRequestProperty("Authorization", "Bearer $secret")
            if (cloudModel.isNotEmpty()) setRequestProperty("X-Model", cloudModel)
            if (cloudKind.isNotEmpty()) setRequestProperty("X-Kind", cloudKind)
            setFixedLengthStreamingMode(0)
            connectTimeout = 20000
            readTimeout = 60000
            instanceFollowRedirects = false
        }
        return try {
            val code = conn.responseCode
            if (code == 200) {
                lastFailure = null
                null
            } else {
                lastFailure = "Cloud replied $code"
                lastFailure
            }
        } catch (error: Exception) {
            lastFailure = error.message ?: error.javaClass.simpleName
            lastFailure
        } finally {
            conn.disconnect()
        }
    }

    internal fun pinnedFactory(fingerprint: String): SSLSocketFactory {
        val trust = object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit

            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {
                if (chain.isEmpty()) throw CertificateException("missing certificate")
                val digest = MessageDigest.getInstance("SHA-256").digest(chain[0].encoded)
                val actual = digest.joinToString("") { "%02x".format(it) }
                if (!sameHex(actual, fingerprint)) {
                    throw CertificateException("certificate does not match the code")
                }
            }

            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }
        val context = SSLContext.getInstance("TLS")
        context.init(null, arrayOf<TrustManager>(trust), SecureRandom())
        return PinnedSockets(context.socketFactory)
    }

    private fun sameHex(left: String, right: String): Boolean {
        if (left.length != right.length) return false
        var diff = 0
        for (i in left.indices) diff = diff or (left[i].code xor right[i].code)
        return diff == 0
    }
}

/** Android otherwise checks the certificate name during the handshake and rejects the PC. */
private class PinnedSockets(private val delegate: SSLSocketFactory) : SSLSocketFactory() {
    private fun tune(socket: Socket): Socket {
        (socket as? SSLSocket)?.let { ssl ->
            val params = ssl.sslParameters
            params.endpointIdentificationAlgorithm = ""
            ssl.sslParameters = params
        }
        return socket
    }

    override fun getDefaultCipherSuites(): Array<String> = delegate.defaultCipherSuites
    override fun getSupportedCipherSuites(): Array<String> = delegate.supportedCipherSuites
    override fun createSocket(socket: Socket?, host: String?, port: Int, autoClose: Boolean): Socket =
        tune(delegate.createSocket(socket, host, port, autoClose))
    override fun createSocket(host: String?, port: Int): Socket = tune(delegate.createSocket(host, port))
    override fun createSocket(host: String?, port: Int, local: InetAddress?, localPort: Int): Socket =
        tune(delegate.createSocket(host, port, local, localPort))
    override fun createSocket(address: InetAddress?, port: Int): Socket = tune(delegate.createSocket(address, port))
    override fun createSocket(address: InetAddress?, port: Int, local: InetAddress?, localPort: Int): Socket =
        tune(delegate.createSocket(address, port, local, localPort))
}
