package org.megavideoprotect.app

import java.util.concurrent.ConcurrentHashMap

/**
 * What a vault file is. The header wins. The filename is used only when the
 * header is not recognized. route: 0 cannot be opened, 1 video or picture,
 * 2 audio, 3 PDF, 4 plain text.
 */
data class FileKind(val label: String, val route: Int)

object FileKinds {
    private val cache = ConcurrentHashMap<Long, FileKind>()
    // Bumped when the open vault changes. A read started on the previous vault
    // must not be stored under the same id in the next one.
    private var stamp = 0

    fun peek(id: Long): FileKind? = cache[id]

    fun clear() {
        stamp++
        cache.clear()
    }

    fun resolve(id: Long, name: String): FileKind {
        cache[id]?.let { return it }
        val seen = stamp
        val head = runCatching { CoreBridge.nativeReadRange(id, 0L, 560) }.getOrNull()
            ?: ByteArray(0)
        if (seen != stamp) return cache[id] ?: fromName(name)
        val sniffed = sniff(head)
        val kind = if (sniffed.label == "File") fromName(name) else sniffed
        if (seen == stamp) cache[id] = kind
        return kind
    }

    fun isImage(kind: FileKind): Boolean {
        val label = kind.label.lowercase()
        return kind.route == 1 && label in IMAGE_LABELS
    }

    fun compactSize(bytes: Long): String {
        if (bytes < 1024) return "${bytes}B"
        val kb = bytes / 1024.0
        if (bytes < 1024 * 1024) return if (kb < 10) "%.1fKB".format(kb) else "${kb.toInt()}KB"
        val mb = bytes / (1024.0 * 1024.0)
        if (bytes < 1024L * 1024L * 1024L) return if (mb < 10) "%.1fMB".format(mb) else "${(mb + 0.5).toInt()}MB"
        return "%.1fGB".format(bytes / (1024.0 * 1024.0 * 1024.0))
    }
}

private val IMAGE_LABELS = setOf(
    "jpeg", "jpg", "png", "gif", "bmp", "webp", "tif", "tiff", "heic", "heif",
)

private fun titled(suffix: String): String {
    if (suffix.isEmpty()) return "File"
    return suffix.replaceFirstChar { it.uppercase() }
}

private fun fromName(name: String): FileKind {
    val suffix = name.substringAfterLast('.', "").lowercase()
    if (suffix.isEmpty() || suffix == name.lowercase()) return FileKind("File", 0)
    if (suffix == "pdf") return FileKind("Pdf", 3)
    if (suffix in TEXT_SUFFIXES) return FileKind(titled(suffix), 4)
    if (isAudioName("$suffix.$suffix") || isAudioName("x.$suffix")) return FileKind(titled(suffix), 2)
    val playable = setOf(
        "mp4", "m4v", "mkv", "avi", "mov", "wmv", "webm", "ts", "mts", "m2ts",
        "flv", "3gp", "mpg", "mpeg", "vob", "asf", "jpg", "jpeg", "png", "webp",
        "bmp", "gif", "heic", "heif", "tif", "tiff",
    )
    return FileKind(titled(suffix), if (suffix in playable) 1 else 0)
}

private fun starts(data: ByteArray, text: String, offset: Int = 0): Boolean {
    val raw = text.toByteArray(Charsets.ISO_8859_1)
    if (offset < 0 || offset + raw.size > data.size) return false
    for (i in raw.indices) if (data[offset + i] != raw[i]) return false
    return true
}

private fun sniff(data: ByteArray): FileKind {
    fun named(label: String, route: Int) = FileKind(label, route)
    if (data.isEmpty()) return named("File", 0)
    if (starts(data, "%PDF")) return named("Pdf", 3)
    if (data.size >= 3 && data[0] == 0xFF.toByte() && data[1] == 0xD8.toByte() && data[2] == 0xFF.toByte()) {
        return named("Jpeg", 1)
    }
    if (starts(data, "\u0089PNG")) return named("Png", 1)
    if (starts(data, "GIF87a") || starts(data, "GIF89a")) return named("Gif", 1)
    if (starts(data, "BM")) return named("Bmp", 1)
    if (starts(data, "RIFF") && starts(data, "WEBP", 8)) return named("Webp", 1)
    if (starts(data, "RIFF") && starts(data, "WAVE", 8)) return named("Wav", 2)
    if (starts(data, "RIFF") && starts(data, "AVI ", 8)) return named("Avi", 1)
    if (starts(data, "fLaC")) return named("Flac", 2)
    if (starts(data, "ID3")) return named("Mp3", 2)
    if (starts(data, "OggS")) return named("Ogg", 2)
    if (data.size >= 2 && data[0] == 0xFF.toByte() && data[1] in listOf(0xFB.toByte(), 0xF3.toByte(), 0xF2.toByte())) {
        return named("Mp3", 2)
    }
    if (data.size >= 12 && starts(data, "ftyp", 4)) {
        val brand = String(data, 8, 4, Charsets.ISO_8859_1).trim()
        if (brand.equals("M4A", true) || brand.equals("M4B", true)) return named("M4a", 2)
        return named(if (brand.isEmpty()) "Mp4" else titled(brand.lowercase()), 1)
    }
    if (data.size >= 4 && data[0] == 0x1A.toByte() && data[1] == 0x45.toByte()
        && data[2] == 0xDF.toByte() && data[3] == 0xA3.toByte()
    ) {
        return named("Mkv", 1)
    }
    if (starts(data, "Rar!")) return named("Rar", 0)
    if (data.size >= 4 && data[0] == 'P'.code.toByte() && data[1] == 'K'.code.toByte()) return named("Zip", 0)
    if (data.size >= 6 && data[0] == 0x37.toByte() && data[1] == 0x7A.toByte()) return named("7z", 0)
    if (data.size >= 2 && data[0] == 0x1F.toByte() && data[1] == 0x8B.toByte()) return named("Gz", 0)
    if (starts(data, "BZh")) return named("Bz2", 0)
    if (starts(data, "ustar", 257)) return named("Tar", 0)
    if (looksLikeText(data)) return named("Text", 4)
    return named("File", 0)
}

private val TEXT_SUFFIXES = setOf(
    "txt", "text", "log", "json", "jsonl", "md", "markdown", "csv", "tsv",
    "xml", "html", "htm", "css", "js", "mjs", "yaml", "yml", "ini", "cfg",
    "conf", "toml", "svg", "sh", "bat", "cmd", "ps1", "py", "srt", "vtt",
)

private fun looksLikeText(data: ByteArray): Boolean {
    if (data.size < 4) return false
    var controls = 0
    for (byte in data) {
        val value = byte.toInt() and 0xFF
        if (value == 0) return false
        if (value < 32 && value != 9 && value != 10 && value != 13) controls++
    }
    return controls * 20 < data.size
}
