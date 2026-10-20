package org.megavideoprotect.app

import java.net.*
import java.io.*
import kotlin.concurrent.thread

private fun response(s: Socket) {
    val reader = s.getInputStream().bufferedReader()
    val headers = generateSequence { reader.readLine()?.takeIf { it.isNotEmpty() } }.toList()
    check(headers.none { it.startsWith("Proxy-Authorization:", true) }) { "Proxy credential leaked to origin" }
    s.getOutputStream().write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK".toByteArray())
}
fun main() {
    // First tracer: an unresolvable origin is reached through SOCKS with remote DNS and RFC1929 auth.
    ServerSocket(0, 5, InetAddress.getLoopbackAddress()).use { proxy ->
        var fixtureError: Throwable? = null
        val worker = thread {
            try { proxy.accept().use { s ->
                s.soTimeout = 5000
                val i = DataInputStream(s.getInputStream()); val o = s.getOutputStream()
                check(i.readUnsignedByte() == 5); val methods = ByteArray(i.readUnsignedByte()); i.readFully(methods)
                check(methods.contentEquals(byteArrayOf(2)))
                o.write(byteArrayOf(5, 2)); o.flush()
                check(i.readUnsignedByte() == 1)
                fun text(): String { val b=ByteArray(i.readUnsignedByte());i.readFully(b);return b.toString(Charsets.UTF_8) }
                check(text()=="fixture-user");check(text()=="fixture-pass")
                o.write(byteArrayOf(1,0));o.flush()
                check(i.readUnsignedByte()==5);check(i.readUnsignedByte()==1);check(i.readUnsignedByte()==0)
                check(i.readUnsignedByte()==3);check(text()=="remote-dns.invalid");check(i.readUnsignedShort()==80)
                o.write(byteArrayOf(5,0,0,1,127,0,0,1,0,80));o.flush()
                response(s)
            }} catch(t: Throwable) { fixtureError=t }
        }
        val config = ProxyProfile("fixture", ProxyKind.SOCKS5, "127.0.0.1", proxy.localPort, "fixture-user", "fixture-pass")
        val c = ProxyTransport.open(URL("http://remote-dns.invalid/"), config)
        c.connectTimeout=3000;c.readTimeout=3000
        check(c.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }=="OK")
        c.disconnect();worker.join(7000);check(!worker.isAlive);fixtureError?.let { throw it }
    }
    testTlsTunnels()
    testHttpProxy()
    testClosedProxy()
    testValidation()
    println("PASS authenticated SOCKS5, remote DNS, origin credential isolation")
}

private fun testHttpProxy() {
    ServerSocket(0, 5, InetAddress.getLoopbackAddress()).use { proxy ->
        var failure: Throwable? = null
        val worker = thread {
            try {
                repeat(2) { attempt -> proxy.accept().use { socket ->
                    socket.soTimeout = 4000
                    val r = socket.getInputStream().bufferedReader()
                    val headers = generateSequence { r.readLine()?.takeIf { it.isNotEmpty() } }.toList()
                    check(headers.first() == "GET http://remote-http.invalid/resource HTTP/1.1")
                    check(headers.any { it == "Authorization: Bearer destination-only" })
                    val auth = headers.firstOrNull { it.startsWith("Proxy-Authorization:",true) }
                    if (attempt == 0) {
                        check(auth == null)
                        socket.getOutputStream().write("HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"fixture\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".toByteArray())
                    } else {
                        check(auth == "Proxy-Authorization: Basic dXNlcjpwYXNz")
                        socket.getOutputStream().write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK".toByteArray())
                    }
                } }
            } catch (t: Throwable) { failure = t }
        }
        val c = ProxyTransport.open(URL("http://remote-http.invalid/resource"), ProxyProfile("http",ProxyKind.HTTP,"127.0.0.1",proxy.localPort,"user","pass"))
        c.connectTimeout=3000;c.readTimeout=3000
        c.setRequestProperty("Authorization","Bearer destination-only")
        // A caller cannot accidentally plant proxy credentials on an origin request.
        c.setRequestProperty("Proxy-Authorization","should-be-stripped")
        check(c.inputStream.use { it.readBytes().toString(Charsets.UTF_8) } == "OK")
        c.disconnect();worker.join(6000);check(!worker.isAlive);failure?.let { throw it }
    }
    println("PASS HTTP 407 scoped authentication, absolute-form proxy request")
}

private fun testClosedProxy() {
    ServerSocket(0,5,InetAddress.getLoopbackAddress()).use { origin ->
        val dead = ServerSocket(0,5,InetAddress.getLoopbackAddress()).use { it.localPort }
        origin.soTimeout=300
        for (kind in ProxyKind.entries) {
            val c = ProxyTransport.open(URL("http://127.0.0.1:${origin.localPort}/"),ProxyProfile("dead",kind,"127.0.0.1",dead))
            c.connectTimeout=300;c.readTimeout=300
            try { check(runCatching { c.inputStream.read() }.isFailure) } finally { c.disconnect() }
        }
        check(runCatching { origin.accept().close() }.exceptionOrNull() is SocketTimeoutException) { "Proxy failure fell back direct" }
    }
    println("PASS HTTP/SOCKS failure never falls back to reachable direct origin")
}

private fun testValidation() {
    for (host in listOf("", "http://host", "server:8080", "user@host", "a/b", "a b", "256.1.1.1", "-host", "a..b")) {
        check(runCatching { ProxyProfile("x",ProxyKind.SOCKS5,host,1080) }.isFailure) { "Accepted invalid host" }
    }
    for (port in listOf(0,-1,65536)) check(runCatching { ProxyProfile("x",ProxyKind.HTTP,"example.com",port) }.isFailure)
    check(!ProxyProfile("x",ProxyKind.HTTP,"example.com",8080,"sensitive-user","sensitive-password").toString().contains("sensitive"))
    ProxyProfile("x",ProxyKind.SOCKS5,"::1",1080)
    println("PASS host/port validation and redacted diagnostic string")
}
