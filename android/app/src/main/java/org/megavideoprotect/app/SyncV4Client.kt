package org.megavideoprotect.app

import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import org.json.JSONArray
import org.json.JSONObject

/**
 * One item's server-side metadata under protocol 4. Mirrors RecordRef in
 * qt-app/src/sync_state.cpp field for field, so the phone and the desktop agree
 * on what a record is.
 */
data class SyncRecord(
    val id: String,
    val name: String,
    val path: String,
    val tags: List<String>,
    val packageName: String?,
    val size: Long,
    val version: Int,
    val deleted: Boolean,
    val updated: Long,
) {
    companion object {
        fun fromJson(value: JSONObject): SyncRecord {
            val tags = ArrayList<String>()
            val array = value.optJSONArray("tags") ?: JSONArray()
            for (index in 0 until array.length()) {
                val name = array.optString(index, "")
                if (name.isNotEmpty() && name.length <= 64) tags.add(name)
            }
            val packageName = value.optString("package", "").takeIf {
                it.isNotEmpty() && SyncPolicyV4.isPackage(it)
            }
            return SyncRecord(
                id = value.optString("id", ""),
                name = value.optString("name", ""),
                path = value.optString("path", ""),
                tags = tags,
                packageName = packageName,
                size = value.optDouble("size", 0.0).toLong(),
                version = value.optInt("version", 0),
                deleted = value.optBoolean("deleted", false),
                updated = value.optDouble("updated", 0.0).toLong(),
            )
        }
    }
}

/** Rules the protocol 4 client and engine share; deliberately free of Android types. */
object SyncPolicyV4 {
    /** The relay accepts at most one MiB per blob PUT body; ranges use the same size. */
    const val CHUNK = 1024 * 1024

    /** A listing is JSON, never a package. */
    const val MAX_RESPONSE = 8 * 1024 * 1024

    /** The relay's hard package limit is 1 GiB; refuse anything near it early. */
    const val MAX_PACKAGE = 1024L * 1024 * 1024

    /** The protocol this client speaks. The relay advertises it at /healthz. */
    const val PROTOCOL = 4

    /** Exactly 64 lowercase hex characters: a package address. */
    fun isPackage(value: String): Boolean = SyncPolicy.isHex(value, 64)

    /**
     * A record id is what the client invents: 1..64 characters, starting with a
     * letter or digit, then letters, digits, '.', '_' or '-'. Same rule the
     * desktop applies to what a server sends back.
     */
    fun isRecordId(value: String): Boolean {
        if (value.isEmpty() || value.length > 64) return false
        if (!value[0].isLetterOrDigit()) return false
        return value.all { it.isLetterOrDigit() || it == '.' || it == '_' || it == '-' }
    }

    /** Percent-encodes one path segment; ids and addresses are opaque to us. */
    fun segment(value: String): String = URLEncoder.encode(value, "UTF-8").replace("+", "%20")
}

/**
 * The protocol 4 HTTP client: accounts, vaults, versioned records and the
 * content-addressed package blobs behind them.
 *
 * Ported from qt-app/src/sync_client.cpp, including its refusal rules — a
 * redirect is never followed (it would replay the token at another host), a
 * listing may not exceed MAX_RESPONSE, and every server-supplied id or address
 * is validated before it is used.
 */
class SyncV4Client(
    val server: String,
    private var token: String? = null,
    private var cookie: String? = null,
) {
    class Failure(message: String, val status: Int = 0) : IOException(message)

    /** A 409: the server holds a newer version of the record we tried to write. */
    class Conflict(val current: SyncRecord?) : IOException("The server reported a version conflict.")

    data class Page(val records: List<SyncRecord>, val cursor: Long)

    data class BlobState(
        val exists: Boolean = false,
        val size: Long = 0,
        val offset: Long = 0,
        val complete: Boolean = false,
    )

    private data class Response(val status: Int, val bytes: ByteArray, val headers: Map<String, String>) {
        fun text(): String = String(bytes, Charsets.UTF_8)

        fun json(): JSONObject = runCatching { JSONObject(text()) }
            .getOrElse { throw Failure("The server returned invalid JSON.") }

        fun header(name: String): String? = headers[name.lowercase()]
    }

    init {
        require(SyncPolicy.validServer(server)) {
            "Enter the server URL, for example https://sync.example.com. Plain HTTP is only accepted for 127.0.0.1."
        }
    }

    /** The token this client presents, once sign-in has issued one. */
    fun deviceToken(): String? = token

    fun sessionCookie(): String? = cookie

    fun useSession(cookie: String?) {
        this.cookie = cookie
    }

    fun useToken(token: String?) {
        this.token = token
    }

    // ── transport ────────────────────────────────────────────────────────

    private fun send(
        method: String,
        path: String,
        body: ByteArray? = null,
        headers: Map<String, String> = emptyMap(),
    ): Response {
        val connection = URL(server.trimEnd('/') + path).openConnection() as HttpURLConnection
        try {
            connection.instanceFollowRedirects = false
            connection.connectTimeout = 15_000
            connection.readTimeout = 60_000
            connection.requestMethod = method
            connection.setRequestProperty("Accept", "application/json")
            token?.let { connection.setRequestProperty("Authorization", "Bearer $it") }
            cookie?.let { connection.setRequestProperty("Cookie", "mv_session=$it") }
            val blob = path.contains("/blobs/")
            connection.setRequestProperty(
                "Content-Type",
                when {
                    blob -> "application/octet-stream"
                    method == "PUT" || method == "POST" || method == "PATCH" -> "application/json"
                    else -> "application/json"
                },
            )
            headers.forEach { (name, value) -> connection.setRequestProperty(name, value) }
            if (body != null) {
                connection.doOutput = true
                connection.setFixedLengthStreamingMode(body.size)
                connection.outputStream.use { it.write(body) }
            }
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            val bytes = if (method == "HEAD" || stream == null) ByteArray(0) else stream.use { input ->
                val out = ByteArrayOutputStream()
                val chunk = ByteArray(65536)
                while (true) {
                    val read = input.read(chunk)
                    if (read < 0) break
                    if (out.size() + read > SyncPolicyV4.MAX_RESPONSE) {
                        throw Failure("The server sent more data than a sync response may carry.")
                    }
                    out.write(chunk, 0, read)
                }
                out.toByteArray()
            }
            val fields = connection.headerFields.entries
                .mapNotNull { entry -> entry.key?.let { it.lowercase() to (entry.value?.firstOrNull() ?: "") } }
                .toMap()
            return Response(status, bytes, fields)
        } catch (failure: Failure) {
            throw failure
        } catch (failure: IOException) {
            throw Failure("Cannot reach the sync server. Check the URL and that it is running.")
        } finally {
            connection.disconnect()
        }
    }

    private fun reject(status: Int) {
        throw when (status) {
            401 -> Failure(
                "The server rejected this device's token. Sign in again, or issue a new device token on the website.",
                status,
            )
            403 -> Failure("The server refused this request.", status)
            404 -> Failure("The server does not have that vault or record any more.", status)
            413 -> Failure("The package is larger than this server accepts.", status)
            416 -> Failure("The server refused the requested byte range.", status)
            422 -> Failure("The server rejected a package because its bytes do not match its name.", status)
            507 -> Failure("The account has no space left for this package.", status)
            else -> Failure("The sync server answered HTTP $status.", status)
        }
    }

    private fun ok(response: Response, vararg statuses: Int): Response {
        if (response.status in statuses) return response
        reject(response.status)
    }

    // ── account ──────────────────────────────────────────────────────────

    /** Proves the server speaks this protocol before anything else is attempted. */
    fun protocol(): Int {
        val answer = ok(send("GET", "/healthz"), 200)
        val value = answer.json().optInt("protocol", 0)
        if (value != SyncPolicyV4.PROTOCOL) {
            throw Failure(
                "This server speaks sync protocol $value; this app needs protocol " +
                    "${SyncPolicyV4.PROTOCOL} (accounts, vaults and versioned records).",
            )
        }
        return value
    }

    /** Whether the relay still accepts self-service signup, so the UI can offer it. */
    fun registrationOpen(): Boolean {
        val answer = ok(send("GET", "/healthz"), 200)
        return answer.json().optString("registration", "open") == "open"
    }

    /** Signs in and remembers the session cookie that vault listing needs. */
    fun login(email: String, password: String): JSONObject {
        val body = JSONObject().put("email", email).put("password", password)
        val answer = send("POST", "/auth/login", body.toString().toByteArray(Charsets.UTF_8))
        if (answer.status == 401) throw Failure("The account email or password is wrong.", 401)
        ok(answer, 200)
        val raw = answer.header("set-cookie")
            ?: throw Failure("The server did not start a session.")
        val marker = "mv_session="
        val start = raw.indexOf(marker)
        if (start < 0) throw Failure("The server did not start a session.")
        val end = raw.indexOf(';', start)
        cookie = raw.substring(start + marker.length, if (end < 0) raw.length else end)
        return answer.json()
    }

    /** Creates the account on a relay that still takes signups. */
    fun register(email: String, password: String): JSONObject {
        val body = JSONObject().put("email", email).put("password", password)
        val answer = send("POST", "/auth/register", body.toString().toByteArray(Charsets.UTF_8))
        if (answer.status == 403) {
            throw Failure("This server does not accept new accounts; ask the operator to create one.", 403)
        }
        return ok(answer, 200, 201).json()
    }

    /** Issues a device token for this phone; the value is only shown once. */
    fun issueToken(label: String): String {
        val body = JSONObject().put("label", label.ifBlank { "phone" })
        val answer = send("POST", "/auth/token", body.toString().toByteArray(Charsets.UTF_8))
        if (answer.status == 401) {
            throw Failure("The sign-in session expired before a device token could be issued.", 401)
        }
        ok(answer, 201)
        val value = answer.json().optString("token", "")
        if (!SyncPolicy.isHex(value, 64)) throw Failure("The server returned a malformed device token.")
        token = value
        return value
    }

    fun changePassword(current: String, next: String): JSONObject {
        val body = JSONObject().put("current_password", current).put("new_password", next)
        return ok(send("POST", "/auth/password", body.toString().toByteArray(Charsets.UTF_8)), 200).json()
    }

    fun vaults(): JSONObject = ok(send("GET", "/v1/vaults"), 200).json()

    // ── records ──────────────────────────────────────────────────────────

    private fun recordsPath(vault: String, id: String? = null): String {
        val base = "/v1/vaults/${SyncPolicyV4.segment(vault)}/records"
        return if (id.isNullOrEmpty()) base else "$base/${SyncPolicyV4.segment(id)}"
    }

    private fun blobPath(vault: String, packageName: String): String =
        "/v1/vaults/${SyncPolicyV4.segment(vault)}/blobs/${SyncPolicyV4.segment(packageName)}"

    fun records(vault: String, since: Long = 0, limit: Int = 500): Page {
        val target = recordsPath(vault) + "?since=${maxOf(0L, since)}&limit=${limit.coerceIn(1, 1000)}"
        val body = ok(send("GET", target), 200).json()
        val array = body.optJSONArray("records") ?: throw Failure("The server returned an invalid record list.")
        if (array.length() > 1000) throw Failure("The server returned more records than one page may hold.")
        val listed = ArrayList<SyncRecord>(array.length())
        for (index in 0 until array.length()) {
            val record = SyncRecord.fromJson(array.getJSONObject(index))
            if (!SyncPolicyV4.isRecordId(record.id)) throw Failure("The server listed an invalid record id.")
            if (record.size < 0 || record.size > SyncPolicyV4.MAX_PACKAGE) {
                throw Failure("The server listed an invalid package size.")
            }
            listed.add(record)
        }
        return Page(listed, body.optDouble("cursor", 0.0).toLong())
    }

    /**
     * Writes one record. `baseVersion` 0 means "create"; anything else must match
     * what the server holds, or it answers 409 and this throws [Conflict].
     */
    fun write(vault: String, record: SyncRecord, baseVersion: Int, operation: String): SyncRecord {
        val body = JSONObject()
            .put("base_version", baseVersion)
            .put("operation", operation)
            .put("name", record.name)
            .put("path", record.path)
            .put("tags", JSONArray(record.tags))
            .put("package", record.packageName ?: JSONObject.NULL)
            .put("size", record.size)
        val answer = send("PUT", recordsPath(vault, record.id), body.toString().toByteArray(Charsets.UTF_8))
        if (answer.status == 409) {
            val held = runCatching { answer.json().optJSONObject("record") }.getOrNull()
            throw Conflict(held?.let { SyncRecord.fromJson(it) })
        }
        ok(answer, 200, 201)
        val stored = answer.json().optJSONObject("record")
            ?: throw Failure("The server did not echo the record it stored.")
        return SyncRecord.fromJson(stored)
    }

    fun record(vault: String, id: String): SyncRecord {
        val answer = send("GET", recordsPath(vault, id))
        if (answer.status == 404) throw Failure("That item is no longer on the server.", 404)
        return SyncRecord.fromJson(ok(answer, 200).json().getJSONObject("record"))
    }

    fun restore(vault: String, id: String): SyncRecord {
        val answer = ok(send("POST", recordsPath(vault, id) + "/restore"), 200)
        return SyncRecord.fromJson(answer.json().getJSONObject("record"))
    }

    fun tombstone(vault: String, id: String): SyncRecord {
        val answer = send("DELETE", recordsPath(vault, id))
        if (answer.status == 204) return SyncRecord(id, "", "", emptyList(), null, 0, 0, true, 0)
        return SyncRecord.fromJson(ok(answer, 200).json().getJSONObject("record"))
    }

    fun purge(vault: String, id: String) {
        rejectOrDone(send("DELETE", recordsPath(vault, id) + "?purge=1"))
    }

    // ── blobs ────────────────────────────────────────────────────────────

    fun headBlob(vault: String, packageName: String): BlobState {
        if (!SyncPolicyV4.isPackage(packageName)) throw Failure("That is not a package address.")
        val answer = send("HEAD", blobPath(vault, packageName))
        if (answer.status == 404) return BlobState()
        ok(answer, 200)
        val size = answer.header("content-length")?.toLongOrNull() ?: -1
        val offset = answer.header("x-offset")?.toLongOrNull() ?: 0
        val completeHeader = answer.header("x-complete")
        val complete = completeHeader == "true" || (completeHeader == null && offset == size)
        if (size < 0 || size > SyncPolicyV4.MAX_PACKAGE) throw Failure("The server reported an invalid package size.")
        if (offset < 0 || offset > size) throw Failure("The server reported an invalid upload offset.")
        return BlobState(true, size, offset, complete)
    }

    /** Uploads a package, resuming from whatever the server already holds. */
    fun putBlob(vault: String, packageName: String, file: File, onProgress: ((Long, Long) -> Unit)? = null) {
        if (!SyncPolicyV4.isPackage(packageName)) throw Failure("The package does not hash to a valid address.")
        if (!file.isFile) throw Failure("Cannot read the exported package.")
        val size = file.length()
        if (size <= 0) throw Failure("The exported package is empty.")
        if (size > SyncPolicyV4.MAX_PACKAGE) throw Failure("The package is larger than this server accepts.")

        var offset = 0L
        val existing = headBlob(vault, packageName)
        if (existing.exists) {
            if (existing.size != size) throw Failure("The server holds a different package under this name.")
            if (existing.complete) {
                onProgress?.invoke(size, size)
                return
            }
            offset = existing.offset
        }

        var stale = 0
        file.inputStream().use { input ->
            while (offset < size) {
                input.channel.position(offset)
                val want = minOf(SyncPolicyV4.CHUNK.toLong(), size - offset).toInt()
                val chunk = ByteArray(want)
                var filled = 0
                while (filled < want) {
                    val read = input.read(chunk, filled, want - filled)
                    if (read < 0) break
                    filled += read
                }
                if (filled != want) throw Failure("Cannot read the exported package.")
                val answer = send(
                    "PUT",
                    blobPath(vault, packageName) + "?offset=$offset",
                    chunk,
                    mapOf("X-Total-Size" to size.toString()),
                )
                if (answer.status == 409) {
                    val reported = runCatching { answer.json().optDouble("offset", -1.0).toLong() }.getOrDefault(-1)
                    if (++stale > 4 || reported < 0 || reported > size) {
                        throw Failure("The server kept reporting a different upload offset.")
                    }
                    offset = reported
                    if (offset == size) break
                    continue
                }
                ok(answer, 200, 201)
                val body = answer.json()
                val next = body.optDouble("offset", -1.0).toLong()
                val complete = body.optBoolean("complete", false)
                if (next != offset + filled) throw Failure("The server acknowledged a different number of bytes.")
                offset = next
                if (offset == size && !complete) throw Failure("The server did not finish the package.")
                onProgress?.invoke(offset, size)
            }
        }
        onProgress?.invoke(size, size)
    }

    /** One byte range of a package; the relay answers 206 with a matching Content-Range. */
    fun getRange(vault: String, packageName: String, from: Long, to: Long, total: Long): ByteArray {
        if (!SyncPolicyV4.isPackage(packageName)) throw Failure("That is not a package address.")
        if (from < 0 || to < from) throw Failure("Invalid download range.")
        val answer = send(
            "GET",
            blobPath(vault, packageName),
            null,
            mapOf("Range" to "bytes=$from-$to"),
        )
        if (answer.status == 404) throw Failure("A package this vault lists is not on the server yet.", 404)
        if (answer.status == 416) throw Failure("The server refused the requested byte range.", 416)
        ok(answer, 206)
        val expected = "bytes $from-$to/$total"
        if (answer.header("content-range") != expected) {
            throw Failure("The server answered with an unexpected byte range.")
        }
        if (answer.bytes.size.toLong() != to - from + 1) {
            throw Failure("The download stopped before the range ended.")
        }
        return answer.bytes
    }

    fun deleteBlob(vault: String, packageName: String) {
        val answer = send("DELETE", blobPath(vault, packageName))
        if (answer.status == 404) return
        rejectOrDone(answer)
    }

    private fun rejectOrDone(answer: Response) {
        if (answer.status == 204 || answer.status == 200) return
        reject(answer.status)
    }
}
