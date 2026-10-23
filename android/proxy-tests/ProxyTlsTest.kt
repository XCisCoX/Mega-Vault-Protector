package org.megavideoprotect.app

import java.io.*
import java.net.*
import java.security.KeyStore
import java.security.cert.X509Certificate
import javax.net.ssl.*
import kotlin.concurrent.thread

fun testTlsTunnels() {
    val ks = KeyStore.getInstance("PKCS12").apply { FileInputStream(System.getenv("PROXY_TEST_KEYSTORE")).use { load(it, "throwaway-fixture".toCharArray()) } }
    val keys = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm()).apply { init(ks, "throwaway-fixture".toCharArray()) }
    val serverContext = SSLContext.getInstance("TLS").apply { init(keys.keyManagers,null,null) }
    val ca = ks.getCertificate("fixture") as X509Certificate
    val pinned = object : X509TrustManager {
        override fun getAcceptedIssuers() = arrayOf(ca)
        override fun checkClientTrusted(chain: Array<X509Certificate>, auth: String) = error("Not used")
        override fun checkServerTrusted(chain: Array<X509Certificate>, auth: String) { check(chain.first().encoded.contentEquals(ca.encoded)) }
    }
    val clientContext = SSLContext.getInstance("TLS").apply { init(null,arrayOf(pinned),null) }
    for (kind in ProxyKind.entries) {
        for (trusted in listOf(false,true)) {
            (serverContext.serverSocketFactory.createServerSocket(0,5,InetAddress.getLoopbackAddress()) as SSLServerSocket).use { origin ->
                origin.soTimeout=5000
                val server = thread(isDaemon=true) {
                    runCatching { origin.accept().use { s ->
                        s.soTimeout=4000
                        val reader = s.getInputStream().bufferedReader()
                        val lines=generateSequence { reader.readLine()?.takeIf { it.isNotEmpty() } }.toList()
                        if (trusted) {
                            check(lines.first().startsWith("GET /"))
                            check(lines.any { it == "Authorization: Bearer origin-only" })
                            check(lines.none { it.startsWith("Proxy-Authorization:",true) })
                        }
                        s.getOutputStream().write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK".toByteArray())
                    } }
                }
                ServerSocket(0,5,InetAddress.getLoopbackAddress()).use { proxy ->
                    proxy.soTimeout=5000
                    var failure: Throwable?=null
                    val tunnel=thread(isDaemon=true) {
                        try { proxy.accept().use { downstream ->
                            downstream.soTimeout=4000
                            val i=DataInputStream(downstream.getInputStream()); val o=downstream.getOutputStream()
                            if (kind==ProxyKind.HTTP) {
                                val line=StringBuilder();val lines=mutableListOf<String>()
                                while(true) { val b=i.readUnsignedByte();if(b==10) { val s=line.toString().trimEnd('\r');line.clear();if(s.isEmpty())break;lines+=s } else line.append(b.toChar()) }
                                check(lines.first()=="CONNECT localhost:${origin.localPort} HTTP/1.1")
                                check(lines.none { it.startsWith("Authorization:",true) }) { "Origin credential leaked into CONNECT" }
                                check(lines.any { it=="Proxy-Authorization: Basic dXNlcjpwYXNz" })
                                o.write("HTTP/1.1 200 Connection Established\r\n\r\n".toByteArray());o.flush()
                            } else {
                                check(i.readUnsignedByte()==5);i.readFully(ByteArray(i.readUnsignedByte()))
                                o.write(byteArrayOf(5,2));o.flush()
                                check(i.readUnsignedByte()==1);i.readFully(ByteArray(i.readUnsignedByte()));i.readFully(ByteArray(i.readUnsignedByte()))
                                o.write(byteArrayOf(1,0));o.flush()
                                check(i.readUnsignedByte()==5);check(i.readUnsignedByte()==1);check(i.readUnsignedByte()==0);check(i.readUnsignedByte()==3)
                                val host=ByteArray(i.readUnsignedByte());i.readFully(host);check(host.toString(Charsets.UTF_8)=="localhost")
                                check(i.readUnsignedShort()==origin.localPort)
                                o.write(byteArrayOf(5,0,0,1,127,0,0,1,0,80));o.flush()
                            }
                            Socket("127.0.0.1",origin.localPort).use { upstream ->
                                upstream.soTimeout=4000
                                val reverse=thread(isDaemon=true) { runCatching { upstream.getInputStream().copyTo(o) };runCatching { downstream.shutdownOutput() } }
                                runCatching { i.copyTo(upstream.getOutputStream()) };runCatching { upstream.shutdownOutput() };reverse.join(4500)
                            }
                        } } catch(t: Throwable) { failure=t }
                    }
                    val c=ProxyTransport.open(URL("https://localhost:${origin.localPort}/"),ProxyProfile("tls",kind,"127.0.0.1",proxy.localPort,"user","pass")) as HttpsURLConnection
                    c.connectTimeout=3000;c.readTimeout=3000;c.setRequestProperty("Authorization","Bearer origin-only")
                    if(trusted) { c.sslSocketFactory=clientContext.socketFactory; c.hostnameVerifier=HostnameVerifier { host,_ -> host=="localhost" } }
                    val result=runCatching { c.inputStream.use { it.readBytes().toString(Charsets.UTF_8) } }
                    c.disconnect();tunnel.join(5000);server.join(5000)
                    failure?.let { throw it }
                    if(trusted) check(result.getOrThrow()=="OK") else check(result.isFailure) { "Untrusted origin accepted" }
                }
            }
        }
    }
    println("PASS HTTPS CONNECT/SOCKS5: default TLS rejects fixture, peer-scoped pin accepts; origin auth absent from CONNECT")
}
