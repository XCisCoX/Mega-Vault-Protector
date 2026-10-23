package org.megavideoprotect.app

import java.io.File
import java.nio.file.Files
import org.json.JSONObject

private val KEY = "a".repeat(64)

private class EngineStore(override val directory: File) : SyncStateStore {
    var state = JSONObject()
    override fun load() = JSONObject(state.toString())
    override fun save(state: JSONObject) {
        this.state = JSONObject(state.toString())
    }
}

private class EngineVault(withItem: Boolean = true) : SyncVault {
    val rows = ArrayList<SyncLocalFile>().apply {
        if (withItem) add(SyncLocalFile(1, "folder/file.bin", "sig-1"))
    }
    var exportCount = 0
    var importCount = 0
    var lastKey = ""
    val fixture = ByteArray(1024 * 1024 + 29) { (it % 251).toByte() }

    override fun isUnlocked() = true
    override fun files() = rows.toList()
    override fun export(file: SyncLocalFile, target: File, key: String) {
        check(file.id > 0L)
        exportCount++
        lastKey = key
        target.writeBytes(fixture)
    }
    override fun import(source: File, key: String): Int {
        importCount++
        lastKey = key
        check(fileHash(source) == sha256(fixture)) { "Import must receive the verified package" }
        rows.add(SyncLocalFile((rows.maxOfOrNull { it.id } ?: 0L) + 1, "imported/$importCount.bin", fileHash(source)))
        return 1
    }
}

/**
 * The pass itself: one export per changed shared item, a PUT named by the
 * SHA-256 of its bytes, no echo upload, no re-import, one visible 401 with no
 * retry loop, protocol check, and deletion only when asked.
 */
fun main() {
    val dir = Files.createTempDirectory("sync-engine").toFile()
    val relay = V3Relay(KEY)
    try {
        val vault = EngineVault()
        val store = EngineStore(dir)
        val engine = SyncEngine(store, vault)

        check(!engine.configured())
        check(!engine.configure("http://vault.example:8766", KEY)) // plain HTTP only on loopback
        check(engine.status.contains("127.0.0.1"))
        check(!engine.configure(relay.endpoint(), "A".repeat(64))) // lowercase hex only
        check(engine.configure(relay.endpoint(), KEY))
        check(!engine.configured()) // nothing is saved before the server accepts the key
        check(engine.savedKey().isEmpty())

        engine.select(setOf(1))
        engine.syncNow()
        val name = SyncPolicy.packageName(sha256(vault.fixture))
        check(engine.packages().size == 1 && engine.packages()[0].getString("name") == name)
        check(relay.rows[name]?.complete == true)
        check(relay.rows[name]!!.bytes.contentEquals(vault.fixture))
        check(vault.exportCount == 1 && vault.importCount == 0)
        check(vault.lastKey == KEY) // the library key is what the JNI export encrypts under
        check(engine.configured() && engine.savedKey() == KEY && engine.savedServer() == relay.endpoint())
        check(engine.isKnown(name) && engine.sharedCount() == 1)
        check(engine.status.startsWith("Sync complete"))
        val ledger = store.state.getJSONObject("files").getJSONObject("1")
        check(ledger.getString("remote") == name && ledger.getString("source") == "sig-1")

        // Unchanged items are never exported or uploaded again.
        val puts = relay.putCount
        engine.syncNow()
        check(vault.exportCount == 1 && relay.putCount == puts && vault.importCount == 0)
        check(engine.status.contains("unchanged"))

        // The ledger survives a restart: still unchanged, the device's own package
        // is not downloaded back into itself, and the selection is remembered.
        val restarted = SyncEngine(store, vault)
        check(restarted.selectedIds() == setOf(1L))
        restarted.configure(relay.endpoint(), KEY)
        restarted.syncNow()
        check(vault.exportCount == 1 && vault.importCount == 0)
        check(restarted.isKnown(name))

        // A second device lists the bucket, downloads with ranges, verifies and
        // imports once; the known package is never imported twice.
        val secondVault = EngineVault(withItem = false)
        val second = SyncEngine(EngineStore(File(dir, "second").apply { mkdirs() }), secondVault)
        check(second.configure(relay.endpoint(), KEY))
        val rangesBefore = relay.rangeOffsets.size
        second.syncNow()
        check(secondVault.importCount == 1 && secondVault.rows.size == 1) { second.status }
        check(secondVault.lastKey == KEY)
        check(secondVault.rows[0].signature == name.substring(0, 64))
        check(relay.rangeOffsets.size - rangesBefore >= 2) // larger than one 1 MiB chunk
        check(second.isKnown(name) && second.status.contains("1 imported"))
        second.syncNow()
        check(secondVault.importCount == 1)

        // A wrong key: one authenticated attempt, the one message, ledger untouched.
        val wrong = "f".repeat(64)
        val requestsBefore = relay.requests
        val savedKey = second.savedKey()
        val shared = second.sharedCount()
        check(second.configure(relay.endpoint(), wrong))
        val rejected = runCatching { second.syncNow() }.exceptionOrNull()
        check(rejected?.message == "The server rejected this library key.") { rejected?.message ?: "no failure" }
        check(second.status == "The server rejected this library key.")
        check(relay.requests - requestsBefore == 2) { "expected health + one rejected listing, got ${relay.requests - requestsBefore}" }
        check(second.savedKey() == savedKey && second.sharedCount() == shared && second.isKnown(name))

        // Correcting the key reuses the ledger: nothing is downloaded again.
        check(second.configure(relay.endpoint(), KEY))
        second.syncNow()
        check(secondVault.importCount == 1)

        // Any protocol other than 3 is refused.
        relay.protocol = 2
        val old = runCatching { second.syncNow() }.exceptionOrNull()
        check(old?.message?.contains("protocol 3") == true)
        relay.protocol = 3

        // Deletion happens only on an explicit call, for one name, without
        // touching local media.
        second.remove(name)
        check(relay.rows.isEmpty() && !second.isKnown(name))
        check(secondVault.rows.size == 1)

        // A device whose ledger still says "shared and unchanged" does not
        // notice the deletion (the protocol has no tombstones); a device without
        // that mapping shares the item again on its next pass.
        restarted.syncNow()
        check(relay.completeCount() == 0) { "unchanged item must not silently re-upload" }
        val freshVault = EngineVault()
        val fresh = SyncEngine(EngineStore(File(dir, "fresh").apply { mkdirs() }), freshVault)
        check(fresh.configure(relay.endpoint(), KEY))
        fresh.select(setOf(1))
        fresh.syncNow()
        check(relay.completeCount() == 1) { fresh.status }
        check(relay.rows[name]?.complete == true)

        println("SyncEngine: export/PUT naming, unchanged skip, restart, import-once, wrong key (no retry), protocol and explicit delete passed")
    } finally {
        relay.close()
        dir.deleteRecursively()
    }
}
