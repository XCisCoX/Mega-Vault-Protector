package org.megavideoprotect.app

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.concurrent.thread
import org.json.JSONArray
import org.json.JSONObject

/**
 * A minimal protocol 3 relay on a real loopback socket, with exact control over
 * status codes and headers. It is a test fixture, not the repository server and
 * not real MVPK crypto: the package bytes are opaque fixtures.
 */
internal class V3Relay(val key: String) {
    class Entry(var total: Long, var bytes: ByteArray, var complete: Boolean, val seq: Int)

    class Reply(val status: Int, val headers: List<Pair<String, String>>, val body: ByteArray)

    val rows = LinkedHashMap<String, Entry>()

    @Volatile var protocol = 3
    @Volatile var unauthorized = false
    @Volatile var answerPresentAtOffsetZero = false
    @Volatile var hidePartialsFromHead = false
    @Volatile var includeIncomplete = false
    @Volatile var failPutAfterStoreOnce = false
    @Volatile var listingOverride: String? = null
    @Volatile var requests = 0
    @Volatile var putCount = 0
    val putOffsets = CopyOnWriteArrayList<Long>()
    val rangeOffsets = CopyOnWriteArrayList<Long>()
    val failures = CopyOnWriteArrayList<Throwable>()
    private var seq = 0

    private val socket = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
    private val worker = thread(start = true, isDaemon = true, name = "v3-relay") { serve() }

    fun endpoint(): String = "http://127.0.0.1:${socket.localPort}"

    fun close() {
        runCatching { socket.close() }
        worker.join(2000)
        check(failures.isEmpty()) { failures.joinToString { it.toString() } }
    }

    fun addComplete(bytes: ByteArray, name: String = sha256(bytes) + ".mvpk") {
        rows[name] = Entry(bytes.size.toLong(), bytes, true, ++seq)
    }

    fun addPartial(name: String, total: Long, prefix: ByteArray) {
        rows[name] = Entry(total, prefix, false, ++seq)
    }

    fun completeCount(): Int = rows.values.count { it.complete }

    private fun serve() {
        while (!socket.isClosed) {
            val client = try {
                socket.accept()
            } catch (error: Exception) {
                break
            }
            try {
                handle(client)
            } catch (error: Throwable) {
                failures.add(error)
            } finally {
                runCatching { client.close() }
            }
        }
    }

    private fun handle(client: java.net.Socket) {
        val input = client.getInputStream()
        val requestLine = readLine(input) ?: return
        val parts = requestLine.split(' ')
        if (parts.size < 3) return
        val method = parts[0].uppercase()
        val target = parts[1]
        val headers = LinkedHashMap<String, String>()
        var contentLengthCount = 0
        while (true) {
            val line = readLine(input) ?: break
            if (line.isEmpty()) break
            val colon = line.indexOf(':')
            if (colon <= 0) continue
            val name = line.substring(0, colon).trim().lowercase()
            if (name == "content-length") contentLengthCount++
            headers[name] = line.substring(colon + 1).trim()
        }
        val length = headers["content-length"]?.toIntOrNull() ?: 0
        val buffer = ByteArray(length)
        var read = 0
        while (read < length) {
            val count = input.read(buffer, read, length - read)
            if (count < 0) break
            read += count
        }
        requests++
        val reply = route(method, target, headers, buffer.copyOf(read), contentLengthCount)
        val output = client.getOutputStream()
        val declared = reply.headers.any { it.first.equals("Content-Length", true) }
        val head = StringBuilder("HTTP/1.1 ${reply.status} ${reason(reply.status)}\r\n")
        for ((name, value) in reply.headers) head.append("$name: $value\r\n")
        if (!declared) head.append("Content-Length: ${reply.body.size}\r\n")
        head.append("Connection: close\r\n\r\n")
        output.write(head.toString().toByteArray(Charsets.ISO_8859_1))
        if (method != "HEAD" && reply.body.isNotEmpty()) output.write(reply.body)
        output.flush()
    }

    private fun route(
        method: String,
        target: String,
        headers: Map<String, String>,
        body: ByteArray,
        contentLengthCount: Int,
    ): Reply {
        val question = target.indexOf('?')
        val path = if (question < 0) target else target.substring(0, question)
        val query = if (question < 0) "" else target.substring(question + 1)
        if (method == "GET" && path == "/healthz") {
            val complete = rows.values.filter { it.complete }
            return json(
                200,
                JSONObject().put("ok", true).put("protocol", protocol)
                    .put("files", complete.size).put("bytes", complete.sumOf { it.total }).toString(),
            )
        }
        if (unauthorized || headers["authorization"] != "Bearer $key") return json(401, "{\"error\":\"unauthorized\"}")
        if (path == "/v3/files") {
            if (method != "GET") return json(405, "{\"error\":\"method not allowed\"}")
            listingOverride?.let { return json(200, "{\"files\":$it}") }
            val array = JSONArray()
            rows.entries.sortedByDescending { it.value.seq }.forEach { (name, entry) ->
                if (entry.complete || includeIncomplete) {
                    array.put(JSONObject().put("name", name).put("size", entry.total).put("seq", entry.seq))
                }
            }
            return json(200, JSONObject().put("files", array).toString())
        }
        val name = if (path.startsWith("/v3/files/")) path.removePrefix("/v3/files/") else ""
        if (!NAME.matches(name)) return json(404, "{\"error\":\"not found\"}")
        return when (method) {
            "PUT" -> put(name, query, headers, body, contentLengthCount)
            "HEAD", "GET" -> send(name, method == "HEAD", headers)
            "DELETE" -> {
                if (rows.remove(name) == null) json(404, "{\"error\":\"not found\"}")
                else Reply(204, emptyList(), ByteArray(0))
            }
            else -> json(405, "{\"error\":\"method not allowed\"}")
        }
    }

    private fun put(
        name: String,
        query: String,
        headers: Map<String, String>,
        body: ByteArray,
        contentLengthCount: Int,
    ): Reply {
        putCount++
        val offset = query.split('&').firstOrNull { it.startsWith("offset=") }
            ?.removePrefix("offset=")?.toLongOrNull() ?: 0L
        putOffsets.add(offset)
        if (contentLengthCount != 1) return json(400, "{\"error\":\"one Content-Length required\"}")
        if (body.size > SyncPolicy.CHUNK) return json(413, "{\"error\":\"chunk too large\"}")
        val total = headers["x-total-size"]?.toLongOrNull() ?: return json(400, "{\"error\":\"invalid header\"}")
        if (total > SyncPolicy.MAX_PACKAGE) return json(413, "{\"error\":\"file too large\"}")
        val entry = rows[name]
        if (entry != null && entry.complete) {
            return json(200, JSONObject().put("offset", entry.total).put("complete", true).toString())
        }
        // The engine treats "200 complete at offset 0" as already present.
        if (entry == null && answerPresentAtOffsetZero) {
            return json(200, JSONObject().put("offset", total).put("complete", true).toString())
        }
        val durable = entry?.bytes?.size?.toLong() ?: 0L
        if (offset != durable) return json(409, JSONObject().put("offset", durable).toString())
        if (offset + body.size > total) return json(400, "{\"error\":\"chunk exceeds declared size\"}")
        val combined = (entry?.bytes ?: ByteArray(0)) + body
        val complete = combined.size.toLong() == total
        if (complete && sha256(combined) + ".mvpk" != name) {
            rows.remove(name)
            return json(422, "{\"error\":\"hash mismatch\"}")
        }
        rows[name] = Entry(total, combined, complete, entry?.seq ?: ++seq)
        if (failPutAfterStoreOnce) {
            failPutAfterStoreOnce = false
            return json(503, "{\"error\":\"storage unavailable\"}")
        }
        return json(
            if (entry == null) 201 else 200,
            JSONObject().put("offset", combined.size).put("complete", complete).toString(),
        )
    }

    private fun send(name: String, head: Boolean, headers: Map<String, String>): Reply {
        val entry = rows[name] ?: return json(404, "{\"error\":\"not found\"}")
        if (head) {
            if (!entry.complete && hidePartialsFromHead) return json(404, "{\"error\":\"not found\"}")
            return Reply(
                200,
                listOf(
                    "Content-Length" to entry.total.toString(),
                    "X-Offset" to entry.bytes.size.toString(),
                    "X-Complete" to entry.complete.toString(),
                ),
                ByteArray(0),
            )
        }
        if (!entry.complete) return json(404, "{\"error\":\"not found\"}")
        val common = listOf(
            "Content-Type" to "application/octet-stream",
            "Accept-Ranges" to "bytes",
            "ETag" to "\"$name\"",
        )
        val range = headers["range"] ?: return Reply(200, common, entry.bytes)
        val match = Regex("bytes=(\\d+)-(\\d*)").matchEntire(range)
            ?: return json(416, "{\"error\":\"invalid range\"}")
        val start = match.groupValues[1].toLong()
        val end = if (match.groupValues[2].isEmpty()) entry.bytes.size - 1L else match.groupValues[2].toLong()
        if (start >= entry.bytes.size || end < start || end >= entry.bytes.size) {
            return json(416, "{\"error\":\"invalid range\"}")
        }
        rangeOffsets.add(start)
        val slice = entry.bytes.copyOfRange(start.toInt(), end.toInt() + 1)
        return Reply(
            206,
            common + ("Content-Range" to "bytes $start-$end/${entry.bytes.size}"),
            slice,
        )
    }

    private fun json(status: Int, body: String): Reply =
        Reply(status, listOf("Content-Type" to "application/json"), body.toByteArray(Charsets.UTF_8))

    private fun readLine(input: InputStream): String? {
        val out = ByteArrayOutputStream()
        while (true) {
            val byte = input.read()
            if (byte < 0) return if (out.size() == 0) null else String(out.toByteArray(), Charsets.ISO_8859_1)
            if (byte == '\r'.code) {
                val next = input.read()
                if (next != '\n'.code) error("Malformed header line")
                return String(out.toByteArray(), Charsets.ISO_8859_1)
            }
            if (byte == '\n'.code) return String(out.toByteArray(), Charsets.ISO_8859_1)
            out.write(byte)
        }
    }

    private fun reason(status: Int): String = when (status) {
        200 -> "OK"
        201 -> "Created"
        204 -> "No Content"
        206 -> "Partial Content"
        302 -> "Found"
        400 -> "Bad Request"
        401 -> "Unauthorized"
        404 -> "Not Found"
        405 -> "Method Not Allowed"
        409 -> "Conflict"
        413 -> "Payload Too Large"
        416 -> "Range Not Satisfiable"
        422 -> "Unprocessable Entity"
        503 -> "Service Unavailable"
        else -> "Error"
    }

    companion object {
        private val NAME = Regex("[0-9a-f]{64}\\.mvpk")
    }
}
