package org.megavideoprotect.app

import java.net.URI

/** Pure safety decisions shared by transfers and the setup UI. */
object SyncPolicy {
    /** core/src/metadata.cpp writes a stable 24-byte vault.ready marker (magic + vault ID).
     * Include it so recreating a vault at the same path cannot reuse another vault's local IDs. */
    fun vaultNamespace(canonicalRoot: String, readyMarker: ByteArray): String {
        require(readyMarker.size == 24) { "Invalid vault identity marker" }
        return java.security.MessageDigest.getInstance("SHA-256")
            .digest(canonicalRoot.toByteArray(Charsets.UTF_8) + byteArrayOf(0) + readyMarker)
            .joinToString("") { "%02x".format(it.toInt() and 255) }
    }
    fun validId(value: String) = Regex("[0-9a-f]{32}").matches(value)
    fun validSecret(value: String) = Regex("[0-9a-f]{64}").matches(value)
    fun retryMillis(attempt: Int, jitter: Int): Long =
        minOf(60_000L, 1000L shl attempt.coerceIn(0, 6)) + jitter.coerceIn(0, 999)
    fun needsDownload(seen: Long, remote: Long) = remote > seen
    fun importedId(before: Set<Long>, after: Set<Long>): Long? = (after - before).singleOrNull()
    fun validEndpoint(value: String): Boolean = runCatching {
        val u = URI(value)
        u.host != null && u.userInfo == null && u.query == null && u.fragment == null &&
            (u.path.isNullOrEmpty() || u.path == "/") &&
            (u.scheme == "https" || (u.scheme == "http" && u.host in setOf("127.0.0.1", "localhost", "[::1]")))
    }.getOrDefault(false)
    fun folderPath(id: Long, folders: Map<Long, Pair<Long, String>>): String {
        var cursor = id
        val seen = HashSet<Long>()
        val names = ArrayList<String>()
        while (cursor != 0L) {
            check(seen.add(cursor) && seen.size <= 64) { "Invalid folder tree" }
            val folder = folders[cursor] ?: error("Missing folder")
            names.add(folder.second)
            cursor = folder.first
        }
        return names.asReversed().joinToString("/")
    }
}
