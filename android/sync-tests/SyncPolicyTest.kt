package org.megavideoprotect.app

fun main() {
    check(SyncPolicy.retryMillis(0, 0) == 1000L)
    check(SyncPolicy.retryMillis(20, 999) == 60999L)
    check(SyncPolicy.needsDownload(4, 3).not()) // rollback rejected
    check(SyncPolicy.needsDownload(4, 4).not())
    check(SyncPolicy.needsDownload(4, 5))
    check(SyncPolicy.importedId(setOf(1, 2), setOf(1, 2, 7)) == 7L)
    check(SyncPolicy.importedId(setOf(1, 2), setOf(1, 2)) == null) // JNI dedup has no ID
    check(SyncPolicy.importedId(setOf(1), setOf(1, 2, 3)) == null)
    check(SyncPolicy.validId("a".repeat(32)))
    check(!SyncPolicy.validId("A".repeat(32)))
    check(!SyncPolicy.validId("a".repeat(31)))
    check(SyncPolicy.validSecret("0".repeat(64)))
    check(!SyncPolicy.validSecret("0".repeat(63)))
    check(SyncPolicy.validEndpoint("https://example.com:8766"))
    check(SyncPolicy.validEndpoint("http://127.0.0.1:8766"))
    check(!SyncPolicy.validEndpoint("http://example.com"))
    check(!SyncPolicy.validEndpoint("https://user:pass@example.com"))
    check(!SyncPolicy.validEndpoint("https://example.com/path"))
    check(SyncPolicy.folderPath(2, mapOf(1L to (0L to "a"), 2L to (1L to "b"))) == "a/b")
    check(runCatching { SyncPolicy.folderPath(1, mapOf(1L to (1L to "a"))) }.isFailure)
    val marker = ByteArray(24) { it.toByte() }
    check(SyncPolicy.vaultNamespace("root", marker) == SyncPolicy.vaultNamespace("root", marker.copyOf()))
    check(SyncPolicy.vaultNamespace("root", marker) != SyncPolicy.vaultNamespace("root", marker.also { it[23] = 99 }))
    check(runCatching { SyncPolicy.vaultNamespace("root", ByteArray(23)) }.isFailure)
    println("SyncPolicy: identity, endpoints, revisions, dedup, backoff and paths passed")
}
