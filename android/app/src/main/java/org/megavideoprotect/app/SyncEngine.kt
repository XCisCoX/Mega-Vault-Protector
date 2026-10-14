package org.megavideoprotect.app

import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.UUID
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.delay
import org.json.JSONArray
import org.json.JSONObject

interface SyncStateStore {
    val directory: File
    fun load(): JSONObject
    fun save(state: JSONObject)
}
data class SyncLocalFile(val id: Long, val path: String, val signature: String)
interface SyncVault {
    fun isUnlocked(): Boolean
    fun files(): List<SyncLocalFile>
    fun export(file: SyncLocalFile, target: File, key: String)
    fun import(source: File, key: String): Int
}
internal fun randomSyncId() = UUID.randomUUID().toString().replace("-", "")
internal fun fileHash(file: File): String {
    val digest = MessageDigest.getInstance("SHA-256")
    file.inputStream().use { input ->
        val buffer = ByteArray(65536)
        while (true) { val n = input.read(buffer); if (n < 0) break; digest.update(buffer, 0, n) }
    }
    return digest.digest().hex()
}

/** One owner, serialized by SyncScreen's mutex. No plaintext media in this store/cache. */
class SyncEngine(private val store: SyncStateStore, private val vault: SyncVault) {
    private var state = store.load()
    private val queue: JSONArray get() = state.optJSONArray("queue") ?: JSONArray().also { state.put("queue", it) }
    private val known: JSONObject get() = state.optJSONObject("known") ?: JSONObject().also { state.put("known", it) }
    private fun save() = store.save(state)
    fun configured() = state.optString("endpoint").isNotEmpty()
    fun endpoint() = state.optString("endpoint")
    fun pin() = state.optString("pin")
    fun pendingCount() = queue.length()
    fun receipt(): JSONObject? = state.optJSONObject("receipt")
    fun status(): String = when {
        state.optBoolean("unauthorized") -> "Re-pair required; pending work preserved"
        receipt() != null -> "Import needs local mapping confirmation; uploads paused"
        state.optString("error").isNotEmpty() -> state.getString("error") + " — Retry available"
        pendingCount() > 0 -> "${pendingCount()} encrypted package(s) pending"
        !configured() -> "Not paired"
        else -> "No pending uploads; only selected remote objects are downloaded"
    }
    fun configure(endpoint: String, pin: String, key: String, token: String) {
        require(SyncPolicy.validEndpoint(endpoint) && SyncPolicy.validSecret(key) && SyncPolicy.validSecret(token))
        require(pin.isEmpty() || SyncPolicy.validSecret(pin))
        check(!configured() || (endpoint == this.endpoint() && key == state.getString("key"))) { "Existing sync belongs to another server/key. Keep its state; do not replace it." }
        state.put("endpoint", endpoint).put("pin", pin).put("key", key).put("token", token).put("unauthorized", false).remove("error")
        save()
    }
    fun rePair(token: String) {
        require(SyncPolicy.validSecret(token)); state.put("token", token).put("unauthorized", false).remove("error"); save()
    }
    fun retry() {
        state.put("attempt", 0).put("retryAt", 0L).remove("error"); save()
    }
    fun selectDownloads(ids: Set<String>) {
        require(ids.all(SyncPolicy::validId)); state.put("selected", JSONArray(ids.sorted())); save()
    }
    fun selectedDownloads(): Set<String> {
        val arr = state.optJSONArray("selected") ?: return emptySet()
        return (0 until arr.length()).map { arr.getString(it) }.toSet()
    }
    fun catalog(): List<JSONObject> {
        val arr = state.optJSONArray("catalog") ?: return emptyList()
        return (0 until arr.length()).map { arr.getJSONObject(it) }
    }
    private fun unlocked() { check(vault.isUnlocked()) { "Unlock the local vault to sync" } }
    suspend fun stage(ids: Set<Long>) {
        unlocked(); check(configured()) { "Pair first" }
        check(receipt() == null) { "Resolve imported file mapping before uploading" }
        val files = vault.files()
        for (file in files.filter { it.id in ids }) {
            currentCoroutineContext().ensureActive(); unlocked()
            if (state.optJSONObject("retained")?.optString(file.id.toString()) == file.signature) continue
            if ((0 until queue.length()).any { queue.getJSONObject(it).getLong("local") == file.id }) continue
            val mappings = known.keys().asSequence().toList().sorted().map { known.getJSONObject(it) }.filter { it.optLong("local", -1) == file.id }
            if (mappings.any { it.optString("signature") == file.signature }) continue
            val mapping = mappings.firstOrNull()
            val id = mapping?.getString("id") ?: randomSyncId()
            val operation = randomSyncId()
            val target = File(store.directory, "$operation.mvpk")
            vault.export(file, target, state.getString("key"))
            // The native writer closes first; force bytes durable before publishing queue metadata.
            FileOutputStream(target, true).use { it.fd.sync() }
            val entry = JSONObject().put("id", id).put("local", file.id).put("signature", file.signature)
                .put("base_revision", mapping?.getLong("revision") ?: 0).put("operation_id", operation)
                .put("blob", fileHash(target)).put("size", target.length()).put("file", target.name)
            queue.put(entry); save()
        }
    }
    private fun api() = SyncHttp(endpoint(), state.getString("token"), pin())
    suspend fun refresh() {
        unlocked(); currentCoroutineContext().ensureActive()
        val records = api().json("GET", "/v2/objects").getJSONArray("objects")
        check(records.length() <= 10000) { "Catalog exceeds protocol limit" }
        val observed = state.optJSONObject("observed") ?: JSONObject()
        val ids = HashSet<String>()
        for (i in 0 until records.length()) {
            val record = records.getJSONObject(i)
            validateRecord(record)
            val id = record.getString("id")
            check(ids.add(id)) { "Duplicate manifest object" }
            val minimum = maxOf(known.optJSONObject(id)?.optLong("revision") ?: 0L, observed.optLong(id))
            check(record.getLong("revision") >= minimum) { "Server rollback rejected" }
        }
        for (i in 0 until records.length()) {
            val record = records.getJSONObject(i)
            observed.put(record.getString("id"), record.getLong("revision"))
        }
        state.put("observed", observed)
        state.put("catalog", records); save()
    }
    private fun validateRecord(record: JSONObject) {
        require(listOf("revision", "size", "sequence").all { record.get(it) is Int || record.get(it) is Long }) { "Protocol requires JSON integers" }
        require(record.get("deleted") is Boolean) { "Protocol requires a JSON boolean" }
        require(SyncPolicy.validId(record.getString("id")) && record.getLong("revision") > 0 && record.getLong("size") >= 0 && record.getLong("sequence") > 0)
        if (!record.getBoolean("deleted")) require(SyncPolicy.validSecret(record.getString("blob")))
        else require(record.getString("blob").isEmpty() && record.getLong("size") == 0L)
    }
    private suspend fun pace(bytes: Int) {
        val speed = state.optLong("speed", 0)
        if (speed > 0) delay((bytes.toLong() * 1000 / speed).coerceAtMost(30_000))
    }
    fun setSpeed(bytesPerSecond: Long) { require(bytesPerSecond >= 0); state.put("speed", bytesPerSecond); save() }
    /** A bounded pass. Caller retries only while this unlocked screen is STARTED. */
    suspend fun pass() {
        unlocked()
        if (state.optBoolean("unauthorized") || receipt() != null || !configured()) return
        if (System.currentTimeMillis() < state.optLong("retryAt")) return
        try {
            refresh()
            val hadUploads = queue.length() > 0
            while (queue.length() > 0) {
                currentCoroutineContext().ensureActive(); unlocked()
                upload(queue.getJSONObject(0))
            }
            if (hadUploads) refresh() // do not compare a pre-commit manifest against our just-advanced mapping
            for (record in catalog().filter { it.getString("id") in selectedDownloads() }) {
                currentCoroutineContext().ensureActive(); unlocked()
                val old = known.optJSONObject(record.getString("id"))
                if (record.getLong("revision") < (old?.optLong("revision") ?: 0L)) error("Server rollback rejected")
                if (!SyncPolicy.needsDownload(old?.optLong("revision") ?: 0, record.getLong("revision"))) continue
                if (record.getBoolean("deleted")) {
                    // Notice only. Never remove a local file; no implicit propagation of local deletions.
                    state.put("notice", "Remote deletion ${record.getString("id")}; local copy retained. Acknowledge explicitly."); save()
                    continue
                }
                download(record)
                if (receipt() != null) break
            }
            state.put("attempt", 0).put("retryAt", System.currentTimeMillis() + 15_000).remove("error"); save()
        } catch (cancel: kotlinx.coroutines.CancellationException) { throw cancel }
        catch (error: Exception) {
            val attempt = state.optInt("attempt")
            state.put("attempt", attempt + 1).put("retryAt", System.currentTimeMillis() + SyncPolicy.retryMillis(attempt, java.security.SecureRandom().nextInt(1000)))
            state.put("error", error.message ?: "Sync failed")
            if (error is SyncHttp.Failure && error.status == 401) state.put("unauthorized", true)
            save()
        }
    }
    fun notice(): String = state.optString("notice")
    fun acknowledgeDelete(id: String) {
        val record = catalog().single { it.getString("id") == id && it.getBoolean("deleted") }
        val mapping = known.optJSONObject(id) ?: JSONObject().put("id", id)
        mapping.put("revision", record.getLong("revision")); known.put(id, mapping)
        state.remove("notice"); save()
    }
    private suspend fun upload(entry: JSONObject) {
        val file = File(store.directory, entry.getString("file"))
        check(file.length() == entry.getLong("size") && fileHash(file) == entry.getString("blob")) { "Pending package damaged; keep original local file" }
        val hash = entry.getString("blob")
        val size = entry.getLong("size")
        val client = api()
        var offset = try {
            val head = client.request("HEAD", "/v2/blobs/$hash")
            check(head.header("Content-Length")?.toLong() == size)
            head.header("X-Upload-Offset")!!.toLong()
        } catch (failure: SyncHttp.Failure) { if (failure.status == 404) 0L else throw failure }
        check(offset in 0..size)
        RandomAccessFile(file, "r").use { input ->
            while (offset < size) {
                currentCoroutineContext().ensureActive(); unlocked()
                val chunk = ByteArray(minOf(1024L * 1024, size - offset).toInt())
                input.seek(offset); input.readFully(chunk)
                val next = try {
                    client.jsonBytesPut(hash, offset, size, chunk)
                } catch (failure: SyncHttp.Failure) {
                    if (failure.status != 409) throw failure
                    val actual = failure.payload?.getLong("offset") ?: throw failure
                    check(actual in 0..size && actual != offset) { "Upload offset stalled" }
                    offset = actual; continue
                }
                val actual = next.getLong("offset")
                check(actual > offset && actual <= size) { "Invalid upload offset" }
                offset = actual; pace(chunk.size)
            }
        }
        val payload = JSONObject().put("base_revision", entry.getLong("base_revision")).put("blob", hash)
            .put("size", size).put("deleted", false).put("operation_id", entry.getString("operation_id"))
        val committed = try { client.json("POST", "/v2/objects/${entry.getString("id")}", payload) }
        catch (failure: SyncHttp.Failure) {
            if (failure.status != 409) throw failure
            // Preserve the original request and encrypted bytes, fork exactly once durably. Never overwrite current revision.
            entry.put("conflict_original", JSONObject(entry.toString())).put("id", randomSyncId())
                .put("operation_id", randomSyncId()).put("base_revision", 0)
            state.put("notice", "CAS conflict: edited copy queued under a new object; both versions retained")
            save(); throw java.io.IOException("Conflict copy pending")
        }
        validateRecord(committed)
        check(committed.getString("id") == entry.getString("id") && committed.getString("blob") == hash && committed.getLong("size") == size)
        known.put(entry.getString("id"), JSONObject().put("id", entry.getString("id")).put("local", entry.getLong("local"))
            .put("revision", committed.getLong("revision")).put("signature", entry.getString("signature")).put("blob", hash))
        queue.remove(0); save(); file.delete()
    }
    private suspend fun download(record: JSONObject) {
        val hash = record.getString("blob")
        val size = record.getLong("size")
        val target = File(store.directory, "$hash.download")
        if (target.length() > size) check(target.delete())
        RandomAccessFile(target, "rw").use { output ->
            var offset = output.length()
            while (offset < size) {
                currentCoroutineContext().ensureActive(); unlocked()
                val end = minOf(size - 1, offset + 1024 * 1024 - 1)
                val response = api().request("GET", "/v2/blobs/$hash", headers = mapOf("Range" to "bytes=$offset-$end"))
                check(response.status == 206 && response.header("Content-Range") == "bytes $offset-$end/$size" && response.bytes.size.toLong() == end - offset + 1) { "Invalid ranged download" }
                output.seek(offset); output.write(response.bytes); output.fd.sync()
                offset += response.bytes.size; pace(response.bytes.size)
            }
        }
        if (target.length() != size || fileHash(target) != hash) {
            target.delete(); error("Downloaded package hash/size mismatch; original local files untouched")
        }
        unlocked()
        state.put("receipt", JSONObject(record.toString()).put("phase", "started")); save()
        resumeImport()
    }
    /** Explicit retry after interruption/failure; encrypted cached bytes are kept, never delete local originals. */
    suspend fun resumeImport() {
        unlocked(); currentCoroutineContext().ensureActive()
        val receipt = receipt() ?: error("No pending import")
        val target = File(store.directory, "${receipt.getString("blob")}.download")
        check(target.length() == receipt.getLong("size") && fileHash(target) == receipt.getString("blob")) { "Cached encrypted package damaged; download selection must be retried" }
        val before = vault.files().map { it.id }.toSet()
        receipt.put("before", JSONArray(before.sorted())).put("phase", "started"); save()
        val importedCount = try { vault.import(target, state.getString("key")) }
        catch (failure: Exception) { receipt.put("phase", "failed"); save(); throw failure }
        receipt.put("phase", "imported"); save()
        val files = vault.files()
        val id = if (importedCount == 1) SyncPolicy.importedId(before, files.map { it.id }.toSet()) else null
        if (id != null) resolveReceipt(id)
        else { state.put("notice", "Native import returned no unambiguous file ID (dedup or concurrent import). Choose the existing local file to link; no automatic echo upload."); save() }
    }
    /** Correct a mistyped initial secret, NOT key rotation. Already acknowledged/queued data is never silently rekeyed. */
    fun correctInitialRecoveryKey(key: String) {
        require(SyncPolicy.validSecret(key))
        check(queue.length() == 0 && known.length() == 0) { "Cannot change key after staging/acknowledged sync; rotation requires re-encryption" }
        state.put("key", key); save()
    }
    /** Explicit mapping also recovers crashes after JNI import, which returns count, not IDs. */
    fun resolveReceipt(localId: Long) {
        unlocked()
        val receipt = receipt() ?: error("No pending import")
        check(receipt.optString("phase") == "imported") { "Retry import successfully before linking a local file" }
        val file = vault.files().single { it.id == localId }
        val id = receipt.getString("id")
        val old = known.optJSONObject(id)
        if (old != null && old.optLong("local", -1) != localId && old.has("signature")) {
            val retained = state.optJSONObject("retained") ?: JSONObject().also { state.put("retained", it) }
            retained.put(old.getLong("local").toString(), old.getString("signature"))
        }
        known.put(id, JSONObject().put("id", id).put("local", localId).put("revision", receipt.getLong("revision"))
            .put("blob", receipt.getString("blob")).put("signature", file.signature))
        state.remove("receipt"); state.remove("error"); save()
        File(store.directory, "${receipt.getString("blob")}.download").delete()
    }
}
private fun SyncHttp.jsonBytesPut(hash: String, offset: Long, size: Long, bytes: ByteArray) =
    JSONObject(String(request("PUT", "/v2/blobs/$hash?offset=$offset", bytes, mapOf("X-Blob-Size" to size.toString())).bytes, Charsets.UTF_8))
