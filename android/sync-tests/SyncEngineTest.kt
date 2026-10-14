package org.megavideoprotect.app

import java.io.File
import java.nio.file.Files
import org.json.JSONObject

private class MemoryStore(override val directory: File) : SyncStateStore {
    var encoded = "{}"
    override fun load() = JSONObject(encoded)
    override fun save(state: JSONObject) { encoded = state.toString() }
}
private class FakeVault : SyncVault {
    var unlocked = true
    var exportCount = 0
    override fun isUnlocked() = unlocked
    override fun files() = listOf(SyncLocalFile(1, "folder/file.txt", "content-and-metadata"))
    override fun export(file: SyncLocalFile, target: File, key: String) { exportCount++; target.writeText("encrypted-mvpk") }
    override fun import(source: File, key: String): Int = 0
}
fun main() = kotlinx.coroutines.runBlocking {
    val dir = Files.createTempDirectory("sync-test").toFile()
    try {
        val store = MemoryStore(dir)
        val vault = FakeVault()
        val engine = SyncEngine(store, vault)
        engine.configure("http://127.0.0.1:1", "", "a".repeat(64), "b".repeat(64))
        engine.stage(setOf(1))
        val first = store.load().getJSONArray("queue").getJSONObject(0).toString()
        engine.stage(setOf(1))
        check(vault.exportCount == 1)
        check(store.load().getJSONArray("queue").length() == 1)
        check(store.load().getJSONArray("queue").getJSONObject(0).toString() == first)
        val restarted = SyncEngine(store, vault)
        check(restarted.pendingCount() == 1)
        check(dir.listFiles()!!.any { it.extension == "mvpk" })
        vault.unlocked = false
        check(runCatching { restarted.stage(setOf(1)) }.isFailure)
        check(store.load().getJSONArray("queue").getJSONObject(0).toString() == first)
        vault.unlocked = true
        store.save(store.load().put("receipt", JSONObject().put("id", "c".repeat(32)).put("revision", 2).put("blob", "d".repeat(64))))
        check(runCatching { SyncEngine(store, vault).stage(setOf(1)) }.isFailure) // no echo while dedup mapping unresolved
        val historical = store.load()
        historical.remove("receipt")
        historical.put("queue", org.json.JSONArray())
        historical.put("retained", JSONObject().put("1", "content-and-metadata"))
        store.save(historical)
        SyncEngine(store, vault).stage(setOf(1))
        check(vault.exportCount == 1) { "Retained prior remote version must not echo-upload" }
        val cached = File(dir, "cached.download").apply { writeText("encrypted-mvpk") }
        val failedReceipt = JSONObject().put("id", "e".repeat(32)).put("revision", 3).put("blob", fileHash(cached)).put("size", cached.length()).put("phase", "failed")
        cached.renameTo(File(dir, "${failedReceipt.getString("blob")}.download"))
        store.save(store.load().put("receipt", JSONObject(failedReceipt.toString()).put("phase", "started")))
        check(runCatching { SyncEngine(store, vault).resolveReceipt(1) }.isFailure) { "Interrupted import must be retried successfully before acknowledging" }
        store.save(store.load().put("receipt", failedReceipt))
        val recoveryEngine = SyncEngine(store, vault)
        check(runCatching { recoveryEngine.resolveReceipt(1) }.isFailure) { "Failed imports must not advance seen revision" }
        kotlinx.coroutines.runBlocking { recoveryEngine.resumeImport() }
        check(recoveryEngine.receipt()!!.getString("phase") == "imported")
        check(recoveryEngine.receipt() != null) // dedup still needs explicit existing ID
        recoveryEngine.resolveReceipt(1)
        check(recoveryEngine.receipt() == null)
        val cancelJob = kotlinx.coroutines.Job()
        var staged = 0
        val cancellableVault = object : SyncVault {
            override fun isUnlocked() = true
            override fun files() = listOf(SyncLocalFile(3, "a", "a"), SyncLocalFile(4, "b", "b"))
            override fun export(file: SyncLocalFile, target: File, key: String) { staged++; target.writeText("ciphertext"); cancelJob.cancel() }
            override fun import(source: File, key: String) = 0
        }
        val cancelStore = MemoryStore(File(dir, "cancel").apply { mkdirs() })
        val cancelEngine = SyncEngine(cancelStore, cancellableVault)
        cancelEngine.configure("http://127.0.0.1:1", "", "a".repeat(64), "b".repeat(64))
        check(runCatching { kotlinx.coroutines.withContext(cancelJob) { cancelEngine.stage(setOf(3, 4)) } }.isFailure)
        check(staged == 1) { "Lifecycle cancellation must stop staging additional files" }
        check(cancelEngine.pendingCount() == 1) { "Already-staged ciphertext remains durable on cancellation" }
        println("SyncEngine: durable staging/restart, lock/dedup/retained guards, failed-import retry and staging cancellation passed")
    } finally { dir.deleteRecursively() }
}
