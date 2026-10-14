package org.megavideoprotect.app

import java.io.File
import org.json.JSONObject
import kotlinx.coroutines.runBlocking

private class InteropStore(override val directory: File) : SyncStateStore {
    private var saved = "{}"
    override fun load() = JSONObject(saved)
    override fun save(state: JSONObject) { saved = state.toString() }
}
/** Opaque deterministic fixture, NOT a real MVPK/native crypto test. */
private class InteropVault(initial: Boolean) : SyncVault {
    val rows = mutableListOf<SyncLocalFile>().apply { if (initial) add(SyncLocalFile(1, "private/folder/file.txt", "initial")) }
    var imports = 0
    override fun isUnlocked() = true
    override fun files() = rows.toList()
    override fun export(file: SyncLocalFile, target: File, key: String) {
        target.writeBytes(ByteArray(2 * 1024 * 1024 + 17) { (it % 233).toByte() } + file.signature.toByteArray())
    }
    override fun import(source: File, key: String): Int {
        imports++
        rows.add(SyncLocalFile(rows.size.toLong() + 1, "local/remote-$imports", fileHash(source)))
        return 1
    }
}
fun main() = runBlocking {
    val endpoint = readln()
    val firstCode = readln()
    val secondCode = readln()
    val directory = File(readln())
    val pairing = SyncHttp(endpoint, "", "")
    check(pairing.json("GET", "/healthz").getInt("protocol") == 2)
    val token1 = pairing.json("POST", "/v2/pair", JSONObject().put("code", firstCode).put("name", "Android interop A")).getString("token")
    val token2 = pairing.json("POST", "/v2/pair", JSONObject().put("code", secondCode).put("name", "Android interop B")).getString("token")
    val vault1 = InteropVault(true)
    val vault2 = InteropVault(false)
    val first = SyncEngine(InteropStore(File(directory, "a").apply { mkdirs() }), vault1)
    val second = SyncEngine(InteropStore(File(directory, "b").apply { mkdirs() }), vault2)
    first.configure(endpoint, "", "a".repeat(64), token1)
    second.configure(endpoint, "", "a".repeat(64), token2)
    first.stage(setOf(1)); first.retry(); first.pass()
    check(first.pendingCount() == 0) { first.status() }
    second.refresh()
    val id = second.catalog().single().getString("id")
    second.selectDownloads(setOf(id)); second.retry(); second.pass()
    check(vault2.imports == 1 && second.receipt() == null) { second.status() }
    second.stage(setOf(1)); check(second.pendingCount() == 0)
    vault1.rows[0] = vault1.rows[0].copy(signature = "first-device-edit")
    vault2.rows[0] = vault2.rows[0].copy(signature = "offline-second-device-edit")
    second.stage(setOf(1)) // base revision 1
    first.stage(setOf(1)); first.retry(); first.pass() // commits revision 2
    second.retry(); second.pass() // CAS failure; retains queue/package
    check(second.pendingCount() == 1 && second.notice().contains("conflict", ignoreCase = true))
    second.retry(); second.pass() // publishes fork then imports first device's new version
    check(second.pendingCount() == 0 && vault2.rows.size == 2) { second.status() }
    val api = SyncHttp(endpoint, token2, "")
    val records = api.json("GET", "/v2/objects").getJSONArray("objects")
    check(records.length() == 2)
    check(api.json("GET", "/v2/objects/$id/history").getJSONArray("versions").length() == 2)
    check(api.json("GET", "/v2/devices").getJSONArray("devices").length() == 2)
    println("Actual sync_server interop: pair, >2 MiB chunk upload/range download, no echo, two-device CAS keep-both, history/devices passed (opaque fixtures; JNI not exercised)")
}
