package org.megavideoprotect.app

import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import org.json.JSONObject

fun main() {
    val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
    val calls = mutableListOf<String>()
    server.createContext("/") { x ->
        calls.add(x.requestMethod + " " + x.requestURI.toString())
        check(x.requestHeaders.getFirst("Authorization") == "Bearer " + "a".repeat(64))
        val body = when {
            x.requestURI.path == "/v2/pair" -> {
                val request = JSONObject(x.requestBody.bufferedReader().readText())
                check(request.keySet() == setOf("code", "name"))
                "{\"device_id\":\"${"b".repeat(32)}\",\"token\":\"${"c".repeat(64)}\"}"
            }
            x.requestMethod == "HEAD" -> {
                x.responseHeaders.add("Content-Length", "3")
                x.responseHeaders.add("X-Upload-Offset", "2")
                x.responseHeaders.add("X-Blob-Complete", "false")
                ""
            }
            x.requestMethod == "PUT" -> {
                check(x.requestHeaders.getFirst("X-Blob-Size") == "3")
                check(x.requestBody.readBytes().contentEquals(byteArrayOf(3)))
                "{\"offset\":3,\"complete\":true}"
            }
            x.requestHeaders.getFirst("Range") != null -> {
                check(x.requestHeaders.getFirst("Range") == "bytes=1-2")
                x.responseHeaders.add("Content-Range", "bytes 1-2/3")
                "bc"
            }
            else -> "{\"objects\":[]}"
        }
        val bytes = body.toByteArray()
        x.sendResponseHeaders(if (x.requestHeaders.getFirst("Range") != null) 206 else 200, if (x.requestMethod == "HEAD") -1 else bytes.size.toLong())
        if (x.requestMethod != "HEAD") x.responseBody.use { it.write(bytes) }
        x.close()
    }
    server.start()
    try {
        val api = SyncHttp("http://127.0.0.1:${server.address.port}", "a".repeat(64), "")
        check(api.json("GET", "/v2/objects").getJSONArray("objects").length() == 0)
        check(api.json("POST", "/v2/pair", JSONObject().put("code", "once").put("name", "phone")).getString("token") == "c".repeat(64))
        val head = api.request("HEAD", "/v2/blobs/${"0".repeat(64)}")
        check(head.header("X-Upload-Offset") == "2")
        val put = api.request("PUT", "/v2/blobs/${"0".repeat(64)}?offset=2", byteArrayOf(3), mapOf("X-Blob-Size" to "3"))
        check(JSONObject(String(put.bytes)).getLong("offset") == 3L)
        val range = api.request("GET", "/v2/blobs/${"0".repeat(64)}", headers = mapOf("Range" to "bytes=1-2"))
        check(String(range.bytes) == "bc" && range.status == 206)
        check(calls.size == 5)
        check(runCatching { SyncHttp("http://public.example", "", "") }.isFailure)
        println("SyncHttp: live loopback protocol checks passed")
    } finally { server.stop(0) }
}
