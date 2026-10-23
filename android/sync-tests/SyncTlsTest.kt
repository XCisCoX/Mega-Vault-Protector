package org.megavideoprotect.app

import com.sun.net.httpserver.HttpsConfigurator
import com.sun.net.httpserver.HttpsServer
import java.io.File
import java.net.InetSocketAddress
import java.security.KeyStore
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.concurrent.atomic.AtomicInteger
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager

/**
 * TLS stays platform-default: a self-signed relay is refused, and a permissive
 * process-wide default (which the PC-share path can install) cannot weaken sync.
 */
fun main() {
    val path = System.getProperty("sync.test.keystore") ?: error("Test keystore property required")
    val keyStore = KeyStore.getInstance("PKCS12").apply {
        File(path).inputStream().use { load(it, "changeit".toCharArray()) }
    }
    val managers = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm())
        .apply { init(keyStore, "changeit".toCharArray()) }
    val serverContext = SSLContext.getInstance("TLS").apply { init(managers.keyManagers, null, SecureRandom()) }

    val server = HttpsServer.create(InetSocketAddress("127.0.0.1", 0), 0)
    server.httpsConfigurator = HttpsConfigurator(serverContext)
    val received = AtomicInteger()
    server.createContext("/healthz") { exchange ->
        received.incrementAndGet()
        val body = "{\"ok\":true,\"protocol\":3}".toByteArray()
        exchange.sendResponseHeaders(200, body.size.toLong())
        exchange.responseBody.use { it.write(body) }
        exchange.close()
    }
    server.start()

    val original = HttpsURLConnection.getDefaultSSLSocketFactory()
    val endpoint = "https://localhost:${server.address.port}"
    try {
        val untrusted = runCatching { SyncHttp(endpoint, "d".repeat(64)).request("GET", "/healthz") }.exceptionOrNull()
        check(untrusted != null) { "A self-signed certificate must not be accepted" }
        check(received.get() == 0)

        val trustAll = object : X509TrustManager {
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = Unit
            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) = Unit
        }
        val poisoned = SSLContext.getInstance("TLS").apply { init(null, arrayOf(trustAll), SecureRandom()) }
        HttpsURLConnection.setDefaultSSLSocketFactory(poisoned.socketFactory)
        val stillUntrusted = runCatching { SyncHttp(endpoint, "d".repeat(64)).request("GET", "/healthz") }.exceptionOrNull()
        check(stillUntrusted != null) { "Sync must not inherit a permissive global TLS factory" }
        check(received.get() == 0) { "No request may reach an untrusted peer" }

        println("SyncTls: self-signed relay refused, platform CA trust isolated from global defaults")
    } finally {
        HttpsURLConnection.setDefaultSSLSocketFactory(original)
        server.stop(0)
    }
}
