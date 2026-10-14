package org.megavideoprotect.app

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.KeyStore
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager
import org.json.JSONObject

internal fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 255) }
internal fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes).hex()

class SyncHttp(val endpoint: String, private val token: String, pin: String) {
    init {
        require(SyncPolicy.validEndpoint(endpoint)) { "Use HTTPS, or HTTP on loopback only; no URL credentials/path." }
        require(token.isEmpty() || SyncPolicy.validSecret(token)) { "Invalid device token" }
        require(pin.isEmpty() || SyncPolicy.validSecret(pin)) { "Pin must be 64 lowercase SHA-256 hex characters" }
    }
    private val factory = run {
        val tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
        tmf.init(null as KeyStore?)
        val normal = tmf.trustManagers.filterIsInstance<X509TrustManager>().single()
        val pinned = object : X509TrustManager {
            override fun getAcceptedIssuers(): Array<X509Certificate> = normal.acceptedIssuers
            override fun checkClientTrusted(c: Array<X509Certificate>, a: String) = normal.checkClientTrusted(c, a)
            override fun checkServerTrusted(c: Array<X509Certificate>, a: String) {
                normal.checkServerTrusted(c, a)
                if (pin.isNotEmpty() && (c.isEmpty() || sha256(c[0].encoded) != pin)) throw CertificateException("Certificate pin mismatch")
            }
        }
        SSLContext.getInstance("TLS").apply { init(null, arrayOf(pinned), null) }.socketFactory
    }
    data class Response(val status: Int, val bytes: ByteArray, val headers: Map<String, List<String>>) {
        fun header(name: String): String? = headers.entries.firstOrNull { it.key.equals(name, true) }?.value?.firstOrNull()
    }
    class Failure(val status: Int, val payload: JSONObject?) : IOException(
        if (status == 401) "Device revoked/unauthorized. Re-pair this device." else "Sync HTTP $status"
    )
    fun request(method: String, path: String, body: ByteArray? = null, headers: Map<String, String> = emptyMap()): Response {
        require(path.startsWith("/") && !path.startsWith("//"))
        val connection = URL(endpoint.trimEnd('/') + path).openConnection() as HttpURLConnection
        try {
            if (connection is HttpsURLConnection) connection.sslSocketFactory = factory
            // Default hostname verifier AND normal platform CA validation stay enabled.
            connection.instanceFollowRedirects = false
            connection.connectTimeout = 15_000
            connection.readTimeout = 30_000
            connection.requestMethod = method
            if (token.isNotEmpty()) connection.setRequestProperty("Authorization", "Bearer $token")
            headers.forEach { (k, v) -> connection.setRequestProperty(k, v) }
            if (body != null) {
                connection.doOutput = true
                connection.setFixedLengthStreamingMode(body.size)
                if (!headers.keys.any { it.equals("Content-Type", true) }) connection.setRequestProperty("Content-Type", "application/octet-stream")
                connection.outputStream.use { it.write(body) }
            }
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            // Blob traffic is one chunk; manifest capped at 10k records. Never unbounded readBytes().
            val bytes = if (method == "HEAD" || stream == null) byteArrayOf() else stream.use {
                val out = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(65536)
                while (true) {
                    val n = it.read(buffer)
                    if (n < 0) break
                    if (out.size() + n > 8 * 1024 * 1024) throw IOException("Oversized sync response")
                    out.write(buffer, 0, n)
                }
                out.toByteArray()
            }
            if (status !in 200..299) throw Failure(status, runCatching { JSONObject(String(bytes, Charsets.UTF_8)) }.getOrNull())
            val responseHeaders: Map<String?, List<String>> = connection.headerFields
            return Response(status, bytes, responseHeaders.mapNotNull { (key, values) -> key?.let { it to values } }.toMap())
        } finally { connection.disconnect() }
    }
    fun json(method: String, path: String, body: JSONObject? = null): JSONObject = JSONObject(
        String(request(method, path, body?.toString()?.toByteArray(Charsets.UTF_8), if (body != null) mapOf("Content-Type" to "application/json") else emptyMap()).bytes, Charsets.UTF_8)
    )
}
