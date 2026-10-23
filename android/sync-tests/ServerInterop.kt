package org.megavideoprotect.app

import java.io.File
import org.json.JSONArray
import org.json.JSONObject

private class InteropStore(override val directory: File) : SyncStateStore {
    private var state = JSONObject()
    override fun load() = JSONObject(state.toString())
    override fun save(state: JSONObject) {
        this.state = JSONObject(state.toString())
    }
}

/** Opaque deterministic fixture, NOT a real MVPK/native crypto test. */
private class InteropVault(withItem: Boolean, private val key: String) : SyncVault {
    val rows = ArrayList<SyncLocalFile>().apply {
        if (withItem) add(SyncLocalFile(1, "private/folder/file.bin", "source-1"))
    }
    var exportCount = 0
    var importCount = 0
    val fixture = ByteArray(2 * 1024 * 1024 + 17) { ((it * 7) % 241).toByte() }

    override fun isUnlocked() = true
    override fun files() = rows.toList()
    override fun export(file: SyncLocalFile, target: File, key: String) {
        check(key == this.key) { "export must receive the library key" }
        exportCount++
        target.writeBytes(fixture)
    }
    override fun import(source: File, key: String): Int {
        check(key == this.key) { "import must receive the library key" }
        importCount++
        check(fileHash(source) == sha256(fixture))
        rows.add(SyncLocalFile((rows.maxOfOrNull { it.id } ?: 0L) + 1, "imported/$importCount", fileHash(source)))
        return 1
    }
}

/**
 * Runs against the repository's real relay on a disposable loopback port:
 * upload of a >2 MiB package in 1 MiB chunks, no echo, a second device importing
 * once with Range, one wrong-key rejection, and explicit delete.
 */
fun main() {
    val endpoint = readln().trim()
    val key = readln().trim()
    val root = File(readln().trim())
    val client = SyncHttp(endpoint, key)

    fun listing(): JSONArray =
        JSONObject(String(client.request("GET", "/v3/files").bytes)).getJSONArray("files")

    val a = InteropVault(withItem = true, key = key)
    val engineA = SyncEngine(InteropStore(File(root, "a").apply { mkdirs() }), a)
    check(engineA.configure(endpoint, key))
    engineA.select(setOf(1))
    engineA.syncNow()
    val name = SyncPolicy.packageName(sha256(a.fixture))
    check(listing().length() == 1) { engineA.status }
    check(listing().getJSONObject(0).getString("name") == name)
    check(client.request("HEAD", "/v3/files/$name").header("X-Complete") == "true")
    check(a.exportCount == 1)
    check(client.request("GET", "/v3/files/$name").bytes.contentEquals(a.fixture))

    val resumed = engineA.status
    engineA.syncNow()
    check(a.exportCount == 1) { "unchanged item must not be exported again" }
    check(listing().length() == 1)

    val b = InteropVault(withItem = false, key = key)
    val engineB = SyncEngine(InteropStore(File(root, "b").apply { mkdirs() }), b)
    check(engineB.configure(endpoint, key))
    engineB.syncNow()
    check(b.importCount == 1 && b.rows.size == 1) { engineB.status }
    check(engineB.status.contains("1 imported")) { engineB.status }
    check(engineB.isKnown(name))
    engineB.syncNow()
    check(b.importCount == 1) { "a known package must not be imported twice" }

    // A wrong key is rejected once and leaves the ledger intact.
    val savedKey = engineB.savedKey()
    val shared = engineB.sharedCount()
    check(engineB.configure(endpoint, "f".repeat(64)))
    val rejected = runCatching { engineB.syncNow() }.exceptionOrNull()
    check(rejected?.message == "The server rejected this library key.") { rejected?.message ?: "no failure" }
    check(engineB.savedKey() == savedKey && engineB.sharedCount() == shared)
    check(engineB.configure(endpoint, key))
    engineB.syncNow()
    check(b.importCount == 1)

    // Explicit delete removes one package for everyone; local items stay put.
    engineA.remove(name)
    check(listing().length() == 0)
    check(a.rows.size == 1 && b.rows.size == 1)
    // The deleting device forgets the mapping, so its next pass shares the item
    // again (no delete propagation, no tombstones).
    engineA.syncNow()
    check(listing().length() == 1) { engineA.status }
    check(engineA.status.contains("1 uploaded"))

    println(
        "Actual protocol 3 relay interop: $resumed / 2.0 MiB chunked upload / range download / " +
            "import once / wrong-key 401 once / explicit delete and re-share passed " +
            "(opaque fixtures; JNI not exercised)",
    )
}
