package org.megavideoprotect.app

import java.net.URI
import java.security.MessageDigest

internal fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 255) }
internal fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(bytes).hex()

/**
 * Pure rules shared by the v3 engine, the HTTP client and the setup screen.
 * Everything here is checkable without Android: the protocol names a package
 * by the SHA-256 of its own bytes and authenticates with one 64-hex key.
 */
object SyncPolicy {
    /** The relay accepts at most one MiB per PUT body; ranges use the same size. */
    const val CHUNK = 1024 * 1024

    /** A listing is JSON, never a package. */
    const val MAX_RESPONSE = 8 * 1024 * 1024

    /** The relay's hard package limit is 1 GiB; refuse anything near it early. */
    const val MAX_PACKAGE = 1024L * 1024 * 1024

    /** Max packages a single listing may carry before the client calls it broken. */
    const val MAX_PACKAGES = 100_000

    fun isHex(value: String, length: Int): Boolean =
        value.length == length && value.all { it in '0'..'9' || it in 'a'..'f' }

    /** Exactly 64 lowercase hex characters plus the .mvpk suffix. */
    fun isPackageName(name: String): Boolean =
        name.length == 69 && name.endsWith(".mvpk") && isHex(name.substring(0, 64), 64)

    fun packageName(hash: String): String {
        require(isHex(hash, 64)) { "A package name is the SHA-256 of its bytes." }
        return "$hash.mvpk"
    }

    /** HTTPS anywhere; plain HTTP only for a loopback relay (development). */
    fun validServer(value: String): Boolean = runCatching {
        val url = URI(value)
        val host = url.host ?: return false
        url.userInfo == null && url.query == null && url.fragment == null &&
            (url.path.isNullOrEmpty() || url.path == "/") &&
            (url.scheme == "https" ||
                (url.scheme == "http" && host in setOf("127.0.0.1", "localhost", "[::1]", "::1")))
    }.getOrDefault(false)

    /**
     * core/src/metadata.cpp writes a stable 24-byte vault.ready marker (magic +
     * vault ID). Include it so recreating a vault at the same path cannot reuse
     * another vault's local-ID ledger.
     */
    fun vaultNamespace(canonicalRoot: String, readyMarker: ByteArray): String {
        require(readyMarker.size == 24) { "Invalid vault identity marker" }
        return sha256(canonicalRoot.toByteArray(Charsets.UTF_8) + byteArrayOf(0) + readyMarker)
    }

    /** Folder chain of one item as "/"-joined package path segments. */
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
