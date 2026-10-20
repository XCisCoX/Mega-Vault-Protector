package org.megavideoprotect.app

import com.sun.net.httpserver.HttpsServer
import com.sun.net.httpserver.HttpsConfigurator
import java.net.InetSocketAddress
import java.security.KeyStore
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import java.io.File

fun httpTest(args: Array<String>) {
    val store = KeyStore.getInstance("PKCS12").apply { File(args[1]).inputStream().use { load(it,"changeit".toCharArray()) } }
    val km = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm()).apply { init(store,"changeit".toCharArray()) }
    val tls = SSLContext.getInstance("TLS").apply { init(km.keyManagers,null,null) }
    val server = HttpsServer.create(InetSocketAddress("localhost",443),0).apply { httpsConfigurator = HttpsConfigurator(tls) }
    server.createContext("/ok") { e ->
        check(e.requestHeaders.getFirst("User-Agent") == "MegaVaultProtect/2.0.3 (music plugin)")
        val b="fLaCfixture".toByteArray(); e.responseHeaders.add("Content-Type","application/octet-stream"); e.sendResponseHeaders(200,b.size.toLong()); e.responseBody.use { it.write(b) }
    }
    server.createContext("/bad") { e -> val b="<html>".toByteArray(); e.responseHeaders.add("Content-Type","audio/flac"); e.sendResponseHeaders(200,b.size.toLong()); e.responseBody.use { it.write(b) } }
    server.createContext("/redirect") { e -> e.responseHeaders.add("Location","https://evil.example/audio"); e.sendResponseHeaders(302,-1); e.close() }
    server.createContext("/huge") { e -> e.responseHeaders.add("Content-Type","audio/flac"); e.sendResponseHeaders(200,1000000); e.close() }
    server.start()
    try {
        val m=PluginManifest("test.fixture","Fixture","1.0.0","Fixture","https://localhost",setOf("localhost"),"Fixture")
        fun body(path: String, cap: Int): ByteArray {
            val c=java.net.URL("https://localhost$path").openConnection() as javax.net.ssl.HttpsURLConnection
            c.setRequestProperty("User-Agent", "MegaVaultProtect/2.0.3 (music plugin)")
            try { return PluginHttp.readBody(c,cap,true) } finally { c.disconnect() }
        }
        check(body("/ok",100).contentEquals("fLaCfixture".toByteArray()))
        check(runCatching { body("/bad",100) }.isFailure)
        check(runCatching { body("/huge",100) }.exceptionOrNull() is PluginLimitException)
        // Real production policy must reject local networking, even with a manually constructed manifest.
        val h=PluginHttp(m)
        check(runCatching { h.audio("/ok",100) }.isFailure)
        h.close(); check(runCatching { h.audio("/ok",100) }.isFailure)
        println("PluginHttpTest: real TLS body fixture, FLAC, cap, local-network denial and closed cancellation passed")
    } finally { server.stop(0) }
}
