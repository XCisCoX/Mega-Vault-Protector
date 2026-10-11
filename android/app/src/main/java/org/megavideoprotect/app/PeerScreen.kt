package org.megavideoprotect.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/** What the PC folder showed last, so returning to it does not reload from scratch. */
private object PeerCache {
    var pc: String? = null
    var folders: List<FolderEntry> = emptyList()
    var files: List<VideoEntry> = emptyList()
    var currentFolderId = 0L
    var loaded = false
    private val thumbs = LinkedHashMap<Long, Bitmap>()

    fun thumb(id: Long): Bitmap? = thumbs[id]

    fun putThumb(id: Long, bitmap: Bitmap) {
        if (thumbs.size > 96) thumbs.remove(thumbs.keys.first())
        thumbs[id] = bitmap
    }

    fun reset(pc: String?) {
        this.pc = pc
        folders = emptyList()
        files = emptyList()
        currentFolderId = 0L
        loaded = false
        thumbs.clear()
    }
}

private fun pickKey(folder: Boolean, id: Long) = if (folder) "f$id" else "v$id"

/**
 * The swapped PC's library. Read-only: nothing here can change or remove the
 * PC's files. Tap a folder to open it, tap files (or long-press folders) to
 * select, then copy them into this phone's library. Folders keep their layout.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun PeerScreen(onClose: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val pc = PeerLink.pcName
    if (PeerCache.pc != pc) PeerCache.reset(pc)

    var folders by remember { mutableStateOf(PeerCache.folders) }
    var files by remember { mutableStateOf(PeerCache.files) }
    var current by remember { mutableStateOf(PeerCache.currentFolderId) }
    var loading by remember { mutableStateOf(!PeerCache.loaded) }
    var message by remember { mutableStateOf<String?>(null) }
    var picked by remember { mutableStateOf(setOf<String>()) }
    var choosing by remember { mutableStateOf(false) }
    var progress by remember { mutableStateOf<Float?>(null) }
    var progressLabel by remember { mutableStateOf("") }
    val copying = progress != null

    LaunchedEffect(pc) {
        if (pc == null) onClose()
    }

    fun open(id: Long) {
        current = id
        PeerCache.currentFolderId = id
        picked = emptySet()
    }

    fun reload() {
        loading = true
        message = null
        scope.launch {
            val listing = withContext(Dispatchers.IO) {
                val f = PeerLink.getText("/v1/folders")
                val v = if (f != null) PeerLink.getText("/v1/videos") else null
                if (f == null || v == null) null
                else runCatching { Json.folders(f) to Json.videos(v) }.getOrNull()
            }
            loading = false
            if (listing == null) {
                message = "The PC did not answer. Keep Mega Vault Protect unlocked on the PC."
                return@launch
            }
            folders = listing.first
            files = listing.second
            PeerCache.folders = listing.first
            PeerCache.files = listing.second
            PeerCache.loaded = true
            if (current != 0L && listing.first.none { it.id == current }) open(0L)
        }
    }

    LaunchedEffect(Unit) {
        if (!PeerCache.loaded) reload()
    }

    BackHandler {
        when {
            copying -> Unit
            choosing -> choosing = false
            picked.isNotEmpty() -> picked = emptySet()
            current != 0L -> open(folders.firstOrNull { it.id == current }?.parentId ?: 0L)
            else -> onClose()
        }
    }

    fun copyTo(destination: Long) {
        val chosen = picked.ifEmpty {
            (folders.filter { it.parentId == current }.map { pickKey(true, it.id) } +
                files.filter { it.folderId == current }.map { pickKey(false, it.id) }).toSet()
        }
        val pickedFolders = chosen.filter { it.startsWith("f") }.mapNotNull { it.drop(1).toLongOrNull() }
        val pickedFiles = chosen.filter { it.startsWith("v") }.mapNotNull { it.drop(1).toLongOrNull() }.toSet()
        progress = 0f
        progressLabel = "Getting ready"
        message = null
        scope.launch {
            val outcome = withContext(Dispatchers.IO) {
                copyFromPc(context, folders, files, pickedFolders, pickedFiles, destination) { done, total, label ->
                    withContext(Dispatchers.Main) {
                        progress = if (total > 0L) done.toFloat() / total.toFloat() else 0f
                        progressLabel = label
                    }
                }
            }
            progress = null
            progressLabel = ""
            picked = emptySet()
            message = outcome
        }
    }

    val childFolders = folders.filter { it.parentId == current }.sortedBy { it.name.lowercase() }
    val childFiles = files.filter { it.folderId == current }.sortedBy { it.name.lowercase() }
    val upParent = if (current == 0L) null else folders.firstOrNull { it.id == current }?.parentId ?: 0L

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        Column(Modifier.fillMaxSize()) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(Color(0xFF101014))
                    .statusBarsPadding()
                    .padding(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 10.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "‹ Library",
                        color = Mvp.accent,
                        fontSize = 15.sp,
                        modifier = Modifier
                            .clickable(enabled = !copying) { onClose() }
                            .padding(end = 12.dp, top = 4.dp, bottom = 4.dp),
                    )
                    Text(
                        pc.orEmpty(),
                        color = Mvp.title,
                        fontSize = 20.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        "Disconnect",
                        color = Color(0xFFFF6B6B),
                        fontSize = 14.sp,
                        modifier = Modifier
                            .clickable(enabled = !copying) {
                                PeerLink.unlink()
                                onClose()
                            }
                            .padding(start = 12.dp, top = 4.dp, bottom = 4.dp),
                    )
                }
                Text(
                    "Shared from the PC. You can look and copy, not change or remove anything. " +
                        "The PC also sees your library as a folder.",
                    color = Mvp.description,
                    fontSize = 12.sp,
                )
                PeerCrumbs(pc.orEmpty(), folders, current, enabled = !copying) { open(it) }
            }

            Box(Modifier.weight(1f).fillMaxWidth()) {
                if (loading && folders.isEmpty() && files.isEmpty()) {
                    CircularProgressIndicator(
                        Modifier.align(Alignment.Center).size(28.dp),
                        color = Mvp.accent,
                        strokeWidth = 2.dp,
                    )
                } else if (childFolders.isEmpty() && childFiles.isEmpty() && upParent == null) {
                    Text(
                        if (PeerCache.loaded) "The PC's library is empty." else "",
                        color = Mvp.description,
                        fontSize = 14.sp,
                        modifier = Modifier.align(Alignment.Center),
                    )
                } else {
                    LazyVerticalGrid(
                        columns = GridCells.Fixed(3),
                        modifier = Modifier.fillMaxSize(),
                        horizontalArrangement = Arrangement.spacedBy(1.dp),
                        verticalArrangement = Arrangement.spacedBy(1.dp),
                    ) {
                        if (upParent != null) {
                            item(key = "up") {
                                PeerFolderTile("..", picked = false, onClick = { if (!copying) open(upParent) }, onLongPress = {})
                            }
                        }
                        items(childFolders, key = { "f${it.id}" }) { folder ->
                            val key = pickKey(true, folder.id)
                            PeerFolderTile(
                                folder.name,
                                picked = key in picked,
                                onClick = {
                                    if (copying) return@PeerFolderTile
                                    if (picked.isNotEmpty()) {
                                        picked = if (key in picked) picked - key else picked + key
                                    } else {
                                        open(folder.id)
                                    }
                                },
                                onLongPress = {
                                    if (!copying) picked = if (key in picked) picked - key else picked + key
                                },
                            )
                        }
                        items(childFiles, key = { "v${it.id}" }) { file ->
                            val key = pickKey(false, file.id)
                            PeerFileTile(file, picked = key in picked) {
                                if (!copying) picked = if (key in picked) picked - key else picked + key
                            }
                        }
                    }
                }
            }

            Column(
                Modifier
                    .fillMaxWidth()
                    .background(Color(0xFF101014))
                    .navigationBarsPadding()
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                message?.let { Text(it, color = Mvp.text, fontSize = 13.sp) }
                val fraction = progress
                if (fraction != null) {
                    LinearProgressIndicator(
                        progress = { fraction.coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth(),
                        color = Mvp.primary,
                        trackColor = Mvp.sliderGroove,
                    )
                    Text(
                        "${(fraction * 100).toInt()}%  $progressLabel",
                        color = Mvp.description,
                        fontSize = 11.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                } else {
                    val nothingHere = childFolders.isEmpty() && childFiles.isEmpty()
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        MvpButton(
                            if (picked.isEmpty()) "Copy everything here" else "Copy ${picked.size} to my library",
                            onClick = { choosing = true },
                            primary = true,
                            enabled = !loading && (picked.isNotEmpty() || !nothingHere),
                            modifier = Modifier.weight(1f),
                        )
                        if (picked.isNotEmpty()) {
                            MvpButton("Clear", onClick = { picked = emptySet() })
                        } else {
                            MvpButton("Refresh", onClick = { reload() }, enabled = !loading)
                        }
                    }
                }
            }
        }
    }

    if (choosing) {
        CopyTargetDialog(
            onPick = { destination ->
                choosing = false
                copyTo(destination)
            },
            onDismiss = { choosing = false },
        )
    }
}

@Composable
private fun PeerCrumbs(pc: String, folders: List<FolderEntry>, current: Long, enabled: Boolean, onOpen: (Long) -> Unit) {
    val chain = ArrayList<FolderEntry>()
    var cursor = current
    var guard = 0
    while (cursor != 0L && guard++ < 64) {
        val folder = folders.firstOrNull { it.id == cursor } ?: break
        chain.add(0, folder)
        cursor = folder.parentId
    }
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PeerCrumb(pc, current == 0L, enabled) { onOpen(0L) }
        chain.forEach { folder ->
            Text("/", color = Mvp.description, fontSize = 13.sp)
            PeerCrumb(folder.name, folder.id == current, enabled) { onOpen(folder.id) }
        }
    }
}

@Composable
private fun PeerCrumb(text: String, current: Boolean, enabled: Boolean, onClick: () -> Unit) {
    Text(
        text,
        color = if (current) Color.White else Mvp.accent,
        fontSize = 13.sp,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 4.dp, vertical = 2.dp),
    )
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun PeerFolderTile(name: String, picked: Boolean, onClick: () -> Unit, onLongPress: () -> Unit) {
    Box(
        Modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .background(Color(0xFF161618))
            .combinedClickable(onClick = onClick, onLongClick = onLongPress),
    ) {
        Canvas(Modifier.align(Alignment.Center).fillMaxSize().padding(18.dp)) {
            val w = size.width
            val h = size.height
            drawRoundRect(
                color = Color(0xFF8CC4FF),
                topLeft = Offset(w * 0.08f, h * 0.14f),
                size = Size(w * 0.42f, h * 0.22f),
                cornerRadius = CornerRadius(w * 0.08f, w * 0.08f),
            )
            drawRoundRect(
                color = Color(0xFF408CE6),
                topLeft = Offset(w * 0.06f, h * 0.30f),
                size = Size(w * 0.88f, h * 0.56f),
                cornerRadius = CornerRadius(w * 0.1f, w * 0.1f),
            )
        }
        Text(
            name,
            color = Color.White,
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.align(Alignment.BottomStart).padding(start = 6.dp, end = 6.dp, bottom = 6.dp),
        )
        if (picked) PickedMark()
    }
}

@Composable
private fun PeerFileTile(file: VideoEntry, picked: Boolean, onClick: () -> Unit) {
    var bitmap by remember(file.id) { mutableStateOf(PeerCache.thumb(file.id)) }
    LaunchedEffect(file.id) {
        if (bitmap != null) return@LaunchedEffect
        val decoded = withContext(Dispatchers.IO) {
            PeerLink.getBytes("/v1/thumbnail?id=${file.id}&max=320")
                ?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        }
        if (decoded != null) {
            PeerCache.putThumb(file.id, decoded)
            bitmap = decoded
        }
    }
    Box(
        Modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .background(Mvp.card)
            .clickable(onClick = onClick),
    ) {
        val image = bitmap
        if (image != null) {
            Image(
                bitmap = image.asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        } else {
            Text(
                if (isAudioName(file.name)) "♪" else file.name.substringAfterLast('.', "").uppercase(),
                color = Mvp.description,
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.align(Alignment.Center),
            )
        }
        Text(
            file.name,
            color = Color.White,
            fontSize = 11.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .align(Alignment.BottomStart)
                .fillMaxWidth()
                .background(Color.Black.copy(alpha = 0.55f))
                .padding(horizontal = 6.dp, vertical = 3.dp),
        )
        if (picked) PickedMark()
    }
}

@Composable
private fun PickedMark() {
    Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.45f))) {
        Box(
            Modifier
                .align(Alignment.TopEnd)
                .padding(6.dp)
                .size(22.dp)
                .clip(CircleShape)
                .background(Mvp.primary),
            contentAlignment = Alignment.Center,
        ) {
            Text("✓", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        }
    }
}

/** Where the copy lands in this phone's library. */
@Composable
private fun CopyTargetDialog(onPick: (Long) -> Unit, onDismiss: () -> Unit) {
    var local by remember { mutableStateOf<List<FolderEntry>?>(null) }
    LaunchedEffect(Unit) {
        local = withContext(Dispatchers.IO) {
            runCatching { Json.folders(CoreBridge.nativeListFolders()) }.getOrDefault(emptyList())
        }
    }
    Dialog(onDismissRequest = onDismiss) {
        MvpCard(Modifier.fillMaxWidth()) {
            MvpTitle("Copy into")
            MvpDescription("Pick a place in your library. Copied folders keep their structure.")
            val folders = local
            if (folders == null) {
                CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
            } else {
                val choices = listOf(0L to "Library") + folders
                    .map { it.id to localPath(folders, it.id) }
                    .sortedBy { it.second.lowercase() }
                Column(Modifier.fillMaxWidth().heightIn(max = 320.dp).verticalScroll(rememberScrollState())) {
                    choices.forEach { (id, label) ->
                        Text(
                            label,
                            color = Mvp.text,
                            fontSize = 15.sp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { onPick(id) }
                                .padding(vertical = 10.dp),
                        )
                    }
                }
            }
            MvpButton("Cancel", onClick = onDismiss, modifier = Modifier.fillMaxWidth())
        }
    }
}

private fun localPath(folders: List<FolderEntry>, folderId: Long): String {
    val parts = ArrayList<String>()
    var cursor = folderId
    for (guard in 0 until 64) {
        if (cursor == 0L) break
        val folder = folders.firstOrNull { it.id == cursor } ?: break
        parts.add(0, folder.name)
        cursor = folder.parentId
    }
    return if (parts.isEmpty()) "Library" else "Library / " + parts.joinToString(" / ")
}

private fun safeName(name: String): String {
    val cleaned = name.map { if (it == '/' || it == '\\' || it == ':' || it.code < 0x20) '_' else it }
        .joinToString("").trim()
    return cleaned.ifEmpty { "file" }
}

/**
 * Copies PC folders (with everything under them) and files into this phone's
 * vault under [destination]. A folder that already exists by name is reused, so
 * copying twice merges. A summary on success, or the first problem.
 */
private suspend fun copyFromPc(
    context: Context,
    pcFolders: List<FolderEntry>,
    pcFiles: List<VideoEntry>,
    pickedFolders: List<Long>,
    pickedFiles: Set<Long>,
    destination: Long,
    onProgress: suspend (done: Long, total: Long, label: String) -> Unit,
): String {
    val ordered = ArrayList<FolderEntry>()
    val inPlan = HashSet<Long>()
    for (root in pickedFolders) {
        val queue = ArrayDeque<Long>()
        queue.add(root)
        while (queue.isNotEmpty()) {
            val id = queue.removeFirst()
            if (!inPlan.add(id)) continue
            val folder = pcFolders.firstOrNull { it.id == id } ?: continue
            ordered.add(folder)
            pcFolders.filter { it.parentId == id }.forEach { queue.add(it.id) }
        }
    }
    val plannedFiles = pcFiles.filter { it.folderId in inPlan || it.id in pickedFiles }
    // Counted twice: once while downloading, once while sealing into the vault.
    val total = plannedFiles.sumOf { it.size.coerceAtLeast(0L) } * 2

    var local = runCatching { Json.folders(CoreBridge.nativeListFolders()) }.getOrDefault(emptyList())
    val made = HashMap<Long, Long>()
    fun localFolder(pcFolder: Long): Long = made[pcFolder] ?: destination

    for (folder in ordered) {
        val parent = localFolder(folder.parentId)
        val existing = local.firstOrNull { it.parentId == parent && it.name.equals(folder.name, ignoreCase = true) }
        if (existing != null) {
            made[folder.id] = existing.id
            continue
        }
        val created = runCatching { JSONObject(CoreBridge.nativeCreateFolder(parent, folder.name)) }.getOrNull()
        if (created == null || !created.optBoolean("ok")) {
            val why = created?.optString("error").orEmpty().ifBlank { created?.optString("detail").orEmpty() }
            return "Could not create the folder \"${folder.name}\". $why".trim()
        }
        val id = created.optLong("id", -1L)
        if (id <= 0L) return "Could not create the folder \"${folder.name}\"."
        made[folder.id] = id
        local = local + FolderEntry(id, parent, folder.name)
    }

    val staging = File(context.cacheDir, "pc_copy").apply {
        deleteRecursively()
        mkdirs()
    }
    var done = 0L
    var copied = 0
    try {
        for ((index, file) in plannedFiles.withIndex()) {
            val label = "Copying ${file.name} (${index + 1} of ${plannedFiles.size})"
            onProgress(done, total, label)
            // The vault names the item after the file it imports, so keep the name.
            val dir = File(staging, index.toString()).apply { mkdirs() }
            val target = File(dir, safeName(file.name))
            try {
                target.outputStream().use { out ->
                    var offset = 0L
                    while (offset < file.size) {
                        val ask = minOf(1L shl 20, file.size - offset).toInt()
                        val chunk = PeerLink.readRange(file.id, offset, ask)
                        if (chunk == null || chunk.isEmpty()) {
                            return if (PeerLink.linked()) {
                                "The PC stopped answering while copying \"${file.name}\". Copied $copied."
                            } else {
                                "The PC disconnected while copying \"${file.name}\". Copied $copied."
                            }
                        }
                        out.write(chunk)
                        offset += chunk.size
                        done += chunk.size
                        onProgress(done, total, label)
                    }
                }
                val result = CoreBridge.Result.parse(CoreBridge.nativeImportInto(target.absolutePath, localFolder(file.folderId)))
                if (!result.ok) {
                    return "\"${file.name}\": ${result.error.ifBlank { result.detail }}. Copied $copied."
                }
                copied++
                done += file.size.coerceAtLeast(0L)
                onProgress(done, total, label)
            } finally {
                target.delete()
                dir.delete()
            }
        }
    } finally {
        staging.deleteRecursively()
    }
    refreshGalleryCache()
    return "Copied $copied file(s) and ${ordered.size} folder(s) from ${PeerLink.pcName ?: "the PC"}."
}

/** The library reuses its cached rows on return, so bring them up to date here. */
private fun refreshGalleryCache() {
    runCatching {
        GalleryCache.videos = Json.videos(CoreBridge.nativeListVideos())
        GalleryCache.folders = Json.folders(CoreBridge.nativeListFolders())
        GalleryCache.tags = Json.tags(CoreBridge.nativeListTags())
    }
}
