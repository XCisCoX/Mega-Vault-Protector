package org.megavideoprotect.app

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.KeyStore
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager
import org.json.JSONObject

/**
 * One request/response over the protocol 3 API. Stateless: every call opens its
 * own connection, presents the library key in the Authorization header, and
 * never follows a redirect (that would replay the key at another host).
 */
class SyncHttp(private val server: String, private val libraryKey: String) {
    init {
        require(SyncPolicy.validServer(server)) {
            "Enter the server URL, for example https://vault.example:8766. Plain HTTP is accepted only for 127.0.0.1."
        }
        require(SyncPolicy.isHex(libraryKey, 64)) { "The library key is 64 lowercase hex characters." }
    }

    data class Response(val status: Int, val bytes: ByteArray, val headers: Map<String, List<String>>) {
        /** Header lookup is case-insensitive: HttpURLConnection preserves the wire case. */
        fun header(name: String): String? =
            headers.entries.firstOrNull { it.key.equals(name, true) }?.value?.firstOrNull()
    }

    /** A non-2xx answer, carrying the server's small JSON error body when there is one. */
    class Failure(val status: Int, val payload: JSONObject?, message: String) : IOException(message)

    // Platform CA validation and normal hostname verification, captured once so
    // a permissive process-wide default (the PC share path) cannot weaken sync.
    private val socketFactory = run {
        val managers = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
            .apply { init(null as KeyStore?) }
            .trustManagers
            .filterIsInstance<X509TrustManager>()
            .single()
        SSLContext.getInstance("TLS").apply { init(null, arrayOf(managers), null) }.socketFactory
    }

    fun request(
        method: String,
        path: String,
        body: ByteArray? = null,
        headers: Map<String, String> = emptyMap(),
    ): Response {
        require(path == "/healthz" || path.startsWith("/v3/")) { "Unexpected sync path" }
        val connection = URL(server.trimEnd('/') + path).openConnection() as HttpURLConnection
        try {
            if (connection is HttpsURLConnection) connection.sslSocketFactory = socketFactory
            connection.instanceFollowRedirects = false
            connection.connectTimeout = 15_000
            connection.readTimeout = 30_000
            connection.requestMethod = method
            connection.setRequestProperty("Authorization", "Bearer $libraryKey")
            connection.setRequestProperty(
                "Content-Type",
                if (method == "PUT") "application/octet-stream" else "application/json",
            )
            headers.forEach { (name, value) -> connection.setRequestProperty(name, value) }
            if (body != null) {
                connection.doOutput = true
                connection.setFixedLengthStreamingMode(body.size)
                connection.outputStream.use { it.write(body) }
            }
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            // A listing is at most a few MiB; a package is streamed in <=1 MiB ranges.
            val bytes = if (method == "HEAD" || stream == null) ByteArray(0) else stream.use { input ->
                val out = ByteArrayOutputStream()
                val buffer = ByteArray(65536)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    if (out.size() + count > SyncPolicy.MAX_RESPONSE) {
                        throw IOException("The server sent more data than a sync response may carry.")
                    }
                    out.write(buffer, 0, count)
                }
                out.toByteArray()
            }
            val responseHeaders = connection.headerFields
                .mapNotNull { (name, values) -> name?.let { it to values } }
                .toMap()
            val response = Response(status, bytes, responseHeaders)
            if (status !in 200..299) throw Failure(status, payload(bytes), message(status))
            return response
        } catch (failure: Failure) {
            throw failure
        } catch (failure: IOException) {
            throw IOException("Cannot reach the sync server. Check the URL and that it is running.", failure)
        } finally {
            connection.disconnect()
        }
    }

    private fun payload(bytes: ByteArray): JSONObject? =
        runCatching { JSONObject(String(bytes, Charsets.UTF_8)) }.getOrNull()

    companion object {
        const val KEY_REJECTED = "The server rejected this library key."

        fun message(status: Int): String = when (status) {
            401 -> KEY_REJECTED
            413 -> "The package is larger than this server accepts."
            416 -> "The server refused the requested byte range."
            422 -> "The server rejected a package because its bytes do not match its name."
            507 -> "The server has no space left for this package."
            else -> "The sync server answered HTTP $status."
        }
    }
}
