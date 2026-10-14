package org.megavideoprotect.app

import com.sun.net.httpserver.HttpsServer
import com.sun.net.httpserver.HttpsConfigurator
import java.io.File
import java.net.InetSocketAddress
import java.security.KeyStore
import java.security.SecureRandom
import java.security.cert.X509Certificate
import javax.net.ssl.*

fun main() {
    val path = System.getProperty("sync.test.keystore") ?: error("Test keystore property required")
    val keyStore = KeyStore.getInstance("PKCS12").apply { File(path).inputStream().use { load(it, "changeit".toCharArray()) } }
    val kmf = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm()).apply { init(keyStore, "changeit".toCharArray()) }
    val serverContext = SSLContext.getInstance("TLS").apply { init(kmf.keyManagers, null, SecureRandom()) }
    val server = HttpsServer.create(InetSocketAddress("127.0.0.1", 0), 0)
    server.httpsConfigurator = HttpsConfigurator(serverContext)
    var received = 0
    server.createContext("/healthz") { x -> received++; val bytes = "{\"protocol\":2}".toByteArray(); x.sendResponseHeaders(200, bytes.size.toLong()); x.responseBody.use { it.write(bytes) }; x.close() }
    server.start()
    val original = HttpsURLConnection.getDefaultSSLSocketFactory()
    try {
        val endpoint = "https://localhost:${server.address.port}"
        val pin = sha256(keyStore.getCertificate("fixture").encoded)
        check(runCatching { SyncHttp(endpoint, "", "").json("GET", "/healthz") }.isFailure)
        check(runCatching { SyncHttp(endpoint, "", pin).json("GET", "/healthz") }.isFailure) // pin never replaces CA trust
        val trustAll = object : X509TrustManager {
            override fun getAcceptedIssuers() = emptyArray<X509Certificate>()
            override fun checkClientTrusted(c: Array<X509Certificate>, a: String) = Unit
            override fun checkServerTrusted(c: Array<X509Certificate>, a: String) = Unit
        }
        val poisoned = SSLContext.getInstance("TLS").apply { init(null, arrayOf(trustAll), SecureRandom()) }
        HttpsURLConnection.setDefaultSSLSocketFactory(poisoned.socketFactory)
        check(runCatching { SyncHttp(endpoint, "", "").json("GET", "/healthz") }.isFailure) { "Sync must not inherit PC-share/global permissive TLS" }
        check(received == 0) { "No request body/auth sent to untrusted TLS peer" }
        println("SyncTls: CA validation, additive pin, global TLS isolation passed")
    } finally { HttpsURLConnection.setDefaultSSLSocketFactory(original); server.stop(0) }
}
