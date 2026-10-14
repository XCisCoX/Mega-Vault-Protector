package org.megavideoprotect.app

import com.sun.net.httpserver.HttpServer
import java.io.File
import java.net.InetSocketAddress
import java.nio.file.Files
import java.util.concurrent.Executors
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject

private class DiskLikeStore(override val directory: File) : SyncStateStore {
    var encoded = "{}"
    override fun load() = JSONObject(encoded)
    override fun save(state: JSONObject) { encoded = state.toString() }
}
private class TransferVault : SyncVault {
    val rows = mutableListOf(SyncLocalFile(1, "a/file", "source1"))
    var importCalls = 0
    var dedup = false
    var concurrentAddition = false
    val encryptedFixture = ByteArray(1024 * 1024 + 29) { (it % 251).toByte() }
    override fun isUnlocked() = true
    override fun files() = rows.toList()
    override fun export(file: SyncLocalFile, target: File, key: String) { target.writeBytes(encryptedFixture) }
    override fun import(source: File, key: String): Int {
        importCalls++
        if (concurrentAddition) rows.add(SyncLocalFile(rows.size.toLong() + 1, "unrelated", "unrelated"))
        if (dedup) return 0
        rows.add(SyncLocalFile(rows.size.toLong() + 1, "remote/file", "remote-$importCalls"))
        return 1
    }
}
private class RelayFixture {
    val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
    val objects = linkedMapOf<String, JSONObject>()
    val blobs = mutableMapOf<String, ByteArray>()
    val partials = mutableMapOf<String, ByteArray>()
    val operations = mutableMapOf<String, JSONObject>()
    val commits = mutableListOf<Pair<String, JSONObject>>()
    var conflictOnce = true
    var failPutOnce = true
    var loseCommitResponse = true
    var unauthorized = false
    var requests = 0
    val pool = Executors.newSingleThreadExecutor()
    val failures = mutableListOf<Throwable>()
    init {
        server.executor = pool
        server.createContext("/") { x ->
            try {
                requests++
                check(x.requestHeaders.getFirst("Authorization") == "Bearer " + "b".repeat(64))
                var status = 200
                var bytes = byteArrayOf()
                val path = x.requestURI.path
                when {
                    unauthorized -> status = 401
                    path == "/v2/objects" -> bytes = JSONObject().put("objects", JSONArray(objects.values.toList())).toString().toByteArray()
                    x.requestMethod == "HEAD" -> {
                        val hash = path.substringAfterLast('/')
                        val blob = blobs[hash]
                        val partial = partials[hash]
                        if (blob == null && partial == null) status = 404
                        else {
                            x.responseHeaders.add("Content-Length", (1024 * 1024 + 29).toString())
                            x.responseHeaders.add("X-Upload-Offset", (blob?.size ?: partial!!.size).toString())
                            x.responseHeaders.add("X-Blob-Complete", (blob != null).toString())
                        }
                    }
                    x.requestMethod == "PUT" -> {
                        val hash = path.substringAfterLast('/')
                        val offset = x.requestURI.query.substringAfter("offset=").toInt()
                        val size = x.requestHeaders.getFirst("X-Blob-Size").toInt()
                        val chunk = x.requestBody.readBytes()
                        check(chunk.size <= 1024 * 1024)
                        val old = partials[hash] ?: byteArrayOf()
                        check(offset == old.size)
                        val combined = old + chunk
                        partials[hash] = combined
                        if (combined.size == size) { check(sha256(combined) == hash); blobs[hash] = combined; partials.remove(hash) }
                        if (failPutOnce) { failPutOnce = false; status = 503 }
                        else bytes = JSONObject().put("offset", combined.size).put("complete", combined.size == size).toString().toByteArray()
                    }
                    x.requestMethod == "POST" -> {
                        val id = path.substringAfterLast('/')
                        val body = JSONObject(x.requestBody.bufferedReader().readText())
                        check(body.keySet() == setOf("base_revision", "blob", "size", "deleted", "operation_id"))
                        commits.add(id to JSONObject(body.toString()))
                        if (conflictOnce) { conflictOnce = false; status = 409; bytes = "{}".toByteArray() }
                        else {
                            val op = body.getString("operation_id")
                            val existing = operations[op]
                            if (existing != null) bytes = existing.toString().toByteArray()
                            else {
                                val record = JSONObject().put("id", id).put("revision", body.getLong("base_revision") + 1)
                                    .put("blob", body.getString("blob")).put("size", body.getLong("size")).put("deleted", false).put("sequence", objects.size + 1)
                                objects[id] = record; operations[op] = record; bytes = record.toString().toByteArray()
                            }
                            if (loseCommitResponse) { loseCommitResponse = false; status = 503 }
                        }
                    }
                    x.requestMethod == "GET" && path.startsWith("/v2/blobs/") -> {
                        val blob = blobs[path.substringAfterLast('/')]!!
                        val range = x.requestHeaders.getFirst("Range").removePrefix("bytes=").split('-').map { it.toInt() }
                        status = 206
                        bytes = blob.copyOfRange(range[0], range[1] + 1)
                        x.responseHeaders.add("Content-Range", "bytes ${range[0]}-${range[1]}/${blob.size}")
                    }
                    else -> error("Unexpected ${x.requestMethod} $path")
                }
                x.sendResponseHeaders(status, if (x.requestMethod == "HEAD") -1 else if (bytes.isEmpty()) -1 else bytes.size.toLong())
                if (x.requestMethod != "HEAD" && bytes.isNotEmpty()) x.responseBody.use { it.write(bytes) }
            } catch (failure: Throwable) { failures.add(failure); runCatching { x.sendResponseHeaders(500, -1) } }
            finally { x.close() }
        }
        server.start()
    }
    fun endpoint() = "http://127.0.0.1:${server.address.port}"
    fun close() { server.stop(0); pool.shutdownNow(); check(failures.isEmpty()) { failures.toString() } }
}
fun main() = runBlocking {
    val dir = Files.createTempDirectory("sync-transfer").toFile()
    val relay = RelayFixture()
    try {
        val vault = TransferVault()
        val store = DiskLikeStore(dir)
        var engine = SyncEngine(store, vault)
        engine.configure(relay.endpoint(), "", "a".repeat(64), "b".repeat(64))
        engine.stage(setOf(1))
        val initial = store.load().getJSONArray("queue").getJSONObject(0)
        val hash = initial.getString("blob")
        relay.partials[hash] = vault.encryptedFixture.copyOfRange(0, 17)
        engine.pass() // server durably appends, response lost
        check(engine.pendingCount() == 1)
        check(relay.partials[hash]!!.size == 17 + 1024 * 1024)
        engine = SyncEngine(store, vault); engine.retry(); engine.pass() // resumes; CAS conflict forks
        val fork = store.load().getJSONArray("queue").getJSONObject(0)
        check(fork.getString("id") != initial.getString("id"))
        check(fork.getString("operation_id") != initial.getString("operation_id"))
        check(fork.getLong("base_revision") == 0L)
        check(fork.getJSONObject("conflict_original").getString("id") == initial.getString("id"))
        check(fileHash(File(dir, fork.getString("file"))) == hash)
        engine.retry(); engine.pass() // commit succeeds but response lost
        check(engine.pendingCount() == 1)
        engine = SyncEngine(store, vault); engine.retry(); engine.pass() // same operation replay
        check(engine.pendingCount() == 0)
        check(relay.commits.takeLast(2).map { it.second.getString("operation_id") }.distinct().size == 1)
        engine.stage(setOf(1)); check(engine.pendingCount() == 0) // no unchanged echo
        val id = "f".repeat(32)
        relay.objects[id] = JSONObject().put("id", id).put("revision", 1).put("blob", hash).put("size", vault.encryptedFixture.size).put("deleted", false).put("sequence", 9)
        File(dir, "$hash.download").writeBytes(vault.encryptedFixture.copyOfRange(0, 5))
        engine.selectDownloads(setOf(id)); engine.retry(); engine.pass()
        check(vault.importCalls == 1 && engine.receipt() == null)
        engine.stage(vault.rows.map { it.id }.toSet()); check(engine.pendingCount() == 0)
        relay.objects[id]!!.put("revision", 2)
        engine.retry(); engine.pass()
        check(vault.importCalls == 2)
        engine.stage(vault.rows.map { it.id }.toSet()); check(engine.pendingCount() == 0) // old retained copy no echo
        relay.objects[id]!!.put("revision", 1)
        engine.selectDownloads(emptySet())
        check(runCatching { engine.refresh() }.isFailure) { "Manifest rollback must be rejected even when object isn't selected" }
        relay.objects[id]!!.put("revision", 3).put("deleted", true).put("blob", "").put("size", 0)
        val originalIds = vault.rows.map { it.id }.toSet()
        engine.selectDownloads(setOf(id)); engine.retry(); engine.pass()
        check(vault.rows.map { it.id }.toSet() == originalIds)
        check(store.load().getJSONObject("known").getJSONObject(id).getLong("revision") == 2L)
        engine.acknowledgeDelete(id)
        check(store.load().getJSONObject("known").getJSONObject(id).getLong("revision") == 3L)
        check(vault.rows.map { it.id }.toSet() == originalIds)
        relay.objects[id]!!.put("revision", 4).put("deleted", false).put("blob", hash).put("size", vault.encryptedFixture.size)
        vault.dedup = true; vault.concurrentAddition = true
        engine.retry(); engine.pass()
        check(engine.receipt() != null) { "Zero-count native dedup must not map a concurrent unrelated addition" }
        check(store.load().getJSONObject("known").getJSONObject(id).getLong("revision") == 3L)
        check(runCatching { engine.stage(setOf(1)) }.isFailure)
        engine.resolveReceipt(2)
        val imports = vault.importCalls
        engine.retry(); engine.pass(); check(vault.importCalls == imports)
        relay.unauthorized = true
        engine.retry(); engine.pass()
        check(store.load().getBoolean("unauthorized"))
        val requestCount = relay.requests
        engine.retry(); engine.pass(); check(relay.requests == requestCount) { "401 must stop automatic retries" }
        relay.unauthorized = false; engine.rePair("b".repeat(64))
        val invalidId = "d".repeat(32)
        val invalidHash = "0".repeat(64)
        relay.blobs[invalidHash] = vault.encryptedFixture
        relay.objects[invalidId] = JSONObject().put("id", invalidId).put("revision", 1).put("blob", invalidHash).put("size", vault.encryptedFixture.size).put("deleted", false).put("sequence", 10)
        engine.selectDownloads(setOf(invalidId)); engine.retry(); engine.pass()
        check(vault.importCalls == imports && engine.receipt() == null)
        check(!File(dir, "$invalidHash.download").exists())
        check(store.load().getString("error").contains("hash/size mismatch"))
        relay.objects[invalidId]!!.put("revision", "2")
        check(runCatching { engine.refresh() }.isFailure) { "Protocol JSON integers must not be accepted as strings" }
        println("SyncTransfer: resume, CAS/idempotency, mappings/no echo, rollback, tombstones, dedup race, 401, digest and strict JSON rejection passed")
    } finally { relay.close(); dir.deleteRecursively() }
}
