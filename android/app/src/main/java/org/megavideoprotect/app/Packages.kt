package org.megavideoprotect.app

import android.content.Context
import android.net.Uri
import java.io.File

/** One .mvpk export, matching the desktop package (password, folders, files). */
data class PackagePlan(
    val files: List<Pair<Long, String>>,
    val directories: List<String>,
    val suggestion: String,
    val error: String? = null,
)

fun packageSegment(raw: String): String {
    var name = raw.replace(Regex("[\\\\/:*?\"<>|\\u0000-\\u001F\\u007F]"), "_").trim()
    while (name.endsWith(' ') || name.endsWith('.')) name = name.dropLast(1)
    if (name.isEmpty() || name == "." || name == "..") name = "_"
    val upper = name.uppercase()
    if (upper in setOf("CON", "PRN", "AUX", "NUL") || Regex("^(COM|LPT)[1-9]$").matches(upper)) {
        name += "_"
    }
    if (name.length > 120) {
        name = name.take(120)
        while (name.endsWith(' ') || name.endsWith('.')) name = name.dropLast(1)
        if (name.isEmpty()) name = "_"
    }
    return name
}

private fun claimPath(used: MutableSet<String>, relative: String): String {
    val slash = relative.lastIndexOf('/')
    val dot = relative.lastIndexOf('.')
    val hasExtension = dot > slash
    val stem = if (hasExtension) relative.substring(0, dot) else relative
    val extension = if (hasExtension) relative.substring(dot) else ""
    var candidate = relative
    var suffix = 2
    while (candidate in used && suffix < 500) {
        candidate = "$stem ($suffix)$extension"
        suffix++
    }
    used.add(candidate)
    return candidate
}

/** Builds the same package tree the desktop writes for a folder or a selection. */
fun planPackage(
    videos: List<VideoEntry>,
    folders: List<FolderEntry>,
    fileIds: Set<Long>,
    folderIds: Set<Long>,
): PackagePlan {
    val files = ArrayList<Pair<Long, String>>()
    val directories = ArrayList<String>()
    val used = HashSet<String>()
    var problem: String? = null
    fun addFolder(folderId: Long, relative: String): Boolean {
        if (relative.toByteArray().size > 1024) {
            problem = "A folder path in this package is too long."
            return false
        }
        val dir = claimPath(used, relative)
        directories.add(dir)
        for (child in folders) {
            if (child.parentId != folderId) continue
            if (!addFolder(child.id, "$dir/${packageSegment(child.name)}")) return false
        }
        for (video in videos) {
            if (video.folderId != folderId) continue
            val filePath = "$dir/${packageSegment(video.name)}"
            if (filePath.toByteArray().size > 1024) {
                problem = "A file path in this package is too long."
                return false
            }
            files.add(video.id to claimPath(used, filePath))
        }
        return true
    }
    var suggestion = "package"
    if (folderIds.isNotEmpty()) {
        val roots = folderIds.filter { id ->
            folderIds.none { other -> other != id && folderContains(folders, other, id) }
        }
        var named = false
        for (folderId in roots) {
            val folder = folders.firstOrNull { it.id == folderId } ?: continue
            val segment = packageSegment(folder.name)
            if (!named) {
                suggestion = segment
                named = true
            }
            if (!addFolder(folderId, segment)) break
        }
    }
    if (problem == null) {
        var named = suggestion != "package"
        for (id in fileIds) {
            val video = videos.firstOrNull { it.id == id } ?: continue
            if (folderIds.any { folderContains(folders, it, video.folderId) || it == video.folderId }) {
                continue
            }
            val filePath = packageSegment(video.name)
            if (!named) {
                suggestion = filePath.substringBeforeLast('.', filePath)
                named = true
            }
            if (filePath.toByteArray().size > 1024) {
                problem = "A file path in this package is too long."
                break
            }
            files.add(video.id to claimPath(used, filePath))
        }
    }
    if (problem != null) return PackagePlan(emptyList(), emptyList(), suggestion, problem)
    if (files.isEmpty() && directories.isEmpty()) {
        return PackagePlan(emptyList(), emptyList(), suggestion, "Select items or a folder to export.")
    }
    return PackagePlan(files, directories, suggestion)
}

private fun folderContains(folders: List<FolderEntry>, ancestor: Long, node: Long): Boolean {
    var cursor = node
    repeat(64) {
        if (cursor == 0L) return false
        if (cursor == ancestor) return true
        cursor = folders.firstOrNull { it.id == cursor }?.parentId ?: return false
    }
    return false
}

fun copyUriToFile(context: Context, uri: Uri, dest: File, onCopy: (Long, Long) -> Unit) {
    val length = context.contentResolver.openAssetFileDescriptor(uri, "r")?.use { it.length } ?: -1L
    context.contentResolver.openInputStream(uri).use { input ->
        if (input == null) error("The package could not be opened.")
        dest.outputStream().use { output ->
            val buffer = ByteArray(64 * 1024)
            var sent = 0L
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                output.write(buffer, 0, n)
                sent += n
                onCopy(sent, length)
            }
        }
    }
}

fun copyFileToUri(context: Context, source: File, uri: Uri, onCopy: (Long, Long) -> Unit) {
    val length = source.length()
    source.inputStream().use { input ->
        context.contentResolver.openOutputStream(uri).use { output ->
            if (output == null) error("The package could not be saved.")
            val buffer = ByteArray(64 * 1024)
            var sent = 0L
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                output.write(buffer, 0, n)
                sent += n
                onCopy(sent, length)
            }
        }
    }
}
