package org.megavideoprotect.app

import java.io.File
import java.nio.file.Files
import org.json.JSONObject

private val KEY = "c".repeat(64)

private class TransferStore(override val directory: File) : SyncStateStore {
    private var state = JSONObject()
    override fun load() = JSONObject(state.toString())
    override fun save(state: JSONObject) {
        this.state = JSONObject(state.toString())
    }
}

private class TransferVault(withItem: Boolean = true) : SyncVault {
    val rows = ArrayList<SyncLocalFile>().apply {
        if (withItem) add(SyncLocalFile(1, "a/file", "source-1"))
    }
    var exportCount = 0
    var importCount = 0
    val fixture = ByteArray(1024 * 1024 + 29) { (it % 251).toByte() }

    override fun isUnlocked() = true
    override fun files() = rows.toList()
    override fun export(file: SyncLocalFile, target: File, key: String) {
        exportCount++
        target.writeBytes(fixture)
    }
    override fun import(source: File, key: String): Int {
        importCount++
        check(fileHash(source) == sha256(fixture))
        rows.add(SyncLocalFile((rows.maxOfOrNull { it.id } ?: 0L) + 1, "imported/$importCount", fileHash(source)))
        return 1
    }
}

private fun scenario(name: String, block: (V3Relay, File) -> Unit) {
    val root = Files.createTempDirectory("sync-$name").toFile()
    val relay = V3Relay(KEY)
    try {
        block(relay, root)
    } finally {
        relay.close()
        root.deleteRecursively()
    }
}

/**
 * Transfer edge cases against a real socket: upload resume from HEAD/409,
 * "200 at offset 0" as already present, a lost PUT answer, download resume from
 * a staged partial, digest rejection before import, and incomplete packages.
 */
fun main() {
    val probe = TransferVault()
    val name = SyncPolicy.packageName(sha256(probe.fixture))
    val total = probe.fixture.size.toLong()

    scenario("resume-up") { relay, root ->
        relay.addPartial(name, total, probe.fixture.copyOfRange(0, 1000))
        val vault = TransferVault()
        val engine = SyncEngine(TransferStore(File(root, "store").apply { mkdirs() }), vault)
        check(engine.configure(relay.endpoint(), KEY))
        engine.select(setOf(1))
        engine.syncNow()
        check(relay.putOffsets.first() == 1000L) { "upload must resume from X-Offset, got ${relay.putOffsets}" }
        check(0L !in relay.putOffsets)
        check(relay.rows[name]?.complete == true && relay.rows[name]!!.bytes.contentEquals(probe.fixture))
        check(vault.exportCount == 1)
    }

    scenario("stale-offset") { relay, root ->
        // The row appeared after our HEAD probe; the server answers 409 with the
        // durable offset and the client continues from there.
        relay.addPartial(name, total, probe.fixture.copyOfRange(0, 1000))
        relay.hidePartialsFromHead = true
        val vault = TransferVault()
        val engine = SyncEngine(TransferStore(File(root, "store").apply { mkdirs() }), vault)
        check(engine.configure(relay.endpoint(), KEY))
        engine.select(setOf(1))
        engine.syncNow()
        check(relay.putOffsets.take(2) == listOf(0L, 1000L)) { relay.putOffsets.toString() }
        check(relay.rows[name]?.complete == true)
    }

    scenario("present-at-zero") { relay, root ->
        // HEAD 404, then the server answers 200 {"complete":true} on the first
        // chunk: the package was already there under that name.
        relay.answerPresentAtOffsetZero = true
        val vault = TransferVault()
        val engine = SyncEngine(TransferStore(File(root, "store").apply { mkdirs() }), vault)
        check(engine.configure(relay.endpoint(), KEY))
        engine.select(setOf(1))
        engine.syncNow()
        check(engine.status.contains("1 already on the server")) { engine.status }
        check(relay.putCount == 1 && relay.rows.isEmpty())
        check(engine.isKnown(name))
    }

    scenario("already-there") { relay, root ->
        relay.addComplete(probe.fixture, name)
        val vault = TransferVault()
        val engine = SyncEngine(TransferStore(File(root, "store").apply { mkdirs() }), vault)
        check(engine.configure(relay.endpoint(), KEY))
        engine.select(setOf(1))
        engine.syncNow()
        check(engine.status.contains("1 already on the server"))
        check(relay.putCount == 0)
        check(relay.rows[name]!!.bytes.contentEquals(probe.fixture))
    }

    scenario("lost-answer") { relay, root ->
        relay.failPutAfterStoreOnce = true
        val vault = TransferVault()
        val engine = SyncEngine(TransferStore(File(root, "store").apply { mkdirs() }), vault)
        check(engine.configure(relay.endpoint(), KEY))
        engine.select(setOf(1))
        val failed = runCatching { engine.syncNow() }.exceptionOrNull()
        check(failed != null)
        // The unfinished/unrecorded package is removed again so it cannot hold quota.
        check(relay.rows.isEmpty()) { relay.rows.keys.toString() }
        check(!engine.isKnown(name))
        engine.syncNow() // re-export is a fresh attempt, not a retry loop
        check(relay.rows[name]?.complete == true)
        check(engine.isKnown(name))
    }

    scenario("download-resume") { relay, root ->
        relay.addComplete(probe.fixture, name)
        val vault = TransferVault(withItem = false)
        val directory = File(root, "store").apply { mkdirs() }
        File(directory, "dl-$name").writeBytes(probe.fixture.copyOfRange(0, 5000))
        val engine = SyncEngine(TransferStore(directory), vault)
        check(engine.configure(relay.endpoint(), KEY))
        engine.syncNow()
        check(relay.rangeOffsets.first() == 5000L) { relay.rangeOffsets.toString() }
        check(vault.importCount == 1)
        check(vault.rows.single().signature == name.substring(0, 64))
        check(!File(directory, "dl-$name").exists())
    }

    scenario("corrupt-download") { relay, root ->
        val damaged = probe.fixture.copyOf().also { it[10] = (it[10] + 1).toByte() }
        // Named by the good bytes but serving different bytes: the client must
        // reject it before any import.
        relay.addComplete(damaged, name)
        val vault = TransferVault(withItem = false)
        val directory = File(root, "store").apply { mkdirs() }
        val engine = SyncEngine(TransferStore(directory), vault)
        check(engine.configure(relay.endpoint(), KEY))
        val error = runCatching { engine.syncNow() }.exceptionOrNull()
        check(error?.message?.contains("does not match its name") == true) { error?.message ?: "no failure" }
        check(vault.importCount == 0)
        check(!engine.isKnown(name))
        check(directory.listFiles()!!.none { it.name.startsWith("dl-") })
    }

    scenario("incomplete") { relay, root ->
        relay.addPartial(name, total, probe.fixture.copyOfRange(0, 4096))
        relay.includeIncomplete = true
        val vault = TransferVault(withItem = false)
        val engine = SyncEngine(TransferStore(File(root, "store").apply { mkdirs() }), vault)
        check(engine.configure(relay.endpoint(), KEY))
        engine.syncNow()
        check(vault.importCount == 0 && !engine.isKnown(name))
        check(engine.status.contains("still uploading")) { engine.status }
    }

    scenario("invalid-listing") { relay, root ->
        relay.listingOverride = "[{\"name\":\"not-a-package\",\"size\":3,\"seq\":1}]"
        val vault = TransferVault(withItem = false)
        val engine = SyncEngine(TransferStore(File(root, "store").apply { mkdirs() }), vault)
        check(engine.configure(relay.endpoint(), KEY))
        val error = runCatching { engine.syncNow() }.exceptionOrNull()
        check(error?.message?.contains("invalid package name") == true) { error?.message ?: "no failure" }
    }

    println("SyncTransfer: upload resume (HEAD and 409), 200-at-offset-0, lost answer, download resume, digest rejection, incomplete and invalid listing passed")
}
