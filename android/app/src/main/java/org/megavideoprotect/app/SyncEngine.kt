package org.megavideoprotect.app

import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.security.SecureRandom
import org.json.JSONArray
import org.json.JSONObject

/** Where the private, already-encrypted sync directory and its ledger live. */
interface SyncStateStore {
    val directory: File
    fun load(): JSONObject
    fun save(state: JSONObject)
}

/** One local item as this device sees it. [signature] changes only when the item does. */
data class SyncLocalFile(val id: Long, val path: String, val signature: String)

/** The existing native (JNI) package format, one item per package. */
interface SyncVault {
    fun isUnlocked(): Boolean
    fun files(): List<SyncLocalFile>
    fun export(file: SyncLocalFile, target: File, key: String)
    fun import(source: File, key: String): Int
}

internal fun fileHash(file: File): String {
    val digest = java.security.MessageDigest.getInstance("SHA-256")
    file.inputStream().use { input ->
        val buffer = ByteArray(65536)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            digest.update(buffer, 0, count)
        }
    }
    return digest.digest().hex()
}

internal fun randomHex(bytes: Int): String =
    ByteArray(bytes).also { SecureRandom().nextBytes(it) }.hex()

/**
 * Protocol 3 hash-bucket client, mirroring qt-app/src/sync_engine.cpp.
 *
 * A pass: check /healthz says protocol 3, list the bucket (which also proves the
 * library key), export each checked item that changed to one .mvpk under that
 * key, PUT it named by the SHA-256 of its own bytes (a HEAD probe skips what the
 * server already has), then download every listed package this device has never
 * imported — resuming with Range and verifying size and digest before the
 * native import. A ledger of local id -> package name keeps unchanged items from
 * being re-exported and known packages from being re-imported. Deletion happens
 * only through [remove], one name at a time.
 */
class SyncEngine(private val store: SyncStateStore, private val vault: SyncVault) {
    private var state = store.load()
    private var catalog: List<JSONObject> = emptyList()
    private var server = ""
    private var libraryKey = ""
    private var busy = false

    private val seen = HashSet<String>()
    private val files: JSONObject get() = state.optJSONObject("files") ?: JSONObject().also { state.put("files", it) }
    private val stats = Stats()

    /** The last status line; on failure this is the message the user must see. */
    var status: String = "Enter the server URL and the library key, then press Sync."
        private set

    /** Set when the last operation failed; never loops or retries on its own. */
    var failure: String? = null
        private set

    /** True when a download was imported, so the gallery must be reloaded. */
    var libraryChanged = false
        private set

    init {
        val saved = state.optJSONArray("seen")
        if (saved != null) for (index in 0 until saved.length()) {
            val name = saved.optString(index)
            if (SyncPolicy.isPackageName(name)) seen.add(name)
        }
        if (savedServer().isNotEmpty() && savedKey().isNotEmpty()) {
            status = "Saved server and library key loaded. Press Sync."
        }
    }

    fun configured(): Boolean = savedServer().isNotEmpty() && savedKey().isNotEmpty()

    fun savedServer(): String = state.optString("server")

    /** Stored only inside the Keystore-sealed (AES-GCM) private ledger, never in plaintext. */
    fun savedKey(): String = state.optString("key")

    fun sharedCount(): Int = files.length()

    fun knownCount(): Int = seen.size

    fun packages(): List<JSONObject> = catalog.toList()

    fun isKnown(name: String): Boolean = seen.contains(name)

    fun selectedIds(): Set<Long> {
        val array = state.optJSONArray("selected") ?: return emptySet()
        val out = LinkedHashSet<Long>()
        for (index in 0 until array.length()) {
            val id = array.optLong(index, -1L)
            if (id > 0L) out.add(id)
        }
        return out
    }

    fun select(ids: Set<Long>) {
        if (busy) return
        val clean = ids.filter { it > 0L }.distinct().sorted()
        val array = JSONArray().also { target -> clean.forEach { target.put(it) } }
        val previous = state.optJSONArray("selected")
        if (previous != null && previous.toString() == array.toString()) return
        state.put("selected", array)
        persist()
    }

    /**
     * Validates and remembers the settings for this pass. Nothing is written
     * here: a mistyped key must stay correctable, so the ledger is only adopted
     * after the server has accepted the key.
     */
    fun configure(serverUrl: String, keyText: String): Boolean {
        if (busy) return false
        val url = serverUrl.trim().trimEnd('/')
        val key = keyText.trim()
        if (!SyncPolicy.validServer(url)) {
            reject(
                "Enter the server URL, for example https://vault.example:8766. " +
                    "Plain HTTP is accepted only for 127.0.0.1.",
            )
            return false
        }
        if (!SyncPolicy.isHex(key, 64)) {
            reject("The library key is 64 lowercase hex characters.")
            return false
        }
        server = url
        libraryKey = key
        failure = null
        status = "Ready. Press Sync to upload the checked items and receive new packages."
        return true
    }

    fun list() = run("Reading the server's package list...") {
        requireSettings()
        ensureDirectory()
        catalog = listPackages(SyncHttp(server, libraryKey))
        status = "${catalog.size} package(s) on the server."
    }

    fun syncNow() = run("Syncing...") {
        requireSettings()
        check(vault.isUnlocked()) { "Unlock a local vault first." }
        ensureDirectory()
        val client = SyncHttp(server, libraryKey)

        val health = client.request("GET", "/healthz")
        val protocol = runCatching {
            JSONObject(String(health.bytes, Charsets.UTF_8)).optInt("protocol", 0)
        }.getOrDefault(0)
        if (protocol != 3) {
            error("This server speaks an older sync protocol; start a protocol 3 sync_server.")
        }

        catalog = listPackages(client) // authenticated: a wrong key fails here, once
        adoptScope()
        stats.reset()
        libraryChanged = false
        uploadChecked(client)
        catalog = listPackages(client)
        downloadNew(client)
        status = summary()
    }

    /** Explicit, one name at a time; there is no delete propagation in either direction. */
    fun remove(packageName: String) = run("Deleting a package...") {
        require(SyncPolicy.isPackageName(packageName)) { "That is not a package name." }
        requireSettings()
        val client = SyncHttp(server, libraryKey)
        val missing = try {
            client.request("DELETE", "/v3/files/$packageName")
            false
        } catch (failure: SyncHttp.Failure) {
            if (failure.status != 404) throw failure
            true
        }
        // Forget it locally: the package is gone from the bucket, so the local
        // item is no longer shared. Local media is never touched.
        val ids = files.keys().asSequence().toList()
        for (id in ids) {
            if (files.getJSONObject(id).optString("remote") == packageName) files.remove(id)
        }
        seen.remove(packageName)
        persist()
        catalog = catalog.filter { it.optString("name") != packageName }
        status = if (missing) {
            "Package ${shortName(packageName)} was not on the server; its local items are no longer shared."
        } else {
            "Package ${shortName(packageName)} was deleted from the server; its local items are no longer shared."
        }
    }

    // -- one pass ---------------------------------------------------------

    private fun uploadChecked(client: SyncHttp) {
        val selected = selectedIds()
        if (selected.isEmpty()) return
        val local = vault.files()
        for (file in local) {
            if (file.id !in selected) continue
            val known = files.optJSONObject(file.id.toString())
            if (known != null && SyncPolicy.isPackageName(known.optString("remote")) &&
                known.optString("source") == file.signature && known.optString("path") == file.path
            ) {
                stats.unchanged++
                continue
            }
            var name: String? = null
            var uploaded = false
            val staged = File(store.directory, "out-${randomHex(8)}.mvpk")
            try {
                vault.export(file, staged, libraryKey)
                // The native writer closes first; force the bytes durable before
                // the name they hash to is published in the ledger.
                FileOutputStream(staged, true).use { it.fd.sync() }
                if (staged.length() <= 0L) error("The exported package is empty.")
                stats.exported++
                name = SyncPolicy.packageName(fileHash(staged))
                putFile(client, name, staged)
                uploaded = true
                files.put(
                    file.id.toString(),
                    JSONObject().put("remote", name).put("source", file.signature).put("path", file.path),
                )
                seen.add(name)
                persist()
            } catch (error: Exception) {
                // Nothing is recorded, so the next pass exports and sends it
                // again; a half-uploaded package must not keep server quota.
                val partial = name
                if (partial != null && !uploaded) {
                    runCatching { client.request("DELETE", "/v3/files/$partial") }
                }
                throw error
            } finally {
                staged.delete()
            }
        }
    }

    private fun putFile(client: SyncHttp, name: String, file: File) {
        require(SyncPolicy.isPackageName(name))
        val size = file.length()
        if (size <= 0L) error("The exported package is empty.")

        var offset = 0L
        val head = try {
            client.request("HEAD", "/v3/files/$name")
        } catch (failure: SyncHttp.Failure) {
            if (failure.status != 404) throw failure
            null
        }
        if (head != null) {
            if (head.header("X-Complete") == "true") {
                stats.present++ // already there: no bytes are sent at all
                return
            }
            offset = head.header("X-Offset")?.toLongOrNull() ?: 0L
            if (offset !in 0L..size) offset = 0L
        }

        var stale = 0
        RandomAccessFile(file, "r").use { input ->
            while (offset < size) {
                val chunk = ByteArray(minOf(size - offset, SyncPolicy.CHUNK.toLong()).toInt())
                input.seek(offset)
                input.readFully(chunk)
                val response = try {
                    client.request(
                        "PUT",
                        "/v3/files/$name?offset=$offset",
                        chunk,
                        mapOf("X-Total-Size" to size.toString()),
                    )
                } catch (failure: SyncHttp.Failure) {
                    if (failure.status != 409) throw failure
                    val reported = failure.payload?.optLong("offset", -1L) ?: -1L
                    if (++stale > 4 || reported !in 0L..size) {
                        error("The server kept reporting a different upload offset.")
                    }
                    offset = reported
                    continue
                }
                val body = runCatching { JSONObject(String(response.bytes, Charsets.UTF_8)) }.getOrNull()
                    ?: error("The server returned invalid JSON.")
                // 200 on the first chunk means the server already owned a
                // complete package under this name. The server also answers 200
                // for every later chunk, so only offset 0 counts as "present".
                if (response.status == 200 && offset == 0L && body.optBoolean("complete", false)) {
                    stats.present++
                    return
                }
                val next = body.optLong("offset", -1L)
                if (next != offset + chunk.size) error("The server acknowledged a different number of bytes.")
                offset = next
                if (offset == size && !body.optBoolean("complete", false)) {
                    error("The server did not finish the package.")
                }
            }
        }
        stats.uploaded++
    }

    private fun downloadNew(client: SyncHttp) {
        for (entry in catalog) {
            val name = entry.getString("name")
            if (seen.contains(name)) continue
            fetch(client, name)
        }
    }

    private fun fetch(client: SyncHttp, name: String) {
        if (!SyncPolicy.isPackageName(name)) error("The server listed an invalid package name.")
        val head = try {
            client.request("HEAD", "/v3/files/$name")
        } catch (failure: SyncHttp.Failure) {
            if (failure.status == 404) error("A package the server listed is no longer there; sync again.")
            throw failure
        }
        val size = head.header("Content-Length")?.toLongOrNull() ?: -1L
        if (size <= 0L || size > SyncPolicy.MAX_PACKAGE) error("The server reported an invalid package size.")
        if (head.header("X-Complete") != "true") {
            stats.pending++ // another device is still uploading it; never import a partial
            return
        }

        val partial = File(store.directory, "dl-$name")
        RandomAccessFile(partial, "rw").use { output ->
            var offset = output.length()
            if (offset > size) {
                output.setLength(0)
                offset = 0L
            }
            while (offset < size) {
                val last = minOf(offset + SyncPolicy.CHUNK - 1, size - 1)
                val response = client.request(
                    "GET",
                    "/v3/files/$name",
                    headers = mapOf("Range" to "bytes=$offset-$last"),
                )
                if (response.status != 206) error("The server answered a range request without a partial response.")
                if (response.header("Content-Range") != "bytes $offset-$last/$size") {
                    error("The server answered with an unexpected byte range.")
                }
                if (response.bytes.size.toLong() != last - offset + 1) {
                    error("The download stopped before the range ended.")
                }
                output.seek(offset)
                output.write(response.bytes)
                output.fd.sync()
                offset = last + 1
            }
        }

        if (partial.length() != size || fileHash(partial) != name.substring(0, 64)) {
            partial.delete()
            error("A downloaded package does not match its name; it was discarded and will be fetched again.")
        }
        stats.downloaded++
        vault.import(partial, libraryKey) // size and digest verified before any decrypt
        stats.imported++
        libraryChanged = true
        seen.add(name)
        persist()
        partial.delete()
    }

    // -- settings and ledger ----------------------------------------------

    private fun adoptScope() {
        // After the server has accepted the key: a different key or server is a
        // different bucket, so the old local-id mappings would be misleading.
        if (state.optString("key") != libraryKey || state.optString("server") != server) {
            state.put("files", JSONObject())
            seen.clear()
        }
        state.put("server", server)
        state.put("key", libraryKey)
        persist()
    }

    private fun listPackages(client: SyncHttp): List<JSONObject> {
        val response = client.request("GET", "/v3/files")
        val body = runCatching { JSONObject(String(response.bytes, Charsets.UTF_8)) }.getOrNull()
            ?: error("The server returned invalid JSON.")
        val array = body.optJSONArray("files") ?: error("The server returned an invalid file list.")
        if (array.length() > SyncPolicy.MAX_PACKAGES) error("The server's file list is too large.")
        val out = ArrayList<JSONObject>(array.length())
        for (index in 0 until array.length()) {
            val entry = array.opt(index)
            if (entry !is JSONObject) error("The server returned an invalid file list.")
            val name = entry.opt("name")
            val size = entry.opt("size")
            val seq = entry.opt("seq")
            if (name !is String || !SyncPolicy.isPackageName(name)) {
                error("The server listed an invalid package name.")
            }
            if (size !is Number || size.toLong() <= 0L || size.toLong() > SyncPolicy.MAX_PACKAGE) {
                error("The server listed an invalid package size.")
            }
            if (seq !is Number) error("The server listed an invalid package sequence.")
            out.add(JSONObject().put("name", name).put("size", size.toLong()).put("seq", seq.toLong()))
        }
        return out
    }

    private fun persist() {
        state.put("version", 3)
        if (libraryKey.isNotEmpty()) {
            state.put("server", server)
            state.put("key", libraryKey)
        }
        state.put("files", files)
        val array = JSONArray().also { target -> seen.sorted().forEach { target.put(it) } }
        state.put("seen", array)
        store.save(state)
    }

    private fun requireSettings() {
        if (server.isEmpty() || libraryKey.isEmpty()) error("Enter the server URL and the library key first.")
    }

    private fun ensureDirectory() {
        val directory = store.directory
        if (!directory.isDirectory && !directory.mkdirs()) error("Cannot create the private sync directory.")
    }

    private fun reject(message: String) {
        failure = message
        status = message
    }

    private fun run(label: String, action: () -> Unit) {
        if (busy) return
        busy = true
        failure = null
        status = label
        try {
            action()
        } catch (error: Exception) {
            failure = error.message ?: "Sync failed."
            status = failure!!
            throw error
        } finally {
            busy = false
        }
    }

    private fun summary(): String {
        if (stats.exported + stats.uploaded + stats.present + stats.unchanged + stats.imported + stats.pending == 0) {
            return "Nothing to do: no item is shared and the server has no new package."
        }
        var text = "Sync complete: ${stats.uploaded} uploaded, ${stats.present} already on the server, " +
            "${stats.unchanged} unchanged, ${stats.imported} imported."
        if (stats.pending > 0) text += " ${stats.pending} package(s) still uploading from another device."
        return text
    }

    private fun shortName(name: String): String = name.take(12) + "..."

    private class Stats {
        var exported = 0
        var uploaded = 0
        var present = 0
        var unchanged = 0
        var imported = 0
        var downloaded = 0
        var pending = 0

        fun reset() {
            exported = 0
            uploaded = 0
            present = 0
            unchanged = 0
            imported = 0
            downloaded = 0
            pending = 0
        }
    }
}
