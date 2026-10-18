package org.megavideoprotect.app

import org.json.JSONObject

private val KEY = "b".repeat(64)
private val PACKAGE = byteArrayOf('a'.code.toByte(), 'b'.code.toByte(), 'c'.code.toByte())
private val NAME = sha256(PACKAGE) + ".mvpk"
private val HASH = NAME.substring(0, 64)
private val OTHER = byteArrayOf(1, 2, 3)
private val OTHER_NAME = sha256(OTHER) + ".mvpk"

/**
 * Live loopback HTTP: the bearer is the library key, HEAD exposes the durable
 * offset, PUT carries X-Total-Size, a Range answer must be 206 with a matching
 * Content-Range, and a wrong key maps to exactly one message.
 */
fun main() {
    val relay = V3Relay(KEY)
    try {
        val api = SyncHttp(relay.endpoint(), KEY)

        val health = api.request("GET", "/healthz")
        check(health.status == 200 && JSONObject(String(health.bytes)).getInt("protocol") == 3)
        check(JSONObject(String(api.request("GET", "/v3/files").bytes)).getJSONArray("files").length() == 0)

        relay.addComplete(PACKAGE, NAME)
        val head = api.request("HEAD", "/v3/files/$NAME")
        check(head.status == 200)
        check(head.header("Content-Length") == "3") // header lookup is case-insensitive
        check(head.header("X-Offset") == "3")
        check(head.header("X-Complete") == "true")
        check(head.bytes.isEmpty())

        val put = api.request(
            "PUT",
            "/v3/files/$OTHER_NAME?offset=0",
            OTHER,
            mapOf("X-Total-Size" to "3"),
        )
        check(put.status == 201)
        check(JSONObject(String(put.bytes)).getLong("offset") == 3L)
        check(relay.rows[OTHER_NAME]?.complete == true)

        val range = api.request("GET", "/v3/files/$NAME", headers = mapOf("Range" to "bytes=1-2"))
        check(range.status == 206)
        check(String(range.bytes) == "bc")
        check(range.header("Content-Range") == "bytes 1-2/3")
        check(range.header("ETag") == "\"$NAME\"")

        relay.unauthorized = true
        val rejected = runCatching { api.request("GET", "/v3/files") }.exceptionOrNull()
        check(rejected is SyncHttp.Failure && rejected.status == 401)
        check(rejected.message == "The server rejected this library key.")
        check(SyncHttp.message(401) == SyncHttp.KEY_REJECTED)

        relay.unauthorized = false
        check(runCatching { SyncHttp("http://vault.example", KEY) }.isFailure)
        check(runCatching { SyncHttp("https://vault.example", "a".repeat(63)) }.isFailure)
        check(SyncHttp("http://127.0.0.1:${relay.endpoint().substringAfterLast(':')}", HASH).request("GET", "/healthz").status == 200)

        println("SyncHttp: bearer key, HEAD/offset headers, PUT, Range, 401 and validation passed")
    } finally {
        relay.close()
    }
}
