package org.megavideoprotect.app

/** Pure protocol 3 rules: package names, endpoints, the vault namespace, paths. */
fun main() {
    check(SyncPolicy.isHex("0".repeat(64), 64))
    check(!SyncPolicy.isHex("0".repeat(63), 64))
    check(!SyncPolicy.isHex("A".repeat(64), 64)) // protocol names are lowercase only
    check(!SyncPolicy.isHex("g".repeat(64), 64))

    val hash = sha256("package bytes".toByteArray())
    val name = SyncPolicy.packageName(hash)
    check(SyncPolicy.isPackageName(name))
    check(name.length == 69 && name.endsWith(".mvpk"))
    check(!SyncPolicy.isPackageName(name.uppercase()))
    check(!SyncPolicy.isPackageName(hash)) // suffix required
    check(!SyncPolicy.isPackageName(hash + ".mvpk.part"))
    check(!SyncPolicy.isPackageName("0".repeat(63) + ".mvpk"))
    check(runCatching { SyncPolicy.packageName("0".repeat(63)) }.isFailure)

    check(SyncPolicy.validServer("https://vault.example:8766"))
    check(SyncPolicy.validServer("https://vault.example"))
    check(SyncPolicy.validServer("http://127.0.0.1:8766"))
    check(SyncPolicy.validServer("http://localhost:8766"))
    check(SyncPolicy.validServer("http://[::1]:8766"))
    check(!SyncPolicy.validServer("http://vault.example:8766")) // plain HTTP off loopback
    check(!SyncPolicy.validServer("https://user:pass@vault.example"))
    check(!SyncPolicy.validServer("https://vault.example/path"))
    check(!SyncPolicy.validServer("https://vault.example/?x=1"))
    check(!SyncPolicy.validServer("ftp://vault.example"))
    check(!SyncPolicy.validServer(""))

    check(SyncPolicy.folderPath(2, mapOf(1L to (0L to "a"), 2L to (1L to "b"))) == "a/b")
    check(SyncPolicy.folderPath(0, mapOf(1L to (0L to "a"))) == "")
    check(runCatching { SyncPolicy.folderPath(1, mapOf(1L to (1L to "a"))) }.isFailure)

    val marker = ByteArray(24) { it.toByte() }
    check(SyncPolicy.vaultNamespace("root", marker) == SyncPolicy.vaultNamespace("root", marker.copyOf()))
    check(SyncPolicy.vaultNamespace("root", marker) != SyncPolicy.vaultNamespace("root", marker.copyOf().also { it[23] = 99 }))
    check(SyncPolicy.vaultNamespace("root", marker) != SyncPolicy.vaultNamespace("other", marker))
    check(runCatching { SyncPolicy.vaultNamespace("root", ByteArray(23)) }.isFailure)

    check(SyncPolicy.CHUNK == 1024 * 1024)
    check(SyncPolicy.MAX_PACKAGE == 1024L * 1024 * 1024)
    println("SyncPolicy: package names, endpoints, namespace and folder paths passed")
}
