package org.megavideoprotect.app

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.SystemClock
import android.widget.Toast
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.view.DragAndDropPermissions
import android.view.View
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.draganddrop.dragAndDropSource
import androidx.compose.foundation.draganddrop.dragAndDropTarget
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.draganddrop.DragAndDropEvent
import androidx.compose.ui.draganddrop.DragAndDropTarget
import androidx.compose.ui.draganddrop.DragAndDropTransferData
import androidx.compose.ui.draganddrop.toAndroidDragEvent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerEventTimeoutCancellationException
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import dev.chrisbanes.haze.HazeStyle
import dev.chrisbanes.haze.HazeTint
import dev.chrisbanes.haze.hazeEffect
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.rememberHazeState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/**
 * Rows, thumbnails *and* the toolbar's own state (view mode, tag filter, search
 * text) survive leaving the gallery — the screen is destroyed while the player
 * is open, so anything remembered inside it was lost on the way back and Icons
 * mode snapped to Details. Cleared on lock, so a vault is never shown from a
 * stale cache after being re-opened.
 */
object GalleryCache {
    var vaultLocation: String? = null
    var videos: List<VideoEntry> = emptyList()
    var tags: List<TagEntry> = emptyList()
    var folders: List<FolderEntry> = emptyList()
    var currentFolderId = 0L
    var loaded = false
    // Toolbar state — deliberately *not* reset by clear(): which view you were
    // using is a preference, not vault data.
    var viewMode = 0
    var filterTagId = 0L
    var search = ""
    var sortKey by mutableStateOf(SortKey.Name)
    var sortAscending by mutableStateOf(true)
    var mediaVersion by mutableStateOf(0)
    var generation by mutableStateOf(0)
    val media = HashMap<Long, MediaInfo>()
    // Ids whose container probe failed. Without this, a duration sort scrolls
    // the row away, the lazy list recreates it, and the probe starts again.
    val probeMiss = HashSet<Long>()
    // Scroll position per view mode. The gallery screen is torn down while the
    // player is open, so a scroll state remembered inside it reset to the top:
    // playing something from the middle of the list dropped you back to item 1.
    val detailsScroll = androidx.compose.foundation.lazy.LazyListState()
    val listScroll = androidx.compose.foundation.lazy.LazyListState()
    val gridScroll = androidx.compose.foundation.lazy.grid.LazyGridState()
    private val thumbs = HashMap<String, android.graphics.Bitmap>()

    fun thumb(id: Long, size: Int): android.graphics.Bitmap? = thumbs["$id:$size"]

    fun putThumb(id: Long, size: Int, bitmap: android.graphics.Bitmap) {
        if (thumbs.size > 64) thumbs.clear()
        thumbs["$id:$size"] = bitmap
    }

    fun noteMedia(id: Long, info: MediaInfo) {
        if (media[id] == info) return
        media[id] = info
        mediaVersion++
    }

    fun clear() {
        generation++
        FileKinds.clear()
        vaultLocation = null
        videos = emptyList()
        tags = emptyList()
        folders = emptyList()
        currentFolderId = 0L
        loaded = false
        thumbs.clear()
        media.clear()
        probeMiss.clear()
        mediaVersion++
    }
}

enum class SortKey(val label: String) {
    Name("Name"),
    Size("Size"),
    Duration("Duration"),
    Resolution("Resolution"),
    Codec("Codec"),
    Tags("Tags"),
    Imported("Imported"),
}

private fun applySort(key: SortKey) {
    if (GalleryCache.sortKey == key) {
        GalleryCache.sortAscending = !GalleryCache.sortAscending
    } else {
        GalleryCache.sortKey = key
        GalleryCache.sortAscending = true
    }
}

/** Per-row container metadata. Probing opens the package and decrypts container
 *  bytes, so it is done lazily for visible rows only — the desktop fills these
 *  columns the same way. */
data class MediaInfo(
    val durationMs: Long,
    val width: Int,
    val height: Int,
    val codec: String,
    val artist: String = "",
    val title: String = "",
) {
    val resolution: String get() = if (width > 0 && height > 0) "${width}×${height}" else "…"
    val durationText: String
        get() {
            if (durationMs <= 0) return "…"
            val sec = durationMs / 1000
            return "%d:%02d".format(sec / 60, sec % 60)
        }
    val codecText: String get() = codec.ifBlank { "…" }
}

/** "Artist - Title" from a file name, with a leading track number stripped. */
private fun songLabel(name: String): Pair<String, String> {
    var base = name.substringBeforeLast('.', name).trim()
    base = base.replace(Regex("^\\d{1,3}[.\\s\\-_]+"), "").trim().ifBlank {
        name.substringBeforeLast('.', name).trim()
    }
    val parts = base.split(Regex("\\s+[—–-]\\s+"), limit = 2)
    return if (parts.size == 2 && parts[0].isNotBlank() && parts[1].isNotBlank()) {
        parts[0].trim() to parts[1].trim()
    } else {
        "" to base
    }
}

// One container probe at a time. Duration sort used to start one per visible
// row; they piled up in front of playback and a tap never reached the file.
private val mediaProbe = Semaphore(1)

private fun loadMedia(id: Long): MediaInfo? = runCatching {
    if (id in GalleryCache.probeMiss) return null
    val entry = GalleryCache.videos.firstOrNull { it.id == id }
    val name = entry?.name.orEmpty()
    val route = FileKinds.peek(id)?.route
    val raw = if (RemoteVault.connected()) {
        RemoteVault.getText("/v1/media?id=$id") ?: "{}"
    } else if (isImageName(name) || isPdfName(name) || route == 0 || route == 3 || route == 4) {
        // Stills have no movie duration, and probing a JPEG crashes the native
        // reader. PDFs and notes are not containers either.
        return MediaInfo(0L, 0, 0, if (isImageName(name)) "image" else "")
    } else {
        CoreBridge.nativeMediaInfo(id)
    }
    val o = JSONObject(raw)
    if (!o.has("width") && !o.has("durationMs")) return null
    MediaInfo(
        durationMs = o.optLong("durationMs", 0),
        width = o.optInt("width", 0),
        height = o.optInt("height", 0),
        codec = o.optString("codec", ""),
        artist = o.optString("artist", ""),
        title = o.optString("title", ""),
    )
}.getOrNull()

@Composable
private fun rememberMediaInfo(id: Long): MediaInfo? {
    val generation = GalleryCache.generation
    var info by remember(id, generation) { mutableStateOf(GalleryCache.media[id]) }
    LaunchedEffect(id, generation) {
        if (info != null || id in GalleryCache.probeMiss) return@LaunchedEffect
        val loaded = withContext(Dispatchers.IO) {
            mediaProbe.withPermit { loadMedia(id) }
        }
        if (!isActive || generation != GalleryCache.generation) return@LaunchedEffect
        if (loaded == null) {
            GalleryCache.probeMiss.add(id)
            return@LaunchedEffect
        }
        info = loaded
        GalleryCache.noteMedia(id, loaded)
    }
    return info
}

/** Gallery: a title row with icon actions, search, a view switch and tag
 *  chips, then an Explorer-style details grid, icon grid, list, a per-item
 *  action sheet (remove / export) and a status bar.
 *
 *  Listing is database-only; duration / resolution / codec are probed by the
 *  visible rows on demand. Probing every row up front made a large vault take
 *  seconds to appear. */
/** The swapped PC's library, shown as a folder at the top of the library root. */
private const val PEER_FOLDER_ID = -1L

@Composable
fun VaultScreen(
    vaultLocation: String,
    onLock: () -> Unit,
    onPlay: (Long, String, List<VideoEntry>) -> Unit,
    onOpenMusic: () -> Unit = {},
    onOpenPeer: () -> Unit = {},
    shareUris: List<Uri> = emptyList(),
    onShareHandled: () -> Unit = {},
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val pcName = PeerLink.pcName
    var scanningPc by remember { mutableStateOf(false) }
    var linkingPc by remember { mutableStateOf(false) }
    var videos by remember { mutableStateOf(GalleryCache.videos) }
    LaunchedEffect(RemoteVault.cloud, videos.map { it.id to it.size }) {
        if (RemoteVault.cloud) CloudFiles.pull(context.cacheDir, videos)
    }
    var tags by remember { mutableStateOf(GalleryCache.tags) }
    var folders by remember { mutableStateOf(GalleryCache.folders) }
    var currentFolderId by remember { mutableStateOf(GalleryCache.currentFolderId) }
    var folderMenu by remember { mutableStateOf<FolderEntry?>(null) }
    var moveTarget by remember { mutableStateOf<MoveRequest?>(null) }
    var namePrompt by remember { mutableStateOf<NamePrompt?>(null) }
    // Initialised from the cache so the chosen view / filter / query come back
    // exactly as they were when the player is dismissed.
    var viewMode by remember { mutableStateOf(GalleryCache.viewMode) }
    var filterTagId by remember { mutableStateOf(GalleryCache.filterTagId) }
    var search by remember { mutableStateOf(GalleryCache.search) }
    var error by remember { mutableStateOf<String?>(null) }
    // Long-press starts a selection; further taps add or remove items. Empty
    // means a tap plays the item again.
    var selectedIds by remember { mutableStateOf(setOf<Long>()) }
    var confirmRemove by remember { mutableStateOf(false) }
    var folderDelete by remember { mutableStateOf<FolderEntry?>(null) }
    val incomingFiles = remember { mutableStateListOf<Long>() }
    val incomingFolders = remember { mutableStateListOf<Long>() }
    var showTags by remember { mutableStateOf(false) }
    var showSettings by remember { mutableStateOf(false) }
    var showTransfers by remember { mutableStateOf(false) }
    // Batch import progress: fraction of the bytes of the whole selection.
    var importProgress by remember { mutableStateOf<Float?>(null) }
    var importLabel by remember { mutableStateOf("") }
    // Per-item actions (opened with the ⋮ at the end of a row).
    var actionTarget by remember { mutableStateOf<VideoEntry?>(null) }
    var actionBusy by remember { mutableStateOf(false) }
    var actionMessage by remember { mutableStateOf<String?>(null) }

    fun refresh() {
        scope.launch {
            val (v, t, f) = withContext(Dispatchers.Default) {
                val list = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/videos") else CoreBridge.nativeListVideos()
                }.getOrNull()
                val tagList = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/tags") else CoreBridge.nativeListTags()
                }.getOrNull()
                val folderList = runCatching {
                    if (RemoteVault.connected()) RemoteVault.getText("/v1/folders") else CoreBridge.nativeListFolders()
                }.getOrNull()
                Triple(Json.videos(list ?: "[]"), Json.tags(tagList ?: "[]"), Json.folders(folderList ?: "[]"))
            }
            videos = v
            tags = t
            folders = f
            if (currentFolderId != 0L && f.none { it.id == currentFolderId }) {
                currentFolderId = 0L
                GalleryCache.currentFolderId = 0L
            }
            GalleryCache.vaultLocation = vaultLocation
            GalleryCache.videos = v
            GalleryCache.tags = t
            GalleryCache.folders = f
            GalleryCache.loaded = true
        }
    }

    // Cached rows render instantly when the same vault is re-entered (returning
    // from the player); the list is refetched only when the vault really changed.
    LaunchedEffect(vaultLocation) {
        if (GalleryCache.loaded && GalleryCache.vaultLocation == vaultLocation) {
            videos = GalleryCache.videos
            tags = GalleryCache.tags
            folders = GalleryCache.folders
            currentFolderId = GalleryCache.currentFolderId
        } else {
            GalleryCache.clear()
            refresh()
        }
    }

    LaunchedEffect(LibraryArrivals.stamp) {
        if (LibraryArrivals.stamp == 0) return@LaunchedEffect
        val (arrivedFiles, arrivedFolders) = LibraryArrivals.take()
        if (arrivedFiles.isEmpty() && arrivedFolders.isEmpty()) return@LaunchedEffect
        val knownVideos = videos.map { it.id }.toHashSet()
        val extraVideos = arrivedFiles.filter { it.id !in knownVideos }.map { file ->
            VideoEntry(
                id = file.id,
                name = file.name,
                size = file.size,
                durationMs = 0L,
                width = 0,
                height = 0,
                codec = "",
                tags = emptyList(),
                importedAt = file.importedAt,
                folderId = file.folderId,
            )
        }
        val knownFolders = folders.map { it.id }.toHashSet()
        val extraFolders = arrivedFolders.filter { it.id !in knownFolders }.map { folder ->
            FolderEntry(id = folder.id, parentId = folder.parentId, name = folder.name)
        }
        if (extraVideos.isNotEmpty()) {
            extraVideos.forEach { incomingFiles.add(it.id) }
            videos = videos + extraVideos
        }
        if (extraFolders.isNotEmpty()) {
            extraFolders.forEach { incomingFolders.add(it.id) }
            folders = folders + extraFolders
        }
        if (extraVideos.isNotEmpty() || extraFolders.isNotEmpty()) {
            GalleryCache.videos = videos
            GalleryCache.folders = folders
            GalleryCache.loaded = true
        }
    }

    // Import: multi-select, streamed into the vault one file at a time with a
    // byte-level progress bar for the whole batch (no per-file jumps).
    fun startImport(
        uris: List<Uri>,
        folderId: Long = currentFolderId,
        permissions: DragAndDropPermissions? = null,
    ) {
        if (RemoteVault.connected()) {
            permissions?.release()
            error = "This vault is open on your PC. Import on the PC."
            return
        }
        if (uris.isEmpty()) {
            permissions?.release()
            return
        }
        error = null
        importProgress = 0f
        importLabel = "Adding to vault"
        scope.launch {
            try {
                val results = withContext(Dispatchers.IO) {
                    importBatch(context, uris, folderId) { done, total, label ->
                        importProgress = if (total > 0L) (done.toFloat() / total.toFloat()) else 0f
                        importLabel = label
                    }
                }
                importProgress = null
                importLabel = ""
                val failures = results.filter { !it.ok }
                if (failures.isNotEmpty()) {
                    error = failures.joinToString("\n") { it.error.ifBlank { it.detail } }
                }
            } finally {
                permissions?.release()
            }
        }
    }

    LaunchedEffect(shareUris) {
        if (shareUris.isEmpty()) return@LaunchedEffect
        startImport(shareUris)
        onShareHandled()
    }

    LaunchedEffect(PeerLink.endedReason) {
        val reason = PeerLink.endedReason ?: return@LaunchedEffect
        error = reason
        PeerLink.endedReason = null
    }

    val pcCamera = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) scanningPc = true else error = "Camera permission is needed to scan the PC."
    }

    fun startSwap(code: String) {
        linkingPc = true
        error = null
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                PeerLink.link(code, PeerLink.deviceName(context))
            }
            linkingPc = false
            error = failure ?: "Swapped libraries. The PC's library is the first folder in Library."
        }
    }

    // Removal is permanent: the core deletes the encrypted package, not just the
    // database row.
    fun removeVideo(target: VideoEntry) {
        if (RemoteVault.connected() && !RemoteVault.cloud) {
            error = "This vault is open on your PC. Remove it on the PC."
            return
        }
        actionBusy = true
        actionMessage = null
        scope.launch {
            val res = withContext(Dispatchers.IO) {
                if (RemoteVault.cloud) {
                    val failure = RemoteVault.remove("/v1/remove?id=${target.id}")
                    if (failure == null) CoreBridge.Result(true, "", "")
                    else CoreBridge.Result(false, failure, failure)
                } else {
                    runCatching { CoreBridge.Result.parse(CoreBridge.nativeRemoveVideo(target.id)) }
                        .getOrElse { CoreBridge.Result(false, "Remove failed", it.message ?: "") }
                }
            }
            actionBusy = false
            if (res.ok) {
                actionTarget = null
                selectedIds = selectedIds - target.id
                refresh()
            } else {
                actionMessage = res.error.ifBlank { res.detail }
            }
        }
    }

    fun removeSelected() {
        if (RemoteVault.connected() && !RemoteVault.cloud) {
            error = "This vault is open on your PC. Remove it on the PC."
            return
        }
        val targets = videos.filter { it.id in selectedIds }
        if (targets.isEmpty()) {
            selectedIds = emptySet()
            return
        }
        actionBusy = true
        error = null
        scope.launch {
            var failure: String? = null
            for (target in targets) {
                val res = withContext(Dispatchers.IO) {
                    if (RemoteVault.cloud) {
                        val failure = RemoteVault.remove("/v1/remove?id=${target.id}")
                        if (failure == null) CoreBridge.Result(true, "", "")
                        else CoreBridge.Result(false, failure, failure)
                    } else {
                        runCatching { CoreBridge.Result.parse(CoreBridge.nativeRemoveVideo(target.id)) }
                            .getOrElse { CoreBridge.Result(false, "Remove failed", it.message ?: "") }
                    }
                }
                if (!res.ok) {
                    failure = res.error.ifBlank { res.detail }
                    break
                }
            }
            actionBusy = false
            selectedIds = emptySet()
            confirmRemove = false
            if (failure != null) error = failure
            refresh()
        }
    }

    // Writes each selected package out as a plaintext file in the folder the
    // user picked. The core decrypts into the app cache; that copy is removed
    // after it has been written into the folder.
    fun restoreSelected(tree: Uri) {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Restore it on the PC."
            return
        }
        val targets = videos.filter { it.id in selectedIds }
        if (targets.isEmpty()) return
        actionBusy = true
        error = null
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    val resolver = context.contentResolver
                    val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    runCatching { resolver.takePersistableUriPermission(tree, flags) }
                    val root = DocumentsContract.buildDocumentUriUsingTree(
                        tree, DocumentsContract.getTreeDocumentId(tree),
                    )
                    for (target in targets) {
                        val raw = CoreBridge.nativeRestoreVideo(target.id, context.cacheDir.absolutePath)
                        val obj = JSONObject(raw)
                        if (!obj.optBoolean("ok")) {
                            return@withContext obj.optString("error").ifBlank {
                                obj.optString("detail", "Restore failed")
                            }
                        }
                        val plain = File(obj.optString("path"))
                        val mime = if (isImageName(target.name)) "image/*" else "video/*"
                        val dest = DocumentsContract.createDocument(resolver, root, mime, target.name)
                        if (dest == null) {
                            plain.delete()
                            return@withContext "Could not create ${target.name} in that folder"
                        }
                        val opened = resolver.openOutputStream(dest)
                        if (opened == null) {
                            plain.delete()
                            return@withContext "Could not write ${target.name}"
                        }
                        opened.use { out -> plain.inputStream().use { it.copyTo(out) } }
                        plain.delete()
                    }
                    null
                } catch (e: Exception) {
                    "Restore failed: ${e.message}"
                }
            }
            actionBusy = false
            if (failure != null) error = failure else selectedIds = emptySet()
            refresh()
        }
    }

    // Attaches an existing tag to every selected item. If they all already
    // have it, the same tap takes it off.
    fun toggleTagOnSelected(tag: TagEntry) {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Change tags on the PC."
            showTags = false
            return
        }
        val targets = videos.filter { it.id in selectedIds }
        if (targets.isEmpty()) return
        val allHave = targets.all { item -> item.tags.any { it.equals(tag.name, ignoreCase = true) } }
        actionBusy = true
        error = null
        scope.launch {
            var failure: String? = null
            for (target in targets) {
                val has = target.tags.any { it.equals(tag.name, ignoreCase = true) }
                if (allHave) {
                    if (!has) continue
                } else if (has) {
                    continue
                }
                val res = withContext(Dispatchers.IO) {
                    val raw = if (allHave) {
                        CoreBridge.nativeRemoveTag(target.id, tag.id)
                    } else {
                        CoreBridge.nativeAddTag(target.id, tag.name)
                    }
                    runCatching { CoreBridge.Result.parse(raw) }
                        .getOrElse { CoreBridge.Result(false, "Tag failed", it.message ?: "") }
                }
                if (!res.ok) {
                    failure = res.error.ifBlank { res.detail }
                    break
                }
            }
            actionBusy = false
            showTags = false
            if (failure != null) error = failure
            refresh()
        }
    }

    // Export: the core streams the plaintext into the app cache, which is then
    // copied to the document the user picked and deleted again.
    fun exportVideo(target: VideoEntry, destination: Uri) {
        if (RemoteVault.connected()) {
            actionMessage = "This vault is open on your PC. Export it on the PC."
            return
        }
        actionBusy = true
        actionMessage = null
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    val raw = CoreBridge.nativeRestoreVideo(target.id, context.cacheDir.absolutePath)
                    val obj = JSONObject(raw)
                    if (!obj.optBoolean("ok")) {
                        obj.optString("error").ifBlank { obj.optString("detail", "Export failed") }
                    } else {
                        val plain = File(obj.optString("path"))
                        val opened = context.contentResolver.openOutputStream(destination)
                        if (opened == null) {
                            "Could not open the destination"
                        } else {
                            opened.use { out -> plain.inputStream().use { it.copyTo(out) } }
                            plain.delete()
                            null
                        }
                    }
                } catch (e: Exception) {
                    "Export failed: ${e.message}"
                }
            }
            actionBusy = false
            if (failure == null) actionTarget = null else actionMessage = failure
        }
    }

    var pendingRelease by remember { mutableStateOf<AppRelease?>(null) }
    var updateProgress by remember { mutableStateOf<Float?>(null) }
    var packagePlan by remember { mutableStateOf<PackagePlan?>(null) }
    var armedPackage by remember { mutableStateOf<PackagePlan?>(null) }
    var packagePassword by remember { mutableStateOf("") }
    var packageCompress by remember { mutableStateOf(true) }
    var packageImportUri by remember { mutableStateOf<Uri?>(null) }
    LaunchedEffect(Unit) {
        pendingRelease = withContext(Dispatchers.IO) {
            runCatching { newerRelease(installedVersion(context)) }.getOrNull()
        }
    }
    fun guardedPackage(): Boolean {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Use packages on the PC."
            return false
        }
        if (importProgress != null) return false
        return true
    }
    fun beginPackageExport(fileIds: Set<Long>, folderIds: Set<Long>) {
        if (!guardedPackage()) return
        val plan = planPackage(videos, folders, fileIds, folderIds)
        if (plan.error != null) {
            error = plan.error
            return
        }
        packageCompress = true
        packagePlan = plan
    }
    fun runPackageExport(plan: PackagePlan, password: String, compress: Boolean, destination: Uri) {
        importProgress = 0f
        importLabel = "Exporting package"
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                val staged = File(context.cacheDir, "export.mvpk")
                try {
                    val poll = launch {
                        while (isActive) {
                            val fraction = CoreBridge.nativePackageProgress()
                            if (fraction >= 0.0) importProgress = (fraction * 0.9).toFloat()
                            delay(150)
                        }
                    }
                    val raw = CoreBridge.nativeExportPackage(
                        staged.absolutePath,
                        password,
                        plan.files.map { it.first }.toLongArray(),
                        plan.files.map { it.second }.toTypedArray(),
                        plan.directories.toTypedArray(),
                        compress,
                    )
                    poll.cancel()
                    val parsed = CoreBridge.Result.parse(raw)
                    if (!parsed.ok) {
                        parsed.error.ifBlank { parsed.detail.ifBlank { "Export failed" } }
                    } else {
                        importLabel = "Saving package"
                        copyFileToUri(context, staged, destination) { sent, total ->
                            if (total > 0L) importProgress = 0.9f + 0.1f * sent.toFloat() / total.toFloat()
                        }
                        null
                    }
                } catch (e: Exception) {
                    e.message ?: "Export failed"
                } finally {
                    staged.delete()
                }
            }
            importProgress = null
            importLabel = ""
            packagePassword = ""
            if (failure != null) error = failure
        }
    }
    fun runPackageImport(uri: Uri, password: String) {
        importProgress = 0f
        importLabel = "Importing package"
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                val staged = File(context.cacheDir, "import.mvpk")
                try {
                    copyUriToFile(context, uri, staged) { sent, total ->
                        if (total > 0L) importProgress = 0.15f * sent.toFloat() / total.toFloat()
                    }
                    val poll = launch {
                        while (isActive) {
                            val fraction = CoreBridge.nativePackageProgress()
                            if (fraction >= 0.0) importProgress = 0.15f + (fraction * 0.85).toFloat()
                            delay(150)
                        }
                    }
                    val raw = CoreBridge.nativeImportPackage(staged.absolutePath, password, currentFolderId)
                    poll.cancel()
                    val parsed = CoreBridge.Result.parse(raw)
                    if (!parsed.ok) parsed.error.ifBlank { parsed.detail.ifBlank { "Import failed" } } else null
                } catch (e: Exception) {
                    e.message ?: "Import failed"
                } finally {
                    staged.delete()
                }
            }
            importProgress = null
            importLabel = ""
            packagePassword = ""
            if (failure != null) error = failure else refresh()
        }
    }
    fun startAppUpdate() {
        val release = pendingRelease ?: return
        if (updateProgress != null) return
        updateProgress = 0f
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    val apk = File(context.cacheDir, "update.apk")
                    downloadRelease(release, apk) { updateProgress = it }
                    updateProgress = 1f
                    installRelease(context, apk)
                    null
                } catch (e: Exception) {
                    e.message ?: "Update failed"
                }
            }
            if (failure != null) {
                updateProgress = null
                error = failure
            }
        }
    }

    var showAdd by remember { mutableStateOf(false) }

    // The system photo picker caps a selection. Ask for its own maximum
    // instead of a small fixed count.
    val galleryLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.PickMultipleVisualMedia()
    ) { uris: List<Uri> -> startImport(uris) }

    val musicLauncher = rememberLauncherForActivityResult(
        OpenDocumentsAtRoot()
    ) { uris: List<Uri> -> startImport(uris) }

    // Documents cover containers the gallery does not index (Matroska, files
    // from another app or a cloud provider).
    val fileLauncher = rememberLauncherForActivityResult(
        OpenDocumentsAtRoot()
    ) { uris: List<Uri> -> startImport(uris) }

    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri: Uri? ->
        val target = actionTarget
        if (uri != null && target != null) exportVideo(target, uri)
    }
    val savePackageLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri: Uri? ->
        val plan = armedPackage
        val password = packagePassword
        val compress = packageCompress
        armedPackage = null
        if (uri != null && plan != null && password.isNotEmpty()) {
            runPackageExport(plan, password, compress, uri)
        } else {
            packagePassword = ""
        }
    }
    val openPackageLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) packageImportUri = uri
    }

    val restoreLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        if (uri != null) restoreSelected(uri)
    }

    val haze = rememberHazeState()
    val density = LocalDensity.current
    var topInset by remember { mutableStateOf(0) }
    var bottomInset by remember { mutableStateOf(0) }
    val contentPad = PaddingValues(
        top = with(density) { topInset.toDp() },
        bottom = with(density) { bottomInset.toDp() },
    )
    val glass = HazeStyle(
        backgroundColor = Color(0xC4101014),
        tints = listOf(HazeTint(Color.Black.copy(alpha = 0.28f))),
        blurRadius = 32.dp,
        noiseFactor = 0.03f,
    )

    var folderForward by remember { mutableStateOf(true) }
    fun openFolder(id: Long) {
        if (id == PEER_FOLDER_ID) {
            selectedIds = emptySet()
            if (PeerLink.linked()) onOpenPeer() else error = "The PC is no longer connected."
            return
        }
        val leaving = folders.firstOrNull { it.id == currentFolderId }
        folderForward = when {
            leaving?.parentId == id -> false
            id == 0L && currentFolderId != 0L -> false
            else -> true
        }
        currentFolderId = id
        GalleryCache.currentFolderId = id
        selectedIds = emptySet()
    }

    var showMusic by remember { mutableStateOf(false) }
    var lastExitPress by remember { mutableStateOf(0L) }
    BackHandler {
        if (showMusic) {
            showMusic = false
            return@BackHandler
        }
        if (showSettings) {
            showSettings = false
            return@BackHandler
        }
        if (selectedIds.isNotEmpty()) {
            lastExitPress = 0L
            selectedIds = emptySet()
            return@BackHandler
        }
        if (currentFolderId != 0L) {
            lastExitPress = 0L
            val parent = folders.firstOrNull { it.id == currentFolderId }?.parentId ?: 0L
            openFolder(parent)
            return@BackHandler
        }
        val now = SystemClock.elapsedRealtime()
        if (now - lastExitPress < 2000L) {
            onLock()
        } else {
            lastExitPress = now
            Toast.makeText(context, "Press back again to lock", Toast.LENGTH_SHORT).show()
        }
    }

    fun moveDragged(drag: VaultDrag, destination: Long) {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Move it on the PC."
            return
        }
        if (destination == PEER_FOLDER_ID) {
            error = "The PC's library is read-only. Open it to copy from it."
            return
        }
        val movingFolders = drag.folders.filter { id ->
            id > 0L && (folders.firstOrNull { it.id == id }?.parentId ?: 0L) != destination
        }
        val movingVideos = drag.videos.filter { id ->
            videos.firstOrNull { it.id == id }?.folderId != destination
        }
        if (movingFolders.isEmpty() && movingVideos.isEmpty()) return
        scope.launch {
            val problem = withContext(Dispatchers.Default) {
                var message = ""
                for (folderId in movingFolders) {
                    if (folderContains(folders, folderId, destination)) {
                        message = "A folder can't be moved into itself."
                        continue
                    }
                    val result = CoreBridge.Result.parse(CoreBridge.nativeMoveFolder(folderId, destination))
                    if (!result.ok) message = result.detail.ifBlank { result.error }
                }
                for (videoId in movingVideos) {
                    val result = CoreBridge.Result.parse(CoreBridge.nativeMoveVideo(videoId, destination))
                    if (!result.ok) message = result.detail.ifBlank { result.error }
                }
                message
            }
            if (problem.isNotEmpty()) error = problem
            selectedIds = emptySet()
            refresh()
        }
    }

    fun handleDrop(event: DragAndDropEvent, destination: Long): Boolean {
        if (destination == PEER_FOLDER_ID) {
            error = "The PC's library is read-only. Open it to copy from it."
            return true
        }
        val dragEvent = event.toAndroidDragEvent()
        val local = dragEvent.localState as? VaultDrag
        if (local != null) {
            moveDragged(local, destination)
            return true
        }
        val clip = dragEvent.clipData
        val uris = ArrayList<Uri>()
        if (clip != null) {
            for (index in 0 until clip.itemCount) {
                clip.getItemAt(index).uri?.let { uris.add(it) }
            }
        }
        if (uris.isNotEmpty()) {
            val permissions = activityOf(context)?.requestDragAndDropPermissions(dragEvent)
            startImport(uris, destination, permissions)
            return true
        }
        val parsed = clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.text?.toString()?.let { parseVaultDrag(it) }
        if (parsed != null) {
            moveDragged(parsed, destination)
            return true
        }
        return false
    }

    val dragVideo: (VideoEntry) -> DragAndDropTransferData = { video ->
        val ids = if (video.id in selectedIds) selectedIds.toList() else listOf(video.id)
        vaultDragData(VaultDrag(ids, emptyList()))
    }
    val dropHere: (DragAndDropEvent) -> Boolean = { handleDrop(it, currentFolderId) }

    if (scanningPc) {
        QrScanScreen(
            onCode = { code ->
                scanningPc = false
                startSwap(code)
            },
            onClose = { scanningPc = false },
        )
        return
    }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black),
    ) {
        if (showTransfers) {
            TransferPage(onClose = { showTransfers = false })
        }
        AnimatedVisibility(
            visible = showSettings,
            enter = fadeIn(tween(220)) + slideInVertically(tween(220)) { it / 10 },
            exit = fadeOut(tween(160)) + slideOutVertically(tween(160)) { it / 10 },
        ) {
            SettingsDialog(
                currentTags = tags,
                remote = RemoteVault.connected(),
                onDismiss = { showSettings = false },
                onChanged = { refresh() },
            )
        }

        val searching = search.isNotBlank()
        val scope = if (searching) folderSubtree(folders, currentFolderId) else emptySet()
        val filtered = videos.filter { v ->
            val here = if (searching) v.folderId in scope else v.folderId == currentFolderId
            here &&
                (filterTagId == 0L || v.tags.any { t -> tags.firstOrNull { it.id == filterTagId }?.name?.equals(t, true) == true }) &&
                (!searching || v.name.contains(search, ignoreCase = true))
        }
        val childFolders = folders.filter { folder ->
            if (!searching) {
                folder.parentId == currentFolderId
            } else {
                folder.id != currentFolderId &&
                    folder.id in scope &&
                    folder.name.contains(search, ignoreCase = true)
            }
        }
        val videoPlace = if (!searching) {
            emptyMap()
        } else {
            buildMap {
                for (v in filtered) {
                    val place = folderPathFrom(folders, v.folderId, currentFolderId)
                    if (place.isNotEmpty()) put(v.id, place)
                }
            }
        }
        val folderPlace = if (!searching) {
            emptyMap()
        } else {
            buildMap {
                for (folder in childFolders) {
                    val place = folderPathFrom(folders, folder.parentId, currentFolderId)
                    if (place.isNotEmpty()) put(folder.id, place)
                }
            }
        }
        val copying = importProgress != null || TransferHub.items.any {
            it.state == TransferState.Waiting || it.state == TransferState.Copying
        }
        LaunchedEffect(copying, incomingFiles.size, incomingFolders.size) {
            if (!copying && (incomingFiles.isNotEmpty() || incomingFolders.isNotEmpty())) {
                incomingFiles.clear()
                incomingFolders.clear()
            }
        }
        val sortKey = GalleryCache.sortKey
        val sortAscending = GalleryCache.sortAscending
        val mediaVersion = GalleryCache.mediaVersion
        val mediaSort = sortKey == SortKey.Duration ||
            sortKey == SortKey.Resolution ||
            sortKey == SortKey.Codec
        // Reordering on every probe made rows fly in and out of the lazy list,
        // and the click landed on whatever had just moved. Wait until the
        // burst of probes goes quiet, then place each row once.
        var sortEpoch by remember { mutableIntStateOf(mediaVersion) }
        LaunchedEffect(sortKey) {
            sortEpoch = GalleryCache.mediaVersion
        }
        LaunchedEffect(mediaVersion) {
            if (!mediaSort) {
                sortEpoch = mediaVersion
                return@LaunchedEffect
            }
            if (sortEpoch == mediaVersion) return@LaunchedEffect
            delay(450)
            sortEpoch = GalleryCache.mediaVersion
        }
        val settling = incomingFiles.isNotEmpty() || incomingFolders.isNotEmpty()
        val shown = remember(
            filtered,
            sortKey,
            sortAscending,
            if (settling) 0 else if (mediaSort) sortEpoch else mediaVersion,
            incomingFiles.toList(),
        ) {
            fun directed(value: Int): Int = if (sortAscending) value else -value
            // Missing container facts stay at the end in both directions so a
            // row does not jump from the top the moment its duration arrives.
            fun unknownLast(aMissing: Boolean, bMissing: Boolean, value: () -> Int): Int = when {
                aMissing && bMissing -> 0
                aMissing -> 1
                bMissing -> -1
                else -> value()
            }
            val cmp = Comparator<VideoEntry> { a, b ->
                val primary = when (sortKey) {
                    SortKey.Name -> directed(a.name.compareTo(b.name, ignoreCase = true))
                    SortKey.Size -> directed(a.size.compareTo(b.size))
                    SortKey.Duration -> {
                        val ad = GalleryCache.media[a.id]?.durationMs ?: 0L
                        val bd = GalleryCache.media[b.id]?.durationMs ?: 0L
                        unknownLast(ad <= 0L, bd <= 0L) { directed(ad.compareTo(bd)) }
                    }
                    SortKey.Resolution -> {
                        fun pixels(v: VideoEntry) =
                            GalleryCache.media[v.id]?.let { it.width.toLong() * it.height.toLong() } ?: 0L
                        val ap = pixels(a)
                        val bp = pixels(b)
                        unknownLast(ap <= 0L, bp <= 0L) { directed(ap.compareTo(bp)) }
                    }
                    SortKey.Codec -> {
                        val ac = GalleryCache.media[a.id]?.codec.orEmpty()
                        val bc = GalleryCache.media[b.id]?.codec.orEmpty()
                        unknownLast(ac.isEmpty(), bc.isEmpty()) {
                            directed(ac.compareTo(bc, ignoreCase = true))
                        }
                    }
                    SortKey.Tags ->
                        directed(a.tags.joinToString(",").compareTo(b.tags.joinToString(","), ignoreCase = true))
                    SortKey.Imported -> directed(a.importedAt.compareTo(b.importedAt))
                }
                if (primary != 0) primary else a.name.compareTo(b.name, ignoreCase = true)
            }
            val ranked = filtered.sortedWith(cmp)
            if (!settling) {
                ranked
            } else {
                val fresh = incomingFiles.toSet()
                ranked.filter { it.id !in fresh } + filtered.filter { it.id in fresh }
            }
        }

        val selecting = selectedIds.isNotEmpty()
        val openActions: (VideoEntry) -> Unit = { v ->
            actionMessage = null
            actionTarget = v
        }
        val openItem: (VideoEntry) -> Unit = { v ->
            if (selecting) {
                selectedIds = if (v.id in selectedIds) selectedIds - v.id else selectedIds + v.id
            } else {
                onPlay(v.id, v.name, shown)
            }
        }
        val pressItem: (VideoEntry) -> Unit = { v ->
            selectedIds = if (v.id in selectedIds) selectedIds - v.id else selectedIds + v.id
        }

        val upParentId = if (currentFolderId == 0L) {
            null
        } else {
            folders.firstOrNull { it.id == currentFolderId }?.parentId ?: 0L
        }
        val orderedFolders = remember(
            childFolders,
            sortKey,
            sortAscending,
            pcName,
            currentFolderId,
            search,
            incomingFolders.toList(),
        ) {
            val fresh = incomingFolders.toSet()
            val older = if (fresh.isEmpty()) childFolders else childFolders.filter { it.id !in fresh }
            val byName = older.sortedBy { it.name.lowercase() }
            val sorted = if (sortKey == SortKey.Name && !sortAscending) byName.asReversed() else byName
            val arrived = childFolders.filter { it.id in fresh }
            val pcFolder = pcName
                ?.let { "PC · $it" }
                ?.takeIf { currentFolderId == 0L && (search.isBlank() || it.contains(search, ignoreCase = true)) }
                ?.let { FolderEntry(PEER_FOLDER_ID, 0L, it) }
            val library = if (pcFolder != null) listOf(pcFolder) + sorted else sorted
            library + arrived
        }
        val libraryFade = remember { Animatable(1f) }
        val libraryShift = remember { Animatable(0f) }
        val libraryScale = remember { Animatable(1f) }
        var skipFolderMotion by remember { mutableStateOf(true) }
        var skipViewMotion by remember { mutableStateOf(true) }
        LaunchedEffect(currentFolderId) {
            if (skipFolderMotion) {
                skipFolderMotion = false
                return@LaunchedEffect
            }
            libraryShift.snapTo(if (folderForward) 56f else -56f)
            libraryFade.snapTo(0.4f)
            libraryScale.snapTo(1f)
            launch { libraryShift.animateTo(0f, tween(280, easing = FastOutSlowInEasing)) }
            launch { libraryFade.animateTo(1f, tween(220)) }
        }
        LaunchedEffect(viewMode) {
            if (skipViewMotion) {
                skipViewMotion = false
                return@LaunchedEffect
            }
            libraryShift.snapTo(0f)
            libraryScale.snapTo(0.97f)
            libraryFade.snapTo(0.55f)
            launch { libraryScale.animateTo(1f, tween(220, easing = FastOutSlowInEasing)) }
            launch { libraryFade.animateTo(1f, tween(200)) }
        }
        Box(
            Modifier
                .fillMaxSize()
                .graphicsLayer {
                    alpha = libraryFade.value
                    translationX = libraryShift.value * this.density
                    scaleX = libraryScale.value
                    scaleY = libraryScale.value
                }
                .hazeSource(state = haze),
        ) {
            when (viewMode) {
                0 -> DetailsView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                    videoPlace, folderPlace,
                )
                1 -> IconGridView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                    videoPlace, folderPlace,
                )
                else -> ListView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                    videoPlace, folderPlace,
                )
            }
        }

        Column(
            Modifier
                .align(Alignment.TopCenter)
                .fillMaxWidth()
                .onSizeChanged { topInset = it.height }
                .hazeEffect(state = haze, style = glass)
                .statusBarsPadding(),
        ) {
            GalleryHeader(
                viewMode = viewMode,
                tags = tags,
                filterTagId = filterTagId,
                search = search,
                selectionTitle = when (selectedIds.size) {
                    0 -> null
                    1 -> videos.firstOrNull { it.id in selectedIds }?.name ?: "1 file"
                    else -> "${selectedIds.size} files"
                },
                actionsEnabled = !actionBusy,
                onViewMode = { mode ->
                    viewMode = mode
                    GalleryCache.viewMode = mode
                },
                onFilterTag = { id ->
                    filterTagId = id
                    GalleryCache.filterTagId = id
                },
                onSearch = { text ->
                    search = text
                    GalleryCache.search = text
                },
                onMusic = { showMusic = true },
                onUpdate = { startAppUpdate() },
                updateReady = pendingRelease != null && updateProgress == null,
                onItemMenu = if (selectedIds.size == 1) {
                    {
                        videos.firstOrNull { it.id == selectedIds.first() }?.let { openActions(it) }
                    }
                } else {
                    null
                },
                onImport = import@{
                    if (RemoteVault.connected()) {
                        error = "This vault is open on your PC. Add it on the PC."
                        return@import
                    }
                    showAdd = true
                },
                onSettings = { showSettings = true },
                onLock = onLock,
                onMove = {
                    if (RemoteVault.connected()) {
                        error = "This vault is open on your PC. Move it on the PC."
                    } else {
                        moveTarget = MoveRequest.Videos(selectedIds.toList())
                    }
                },
                onRemove = { confirmRemove = true },
                onRestore = { restoreLauncher.launch(FolderPicker.storageRootUri()) },
                onTag = { showTags = true },
                onClearSelection = { selectedIds = emptySet() },
            )
            CloudBar()
            TelegramMusicBar(onOpen = onOpenMusic)
            FolderBar(
                folders = folders,
                currentFolderId = currentFolderId,
                onOpen = { openFolder(it) },
                onDrop = { event, folderId -> handleDrop(event, folderId) },
            )
            importProgress?.let { fraction ->
                Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp)) {
                    LinearProgressIndicator(
                        progress = { fraction.coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth(),
                        color = Mvp.primary,
                        trackColor = Mvp.sliderGroove,
                    )
                    Text(
                        "${(fraction * 100).toInt()}%  $importLabel",
                        color = Mvp.description,
                        fontSize = 11.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }

        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .onSizeChanged { bottomInset = it.height },
        ) {
        TransferBar { showTransfers = true }
        Row(
            Modifier
                .fillMaxWidth()
                .hazeEffect(state = haze, style = glass)
                .navigationBarsPadding()
                .padding(horizontal = 16.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(vaultLocation, color = Color.White.copy(alpha = 0.86f), fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            Text("${childFolders.size + filtered.size} items", color = Color.White.copy(alpha = 0.86f), fontSize = 12.sp)
        }
        }

        if (updateProgress != null) {
            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(16.dp)
                    .background(Color(0xFF1C1C1E), RoundedCornerShape(14.dp))
                    .padding(14.dp),
            ) {
                Text("Updating…", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                LinearProgressIndicator(
                    progress = { (updateProgress ?: 0f).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                    color = Mvp.accent,
                    trackColor = Mvp.inputBg,
                )
            }
        }
        if (showMusic) {
            MusicLibrary(
                videos = videos,
                folders = folders,
                onClose = { showMusic = false },
                onPlay = { id, name, playlist ->
                    showMusic = false
                    onPlay(id, name, playlist)
                },
            )
        }

        error?.let { message ->
            LaunchedEffect(message) {
                delay(4200)
                if (error == message) error = null
            }
            Text(
                message,
                color = Color.White,
                fontSize = 13.sp,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = with(density) { bottomInset.toDp() } + 10.dp)
                    .padding(horizontal = 28.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .hazeEffect(state = haze, style = glass)
                    .border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(16.dp))
                    .clickable { error = null }
                    .padding(horizontal = 16.dp, vertical = 10.dp),
            )
        }
    }

    if (showTags) {
        val chosen = videos.filter { it.id in selectedIds }
        Dialog(onDismissRequest = { if (!actionBusy) showTags = false }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Tag")
                MvpDescription(
                    if (tags.isEmpty()) "No tags yet. Create one in Settings."
                    else "Tap a tag to add it to the selection. A tag already on every selected item is removed.",
                )
                if (actionBusy) {
                    CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                }
                Column(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(max = 240.dp)
                        .verticalScroll(rememberScrollState()),
                ) {
                FlowRow(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    tags.forEach { tag ->
                        val onAll = chosen.isNotEmpty() && chosen.all { item ->
                            item.tags.any { it.equals(tag.name, ignoreCase = true) }
                        }
                        GlassChip(
                            text = if (onAll) "✓  ${tag.name}" else tag.name,
                            selected = onAll,
                            onClick = { toggleTagOnSelected(tag) },
                            enabled = !actionBusy,
                        )
                    }
                }
                }
                Text(
                    "Done",
                    color = Mvp.accent,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .align(Alignment.CenterHorizontally)
                        .clickable(enabled = !actionBusy) { showTags = false }
                        .padding(vertical = 4.dp),
                )
            }
        }
    }

    if (confirmRemove) {
        Dialog(onDismissRequest = { if (!actionBusy) confirmRemove = false }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Remove ${selectedIds.size} item(s)?")
                MvpDescription("Removal deletes the encrypted packages. This cannot be undone.")
                MvpButton(
                    "Remove",
                    onClick = { removeSelected() },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Cancel",
                    onClick = { confirmRemove = false },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }

    // Per-item action sheet: remove (permanent) or export a plaintext copy.
    actionTarget?.let { target ->
        Dialog(onDismissRequest = { if (!actionBusy) actionTarget = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle(target.name)
                MvpDescription("${target.sizeText} · ${target.tags.joinToString(", ")}")
                MvpError(actionMessage)
                if (actionBusy) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                        Text("Working…", color = Mvp.description, fontSize = 12.sp)
                    }
                }
                MvpButton(
                    "Remove from vault",
                    onClick = { removeVideo(target) },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpDescription("Removal deletes the encrypted package — this cannot be undone.")
                MvpButton(
                    "Rename",
                    onClick = {
                        if (RemoteVault.connected()) {
                            actionMessage = "This vault is open on your PC. Rename it on the PC."
                        } else {
                            val video = target
                            actionTarget = null
                            namePrompt = NamePrompt.RenameVideo(video.id, video.name)
                        }
                    },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Move to folder…",
                    onClick = {
                        if (RemoteVault.connected()) {
                            actionMessage = "This vault is open on your PC. Move it on the PC."
                        } else {
                            moveTarget = MoveRequest.Videos(listOf(target.id))
                            actionTarget = null
                        }
                    },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Export as package…",
                    onClick = {
                        val video = target
                        actionTarget = null
                        beginPackageExport(setOf(video.id), emptySet())
                    },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Export a plaintext copy…",
                    onClick = { exportLauncher.launch(target.name) },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Cancel",
                    onClick = { actionTarget = null },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }

    folderMenu?.takeIf { it.id == PEER_FOLDER_ID }?.let { folder ->
        Dialog(onDismissRequest = { folderMenu = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle(folder.name)
                MvpDescription(
                    "The PC's library, shared with you. You can look and copy, but not change it. " +
                        "The PC sees your library the same way."
                )
                MvpButton("Open", onClick = { folderMenu = null; openFolder(folder.id) }, primary = true, modifier = Modifier.fillMaxWidth())
                MvpButton("Disconnect PC", onClick = {
                    folderMenu = null
                    PeerLink.unlink()
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { folderMenu = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    folderMenu?.takeIf { it.id != PEER_FOLDER_ID }?.let { folder ->
        Dialog(onDismissRequest = { folderMenu = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle(folder.name)
                MvpDescription("Removing a folder deletes the files and folders inside it.")
                MvpButton("Open", onClick = { folderMenu = null; openFolder(folder.id) }, modifier = Modifier.fillMaxWidth())
                MvpButton("Rename", onClick = {
                    folderMenu = null
                    if (RemoteVault.connected()) error = "This vault is open on your PC. Rename the folder on the PC."
                    else namePrompt = NamePrompt.RenameFolder(folder.id, folder.name)
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Export as package…", onClick = {
                    val folder = folder
                    folderMenu = null
                    beginPackageExport(emptySet(), setOf(folder.id))
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Move to…", onClick = {
                    folderMenu = null
                    if (RemoteVault.connected()) error = "This vault is open on your PC. Move the folder on the PC."
                    else moveTarget = MoveRequest.Folder(folder.id)
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Remove folder", onClick = {
                    folderMenu = null
                    if (RemoteVault.connected() && !RemoteVault.cloud) {
                        error = "This vault is open on your PC. Remove the folder on the PC."
                    } else {
                        folderDelete = folder
                    }
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { folderMenu = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    folderDelete?.let { folder ->
        val tree = folderSubtree(folders, folder.id)
        val inside = videos.count { it.folderId in tree }
        Dialog(onDismissRequest = { if (!actionBusy) folderDelete = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Remove \"${folder.name}\"?")
                MvpDescription(
                    if (inside > 0) {
                        "Delete $inside file(s) inside this folder? This cannot be undone."
                    } else {
                        "This folder is empty."
                    }
                )
                MvpButton(
                    if (inside > 0) "Delete" else "Remove folder",
                    onClick = {
                        scope.launch {
                            actionBusy = true
                            val result = withContext(Dispatchers.IO) {
                                if (RemoteVault.cloud) {
                                    val failure = RemoteVault.remove("/v1/remove-folder?id=${folder.id}")
                                    if (failure == null) CoreBridge.Result(true, "", "")
                                    else CoreBridge.Result(false, failure, failure)
                                } else {
                                    CoreBridge.Result.parse(CoreBridge.nativeRemoveFolder(folder.id))
                                }
                            }
                            actionBusy = false
                            folderDelete = null
                            if (!result.ok) {
                                error = result.detail.ifBlank { result.error }
                            } else {
                                if (folderContains(folders, folder.id, currentFolderId)) {
                                    openFolder(folder.parentId)
                                }
                                refresh()
                            }
                        }
                    },
                    enabled = !actionBusy,
                    primary = inside > 0,
                    modifier = Modifier.fillMaxWidth(),
                )
                MvpButton(
                    "Cancel",
                    onClick = { folderDelete = null },
                    enabled = !actionBusy,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }

    var mediaPick by remember { mutableStateOf<DevicePick?>(null) }
    if (showAdd) {
        Dialog(onDismissRequest = { showAdd = false }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Add")
                MvpButton("Gallery", onClick = {
                    showAdd = false
                    mediaPick = DevicePick.Gallery
                }, primary = true, modifier = Modifier.fillMaxWidth())
                MvpButton("Music", onClick = {
                    showAdd = false
                    mediaPick = DevicePick.Music
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Browse storage", onClick = {
                    showAdd = false
                    fileLauncher.launch(arrayOf("*/*"))
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Import package…", onClick = {
                    showAdd = false
                    if (guardedPackage()) openPackageLauncher.launch(arrayOf("*/*"))
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Export package…", onClick = {
                    showAdd = false
                    beginPackageExport(selectedIds, emptySet())
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("New folder", onClick = {
                    showAdd = false
                    namePrompt = NamePrompt.NewFolder(currentFolderId)
                }, modifier = Modifier.fillMaxWidth())
                MvpButton(if (pcName == null) "Swap with PC" else "Open PC · $pcName", onClick = {
                    showAdd = false
                    when {
                        pcName != null -> onOpenPeer()
                        RemoteVault.connected() -> error = "This vault is already open on your PC."
                        else -> pcCamera.launch(Manifest.permission.CAMERA)
                    }
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { showAdd = false }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    mediaPick?.let { kind ->
        DeviceLibraryScreen(
            kind = kind,
            onClose = { mediaPick = null },
            onBrowse = {
                mediaPick = null
                if (kind == DevicePick.Music) {
                    musicLauncher.launch(arrayOf("audio/*"))
                } else {
                    galleryLauncher.launch(
                        PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageAndVideo)
                    )
                }
            },
            onImport = { uris ->
                mediaPick = null
                startImport(uris)
            },
        )
    }

    if (linkingPc) {
        Dialog(onDismissRequest = {}) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Swap with PC")
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
                    Text(
                        "Waiting for the PC… Accept the swap on the PC.",
                        color = Mvp.description,
                        fontSize = 13.sp,
                    )
                }
            }
        }
    }

    packagePlan?.let { plan ->
        var password by remember { mutableStateOf("") }
        var confirm by remember { mutableStateOf("") }
        var compress by remember { mutableStateOf(true) }
        var problem by remember { mutableStateOf<String?>(null) }
        Dialog(onDismissRequest = { packagePlan = null; packagePassword = "" }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Export package")
                MvpDescription(problem ?: "This password locks the package. It is separate from the vault password.")
                MvpInput(password, { password = it }, "Package password", isPassword = true)
                MvpInput(confirm, { confirm = it }, "Confirm password", isPassword = true)
                Text(
                    if (compress) "Compress the files  ✓" else "Compress the files",
                    color = if (compress) Mvp.accent else Mvp.description,
                    fontSize = 14.sp,
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { compress = !compress }
                        .padding(vertical = 6.dp),
                )
                MvpButton("Export", onClick = {
                    if (password.isEmpty()) {
                        problem = "Enter a password."
                        return@MvpButton
                    }
                    if (password != confirm) {
                        problem = "The password confirmation does not match."
                        return@MvpButton
                    }
                    packagePassword = password
                    packageCompress = compress
                    armedPackage = plan
                    packagePlan = null
                    savePackageLauncher.launch(plan.suggestion + ".mvpk")
                }, primary = true, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { packagePlan = null; packagePassword = "" }, modifier = Modifier.fillMaxWidth())
            }
        }
    }
    packageImportUri?.let { uri ->
        var password by remember { mutableStateOf("") }
        var problem by remember { mutableStateOf<String?>(null) }
        Dialog(onDismissRequest = { packageImportUri = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Import package")
                MvpDescription(problem ?: "Enter the password that was set when this package was created.")
                MvpInput(password, { password = it }, "Package password", isPassword = true)
                MvpButton("Import", onClick = {
                    if (password.isEmpty()) {
                        problem = "Enter a password."
                        return@MvpButton
                    }
                    val chosen = uri
                    val typed = password
                    packageImportUri = null
                    runPackageImport(chosen, typed)
                }, primary = true, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { packageImportUri = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    namePrompt?.let { prompt ->
        var draft by remember(prompt) { mutableStateOf(prompt.initial) }
        Dialog(onDismissRequest = { namePrompt = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle(prompt.title)
                MvpInput(draft, { draft = it }, "Name")
                MvpButton("Save", onClick = {
                    val typed = draft.trim()
                    scope.launch {
                        val result = withContext(Dispatchers.Default) {
                            CoreBridge.Result.parse(
                                when (prompt) {
                                    is NamePrompt.NewFolder -> CoreBridge.nativeCreateFolder(prompt.parentId, typed)
                                    is NamePrompt.RenameFolder -> CoreBridge.nativeRenameFolder(prompt.id, typed)
                                    is NamePrompt.RenameVideo -> CoreBridge.nativeRenameVideo(prompt.id, typed)
                                }
                            )
                        }
                        if (!result.ok) {
                            error = result.detail.ifBlank { result.error }
                        } else {
                            namePrompt = null
                            refresh()
                        }
                    }
                }, modifier = Modifier.fillMaxWidth())
                MvpButton("Cancel", onClick = { namePrompt = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }

    moveTarget?.let { request ->
        val blocked = when (request) {
            is MoveRequest.Folder -> folderSubtree(folders, request.id)
            is MoveRequest.Videos -> emptySet()
        }
        val choices = ArrayList<Pair<Long, String>>()
        if (0L !in blocked) choices.add(0L to "Library")
        folders.filter { it.id !in blocked }.sortedBy { it.name.lowercase() }.forEach { folder ->
            choices.add(folder.id to folderPath(folders, folder.id))
        }
        Dialog(onDismissRequest = { moveTarget = null }) {
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Move to")
                Column(
                    Modifier.fillMaxWidth().heightIn(max = 320.dp).verticalScroll(rememberScrollState()),
                ) {
                    choices.forEach { (id, label) ->
                        Text(
                            label,
                            color = Mvp.text,
                            fontSize = 15.sp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable {
                                    val chosen = id
                                    val moving = request
                                    moveTarget = null
                                    scope.launch {
                                        val result = withContext(Dispatchers.Default) {
                                            when (moving) {
                                                is MoveRequest.Folder ->
                                                    CoreBridge.Result.parse(CoreBridge.nativeMoveFolder(moving.id, chosen))
                                                is MoveRequest.Videos -> {
                                                    var last = CoreBridge.Result(true, "", "")
                                                    for (videoId in moving.ids) {
                                                        last = CoreBridge.Result.parse(CoreBridge.nativeMoveVideo(videoId, chosen))
                                                        if (!last.ok) break
                                                    }
                                                    last
                                                }
                                            }
                                        }
                                        if (!result.ok) error = result.detail.ifBlank { result.error }
                                        else refresh()
                                    }
                                }
                                .padding(vertical = 10.dp),
                        )
                    }
                }
                MvpButton("Cancel", onClick = { moveTarget = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}

/** Every song in the vault, from every folder, with the folder it lives in. */
@Composable
private fun MusicLibrary(
    videos: List<VideoEntry>,
    folders: List<FolderEntry>,
    onClose: () -> Unit,
    onPlay: (Long, String, List<VideoEntry>) -> Unit,
) {
    var query by remember { mutableStateOf("") }
    var stamp by remember { mutableIntStateOf(0) }
    LaunchedEffect(videos) {
        val pending = videos.filter { FileKinds.peek(it.id) == null && isAudioName(it.name) }
        for (song in pending) {
            withContext(Dispatchers.IO) { FileKinds.resolve(song.id, song.name) }
            stamp++
        }
    }
    val songs = remember(videos, stamp) {
        videos.filter { song ->
            val known = FileKinds.peek(song.id)
            if (known != null) known.route == 2 else isAudioName(song.name)
        }.sortedBy { songLabel(it.name).second.lowercase() }
    }
    val shown = if (query.isBlank()) {
        songs
    } else {
        songs.filter { song ->
            song.name.contains(query, ignoreCase = true) ||
                folderPath(folders, song.folderId).contains(query, ignoreCase = true)
        }
    }
    Column(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(start = 8.dp, end = 16.dp, top = 8.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            HeaderIcon("Close", R.drawable.msg_close, onClose)
            Column(Modifier.weight(1f)) {
                Text("Music", color = Mvp.title, fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
                Text(
                    if (songs.isEmpty()) "No songs" else "${songs.size} songs",
                    color = Mvp.description,
                    fontSize = 13.sp,
                )
            }
            if (shown.isNotEmpty()) {
                Text(
                    "Play",
                    color = Mvp.accent,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .clip(RoundedCornerShape(16.dp))
                        .clickable { onPlay(shown.first().id, shown.first().name, shown) }
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                )
            }
        }
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp)
                .height(40.dp)
                .clip(RoundedCornerShape(20.dp))
                .background(Mvp.glass)
                .border(1.dp, Mvp.glassStroke, RoundedCornerShape(20.dp))
                .padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(
                painterResource(R.drawable.outline_header_search),
                contentDescription = null,
                tint = Mvp.placeholder,
                modifier = Modifier.size(18.dp),
            )
            Spacer(Modifier.width(8.dp))
            BasicTextField(
                value = query,
                onValueChange = { query = it },
                modifier = Modifier.weight(1f),
                singleLine = true,
                textStyle = TextStyle(color = Mvp.title, fontSize = 15.sp),
                cursorBrush = SolidColor(Mvp.accent),
                decorationBox = { inner ->
                    Box {
                        if (query.isEmpty()) {
                            Text("Search songs or folders", color = Mvp.placeholder, fontSize = 15.sp)
                        }
                        inner()
                    }
                },
            )
        }
        if (shown.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(
                    if (songs.isEmpty()) "No music in this vault yet." else "No matches.",
                    color = Mvp.description,
                    fontSize = 15.sp,
                )
            }
        } else {
            LazyColumn(Modifier.fillMaxSize()) {
                items(shown, key = { it.id }) { song ->
                    val (artist, title) = songLabel(song.name)
                    val place = folderPath(folders, song.folderId)
                    val subtitle = listOf(artist, place).filter { it.isNotBlank() }.joinToString("  ·  ")
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable { onPlay(song.id, song.name, shown) }
                            .padding(horizontal = 16.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        MusicNote(Modifier.size(44.dp))
                        Column(Modifier.weight(1f).padding(start = 12.dp)) {
                            Text(
                                title.ifBlank { song.name },
                                color = Mvp.text,
                                fontSize = 15.sp,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            if (subtitle.isNotEmpty()) {
                                Text(
                                    subtitle,
                                    color = Mvp.description,
                                    fontSize = 12.sp,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

/** Library title, icon actions, search, and a small view switch. */
@Composable
private fun GalleryHeader(
    viewMode: Int,
    tags: List<TagEntry>,
    filterTagId: Long,
    search: String,
    selectionTitle: String?,
    actionsEnabled: Boolean,
    onViewMode: (Int) -> Unit,
    onFilterTag: (Long) -> Unit,
    onSearch: (String) -> Unit,
    onMusic: () -> Unit,
    onUpdate: () -> Unit,
    updateReady: Boolean,
    onItemMenu: (() -> Unit)?,
    onImport: () -> Unit,
    onSettings: () -> Unit,
    onLock: () -> Unit,
    onMove: () -> Unit,
    onRemove: () -> Unit,
    onRestore: () -> Unit,
    onTag: () -> Unit,
    onClearSelection: () -> Unit,
) {
        Column(
        Modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 10.dp, top = 8.dp, bottom = 12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        if (selectionTitle != null) {
            Row(
                Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                HeaderIcon("Clear selection", R.drawable.msg_close, onClearSelection)
                Text(
                    selectionTitle,
                    color = Mvp.title,
                    fontSize = 18.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f).padding(horizontal = 4.dp),
                )
                if (onItemMenu != null) {
                    HeaderIcon("Actions", R.drawable.ic_ab_other, onItemMenu, enabled = actionsEnabled)
                }
                HeaderIcon("Move", R.drawable.msg_forward, onMove, enabled = actionsEnabled)
                HeaderIcon("Tag", R.drawable.menu_tag_plus, onTag, enabled = actionsEnabled)
                HeaderIcon("Restore", R.drawable.msg_download, onRestore, enabled = actionsEnabled)
                HeaderIcon("Remove", R.drawable.msg_delete_filled, onRemove, enabled = actionsEnabled, iconTint = Mvp.errorText)
            }
        } else {
        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                "Library",
                color = Mvp.title,
                fontSize = 22.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
            )
            if (updateReady) {
                HeaderIcon("Update", R.drawable.msg_download, onUpdate)
            }
            Text(
                "Music",
                color = Mvp.accent,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier
                    .clip(RoundedCornerShape(16.dp))
                    .clickable(onClick = onMusic)
                    .padding(horizontal = 10.dp, vertical = 6.dp),
            )
            HeaderIcon("Add", R.drawable.msg_add, onImport, filled = true)
            HeaderIcon("Settings", R.drawable.msg_settings, onSettings)
            HeaderIcon("Lock", R.drawable.outline_header_lock_24, onLock)
        }
        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(
                Modifier
                    .weight(1f)
                    .height(40.dp)
                    .clip(RoundedCornerShape(20.dp))
                    .background(Mvp.glass)
                    .border(1.dp, Mvp.glassStroke, RoundedCornerShape(20.dp))
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    painterResource(R.drawable.outline_header_search),
                    contentDescription = null,
                    tint = Mvp.placeholder,
                    modifier = Modifier.size(18.dp),
                )
                Spacer(Modifier.width(8.dp))
                BasicTextField(
                    value = search,
                    onValueChange = onSearch,
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    textStyle = TextStyle(color = Mvp.title, fontSize = 15.sp),
                    cursorBrush = SolidColor(Mvp.accent),
                    decorationBox = { inner ->
                        Box {
                            if (search.isEmpty()) {
                                Text("Search", color = Mvp.placeholder, fontSize = 15.sp)
                            }
                            inner()
                        }
                    },
                )
                if (search.isNotEmpty()) {
                    Box(
                        Modifier
                            .size(24.dp)
                            .clip(CircleShape)
                            .clickable { onSearch("") },
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(
                            painterResource(R.drawable.msg_close),
                            contentDescription = "Clear search",
                            tint = Mvp.description,
                            modifier = Modifier.size(14.dp),
                        )
                    }
                }
            }
            Row(
                Modifier
                    .clip(RoundedCornerShape(20.dp))
                    .background(Mvp.glass)
                    .border(1.dp, Mvp.glassStroke, RoundedCornerShape(20.dp))
                    .padding(2.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ViewModeButton(R.drawable.iv_list, "Details", viewMode == 0) { onViewMode(0) }
                ViewModeButton(R.drawable.msg_media, "Icons", viewMode == 1) { onViewMode(1) }
                ViewModeButton(R.drawable.msg_list, "List", viewMode == 2) { onViewMode(2) }
            }
            if (tags.isNotEmpty()) {
                TagFilterButton(tags, filterTagId, onFilterTag)
            }
        }
        }
    }
}

@Composable
private fun ViewModeButton(
    icon: Int,
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
) {
    val fill by animateColorAsState(
        if (selected) Mvp.selection else Color.Transparent,
        tween(180),
        label = "viewMode",
    )
    val tint by animateColorAsState(
        if (selected) Color.White else Mvp.description,
        tween(180),
        label = "viewModeTint",
    )
    Box(
        Modifier
            .size(32.dp)
            .clip(CircleShape)
            .background(fill)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            painterResource(icon),
            contentDescription = label,
            tint = tint,
            modifier = Modifier.size(18.dp),
        )
    }
}

@Composable
private fun TagFilterButton(
    tags: List<TagEntry>,
    filterTagId: Long,
    onFilterTag: (Long) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Box {
        ViewModeButton(R.drawable.menu_tag_filter, "Tags", filterTagId != 0L) { open = true }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(
                text = {
                    Text(
                        "All",
                        color = if (filterTagId == 0L) Mvp.accent else Mvp.text,
                        fontSize = 14.sp,
                    )
                },
                onClick = { open = false; onFilterTag(0L) },
            )
            tags.forEach { tag ->
                DropdownMenuItem(
                    text = {
                        Text(
                            tag.name,
                            color = if (filterTagId == tag.id) Mvp.accent else Mvp.text,
                            fontSize = 14.sp,
                        )
                    },
                    onClick = { open = false; onFilterTag(tag.id) },
                )
            }
        }
    }
}

@Composable
private fun HeaderIcon(
    label: String,
    icon: Int,
    onClick: () -> Unit,
    filled: Boolean = false,
    enabled: Boolean = true,
    iconTint: Color? = null,
) {
    val tint = iconTint ?: if (filled) Color.White else Mvp.title
    Box(
        Modifier
            .padding(horizontal = 2.dp)
            .size(36.dp)
            .clip(CircleShape)
            .background(if (filled) Mvp.primary else Color.Transparent)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            painterResource(icon),
            contentDescription = label,
            tint = if (enabled) tint else tint.copy(alpha = 0.35f),
            modifier = Modifier.size(22.dp),
        )
    }
}

private fun queryName(resolver: android.content.ContentResolver, uri: Uri): String? {
    return runCatching {
        resolver.query(uri, null, null, null, null)?.use { c ->
            val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx >= 0 && c.moveToFirst()) c.getString(idx) else null
        }
    }.getOrNull()
}

/**
 * Copies each picked document into the app cache (streamed in 64 KiB blocks so
 * the progress bar tracks real bytes) and imports it into the vault. Files are
 * processed one at a time — the vault allows a single writer — while the bar
 * spans the whole selection, so it never jumps per file.
 */
private sealed class NamePrompt(val title: String, val initial: String) {
    class NewFolder(val parentId: Long) : NamePrompt("New folder", "")
    class RenameFolder(val id: Long, name: String) : NamePrompt("Rename folder", name)
    class RenameVideo(val id: Long, name: String) : NamePrompt("Rename", name)
}

private sealed class MoveRequest {
    class Videos(val ids: List<Long>) : MoveRequest()
    class Folder(val id: Long) : MoveRequest()
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

private fun folderSubtree(folders: List<FolderEntry>, root: Long): Set<Long> {
    val ids = ArrayList<Long>()
    ids.add(root)
    var index = 0
    while (index < ids.size) {
        val parent = ids[index++]
        folders.filter { it.parentId == parent }.forEach { ids.add(it.id) }
    }
    return ids.toSet()
}

private fun folderPath(folders: List<FolderEntry>, folderId: Long): String {
    val parts = ArrayList<String>()
    var cursor = folderId
    for (guard in 0 until 64) {
        if (cursor == 0L) break
        val folder = folders.firstOrNull { it.id == cursor } ?: break
        parts.add(0, folder.name)
        cursor = folder.parentId
    }
    return if (parts.isEmpty()) "Library" else parts.joinToString(" / ")
}

/** Path of [folderId] walking up until [stopAt], which is not included. */
private fun folderPathFrom(folders: List<FolderEntry>, folderId: Long, stopAt: Long): String {
    val parts = ArrayList<String>()
    var cursor = folderId
    for (guard in 0 until 64) {
        if (cursor == 0L || cursor == stopAt) break
        val folder = folders.firstOrNull { it.id == cursor } ?: break
        parts.add(0, folder.name)
        cursor = folder.parentId
    }
    return parts.joinToString(" / ")
}

private data class VaultDrag(val videos: List<Long>, val folders: List<Long>)

private fun vaultDragData(drag: VaultDrag): DragAndDropTransferData {
    val text = "v:" + drag.videos.joinToString(",") + ";f:" + drag.folders.joinToString(",")
    return DragAndDropTransferData(
        ClipData.newPlainText("mvp-vault", text),
        drag,
        View.DRAG_FLAG_GLOBAL,
    )
}

private fun parseVaultDrag(text: String): VaultDrag? {
    if (!text.startsWith("v:") || !text.contains(";f:")) return null
    val parts = text.split(";f:", limit = 2)
    if (parts.size != 2) return null
    val videos = parts[0].removePrefix("v:").split(',').mapNotNull { it.toLongOrNull() }
    val folders = parts[1].split(',').mapNotNull { it.toLongOrNull() }
    if (videos.isEmpty() && folders.isEmpty()) return null
    return VaultDrag(videos, folders)
}

private fun activityOf(context: Context): Activity? {
    var current = context
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

private fun acceptsVaultDrop(event: DragAndDropEvent): Boolean {
    val drag = event.toAndroidDragEvent()
    if (drag.localState is VaultDrag) return true
    val clip = drag.clipData ?: return false
    for (index in 0 until clip.itemCount) {
        val item = clip.getItemAt(index)
        if (item.uri != null) return true
        val text = item.text?.toString() ?: continue
        if (parseVaultDrag(text) != null) return true
    }
    return false
}

@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.vaultDropTarget(
    onActive: (Boolean) -> Unit,
    onDrop: (DragAndDropEvent) -> Boolean,
): Modifier = this.dragAndDropTarget(
    shouldStartDragAndDrop = ::acceptsVaultDrop,
    target = object : DragAndDropTarget {
        override fun onEntered(event: DragAndDropEvent) { onActive(true) }
        override fun onExited(event: DragAndDropEvent) { onActive(false) }
        override fun onEnded(event: DragAndDropEvent) { onActive(false) }
        override fun onDrop(event: DragAndDropEvent): Boolean {
            onActive(false)
            return onDrop(event)
        }
    },
)

/**
 * Tap opens. A long press that lifts without moving is the item menu or
 * selection. A long press that then moves starts a drag, the same split a
 * file manager uses so the list can still scroll before the long press.
 */
@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.explorerItem(
    onClick: () -> Unit,
    onLongPress: () -> Unit,
    transfer: () -> DragAndDropTransferData,
): Modifier = this.dragAndDropSource(
    drawDragDecoration = {
        drawRoundRect(color = Color(0xCC3390EC), cornerRadius = CornerRadius(24f, 24f))
    },
    block = {
    val source = this
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        var timedOut = false
        val up = try {
            withTimeout(viewConfiguration.longPressTimeoutMillis) {
                waitForUpOrCancellation()
            }
        } catch (_: PointerEventTimeoutCancellationException) {
            timedOut = true
            null
        }
        if (!timedOut) {
            if (up != null) {
                up.consume()
                onClick()
            }
            return@awaitEachGesture
        }
        // A long press that lifts is the menu. The first move hands the pointer
        // to the system drag so a drop on a folder is delivered.
        val origin = down.position
        val slop = viewConfiguration.touchSlop
        while (true) {
            val event = awaitPointerEvent(PointerEventPass.Initial)
            val change = event.changes.firstOrNull { it.id == down.id } ?: return@awaitEachGesture
            if (!change.pressed) {
                onLongPress()
                return@awaitEachGesture
            }
            if ((change.position - origin).getDistance() >= slop) {
                source.startTransfer(transfer())
                return@awaitEachGesture
            }
        }
    }
    },
)

@Composable
private fun FolderBar(
    folders: List<FolderEntry>,
    currentFolderId: Long,
    onOpen: (Long) -> Unit,
    onDrop: (DragAndDropEvent, Long) -> Boolean,
) {
    val crumb = ArrayList<FolderEntry>()
    var cursor = currentFolderId
    var guard = 0
    while (cursor != 0L && guard++ < 64) {
        val folder = folders.firstOrNull { it.id == cursor } ?: break
        crumb.add(0, folder)
        cursor = folder.parentId
    }
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Crumb("Library", current = currentFolderId == 0L, onClick = { onOpen(0L) }, onDrop = { onDrop(it, 0L) })
        crumb.forEach { folder ->
            Text("/", color = Mvp.description, fontSize = 13.sp)
            Crumb(
                folder.name,
                current = folder.id == currentFolderId,
                onClick = { onOpen(folder.id) },
                onDrop = { onDrop(it, folder.id) },
            )
        }
    }
}

@Composable
private fun Crumb(
    text: String,
    current: Boolean,
    onClick: () -> Unit,
    onDrop: (DragAndDropEvent) -> Boolean,
) {
    var hot by remember { mutableStateOf(false) }
    Text(
        text,
        color = if (current || hot) Color.White else Mvp.accent,
        fontSize = 13.sp,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(if (hot) Mvp.accent.copy(alpha = 0.45f) else Color.Transparent)
            .vaultDropTarget(onActive = { hot = it }, onDrop = onDrop)
            .clickable(onClick = onClick)
            .padding(horizontal = 4.dp, vertical = 2.dp),
    )
}

@Composable
private fun FolderGlyph(modifier: Modifier = Modifier) {
    Icon(
        painterResource(R.drawable.files_folder),
        contentDescription = null,
        tint = Color(0xFF4FA6F7),
        modifier = modifier,
    )
}

private fun folderModifier(
    onClick: () -> Unit,
    onLongPress: (() -> Unit)?,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onHot: (Boolean) -> Unit,
): Modifier {
    val base = Modifier.vaultDropTarget(onActive = onHot, onDrop = onDrop)
    return if (transfer != null && onLongPress != null) {
        base.explorerItem(onClick = onClick, onLongPress = onLongPress, transfer = transfer)
    } else {
        base.clickable(onClick = onClick)
    }
}

@Composable
private fun FolderDetailRow(
    name: String,
    meta: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    var hot by remember { mutableStateOf(false) }
    val rowColor by animateColorAsState(
        if (hot) Mvp.accent.copy(alpha = 0.35f) else Mvp.inputBg,
        tween(160),
        label = "folderRow",
    )
    Row(
        modifier
            .fillMaxWidth()
            .background(rowColor)
            .then(folderModifier(onClick, onLongPress, transfer, onDrop) { hot = it })
            .padding(horizontal = 10.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FolderGlyph(Modifier.size(44.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(name, color = Mvp.text, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(meta, color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (onLongPress != null) {
            RowActions(onLongPress)
        }
    }
}

@Composable
private fun FolderListRow(
    name: String,
    meta: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    var hot by remember { mutableStateOf(false) }
    Row(
        modifier
            .fillMaxWidth()
            .background(if (hot) Mvp.accent.copy(alpha = 0.35f) else Mvp.inputBg)
            .then(folderModifier(onClick, onLongPress, transfer, onDrop) { hot = it })
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FolderGlyph(Modifier.size(36.dp))
        Column(Modifier.weight(1f).padding(start = 10.dp)) {
            Text(name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(meta, color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (onLongPress != null) {
            RowActions(onLongPress)
        }
    }
}

@Composable
private fun FolderTile(
    name: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
    place: String = "",
) {
    var hot by remember { mutableStateOf(false) }
    Box(
        modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .background(if (hot) Mvp.accent.copy(alpha = 0.35f) else Color(0xFF161618))
            .then(folderModifier(onClick, onLongPress, transfer, onDrop) { hot = it }),
    ) {
        FolderGlyph(Modifier.align(Alignment.Center).fillMaxSize().padding(18.dp))
        Column(
            Modifier
                .align(Alignment.BottomStart)
                .fillMaxWidth()
                .padding(start = 6.dp, end = 6.dp, bottom = 6.dp),
        ) {
            Text(
                name,
                color = Color.White,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (place.isNotEmpty()) {
                Text(
                    place,
                    color = Color(0xFFDCDCE0),
                    fontSize = 10.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (onLongPress != null) {
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .padding(2.dp)
                    .size(28.dp)
                    .clip(CircleShape)
                    .background(Color.Black.copy(alpha = 0.45f))
                    .clickable { onLongPress() },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    painterResource(R.drawable.ic_ab_other),
                    contentDescription = "Actions",
                    tint = Color.White,
                    modifier = Modifier.size(16.dp),
                )
            }
        }
        if (hot) {
            Box(Modifier.fillMaxSize().background(Mvp.accent.copy(alpha = 0.35f)))
        }
    }
}

private suspend fun importBatch(
    context: android.content.Context,
    uris: List<Uri>,
    folderId: Long,
    onProgress: (done: Long, total: Long, label: String) -> Unit,
): List<CoreBridge.Result> {
    // Sizes up front (when the provider reports them) so the aggregate bar is
    // proportional across the batch.
    val declared = uris.map { uri ->
        runCatching {
            context.contentResolver.openAssetFileDescriptor(uri, "r")?.use { it.length }
        }.getOrNull() ?: -1L
    }
    val knownTotal = declared.filter { it > 0L }.sum()
    var done = 0L
    val results = ArrayList<CoreBridge.Result>(uris.size)

    uris.forEachIndexed { index, uri ->
        val name = queryName(context.contentResolver, uri) ?: "import_$index.mp4"
        // Stage each file in its own directory but keep the *original* file
        // name: the core records the gallery name from the path it imports, so
        // prefixing the temp file would rename the user's video in the vault.
        val staging = File(context.cacheDir, "import_$index").apply { mkdirs() }
        val tmp = File(staging, name)
        val result = try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                tmp.outputStream().use { output ->
                    val block = ByteArray(1 shl 16)
                    while (true) {
                        val read = input.read(block)
                        if (read < 0) break
                        output.write(block, 0, read)
                        done += read.toLong()
                        onProgress(done, maxOf(knownTotal, done), name)
                    }
                }
            } ?: run {
                results.add(CoreBridge.Result(false, "Could not open $name", ""))
                return@forEachIndexed
            }
            CoreBridge.Result.parse(CoreBridge.nativeImportInto(tmp.absolutePath, folderId)).also { parsed ->
                if (parsed.ok && !parsed.duplicate && parsed.id > 0L) {
                    LibraryArrivals.addFile(parsed.id, name, tmp.length(), folderId)
                }
            }
        } catch (e: Exception) {
            CoreBridge.Result(false, "Import failed: $name", e.message ?: "")
        } finally {
            tmp.delete()
            staging.delete()
        }
        results.add(result)
    }
    return results
}

@Composable
private fun CloudBar() {
    if (!RemoteVault.cloud) return
    var people by remember { mutableStateOf("") }
    var speed by remember { mutableStateOf(RemoteVault.syncSpeed) }
    LaunchedEffect(Unit) {
        while (isActive) {
            val raw = withContext(Dispatchers.IO) { RemoteVault.getText("/v1/sessions") }
            people = CloudFiles.sessionLine(raw)
            delay(4000)
        }
    }
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
        if (people.isNotBlank()) {
            Text(people, color = Color.White.copy(alpha = 0.9f), fontSize = 12.sp, maxLines = 2)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            listOf("slow" to "Slow", "normal" to "Normal", "fast" to "Fast").forEach { (id, label) ->
                Text(
                    label,
                    color = if (speed == id) Mvp.accent else Color.White.copy(alpha = 0.55f),
                    fontSize = 13.sp,
                    modifier = Modifier.clickable {
                        speed = id
                        RemoteVault.syncSpeed = id
                    },
                )
            }
        }
    }
}

@Composable
private fun FileTile(video: VideoEntry, edge: Int, square: Boolean) {
    val ticks by CloudFiles.synced
    Box(if (square) Modifier.fillMaxSize() else Modifier) {
        FileFace(video, edge, square)
        if (video.id in ticks) {
            Box(
                Modifier
                    .align(Alignment.BottomEnd)
                    .padding(2.dp)
                    .size(14.dp)
                    .background(Color(0xFF107C10), CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Text("✓", color = Color.White, fontSize = 9.sp)
            }
        }
    }
}

@Composable
private fun FileFace(video: VideoEntry, edge: Int, square: Boolean) {
    val generation = GalleryCache.generation
    var kind by remember(video.id, generation) { mutableStateOf(FileKinds.peek(video.id)) }
    LaunchedEffect(video.id, video.name, generation) {
        if (kind == null) {
            val resolved = withContext(Dispatchers.IO) { FileKinds.resolve(video.id, video.name) }
            if (generation == GalleryCache.generation) kind = resolved
        }
    }
    val resolved = kind
    val shape = if (square) RoundedCornerShape(0.dp) else RoundedCornerShape(6.dp)
    val modifier = if (square) Modifier.fillMaxSize() else Modifier.size(edge.dp).clip(shape)
    if (resolved != null && resolved.route == 0) {
        KindPlate(resolved.label, FileKinds.compactSize(video.size), modifier.clip(shape))
        return
    }
    if (resolved?.route == 4) {
        TextBadge(resolved.label, modifier)
        return
    }
    val audio = resolved?.route == 2 || (resolved == null && isAudioName(video.name))
    val pdf = resolved?.route == 3 || (resolved == null && isPdfName(video.name))
    val image = resolved?.let { FileKinds.isImage(it) } ?: isImageName(video.name)
    if (square) {
        SquareThumb(video.id, audio = audio, image = image, pdf = pdf)
    } else {
        Thumb(video.id, edge, audio = audio, image = image, pdf = pdf)
    }
}

@Composable
private fun KindPlate(label: String, sizeText: String, modifier: Modifier) {
    val colors = listOf(
        0xFFC44B4B, 0xFF3E7CB8, 0xFF3F8F6B, 0xFFC4842A, 0xFF7A5AC4, 0xFF2F8A8A,
        0xFFB04E7A, 0xFF5C6BC4, 0xFF8A6A3B, 0xFF3D7A4A, 0xFFA14E3C, 0xFF4A6A8A,
    )
    val color = Color(colors[(label.hashCode() and Int.MAX_VALUE) % colors.size].toInt())
    Box(modifier.background(color), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text("[ $label ]", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            Text(sizeText, color = Color.White, fontSize = 11.sp)
        }
    }
}

@Composable
private fun Thumb(id: Long, size: Int, audio: Boolean = false, image: Boolean = false, pdf: Boolean = false) {
    var bmp by remember(id) { mutableStateOf(GalleryCache.thumb(id, size)) }
    LaunchedEffect(id) {
        if (bmp != null || pdf) return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            if (image && !RemoteVault.connected()) {
                decodeVaultImage(id, 320)
            } else {
                val jpeg = if (RemoteVault.connected()) {
                    RemoteVault.getBytes("/v1/thumbnail?id=$id&max=320")
                } else {
                    CoreBridge.nativeThumbnail(id, 320)
                }
                jpeg?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
            }
        }
        if (decoded != null) {
            GalleryCache.putThumb(id, size, decoded)
            bmp = decoded
        }
    }
    val image = bmp
    if (image != null) {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = null,
            modifier = Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)),
            contentScale = ContentScale.Crop,
        )
    } else if (audio) {
        MusicNote(Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)))
    } else if (pdf) {
        PdfBadge(Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)), large = false)
    } else {
        Box(
            Modifier.size(size.dp).clip(RoundedCornerShape(6.dp)).background(Mvp.card),
            contentAlignment = Alignment.Center,
        ) { CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp) }
    }
}

@Composable
private fun MusicNote(modifier: Modifier) {
    Image(
        painterResource(R.drawable.nocover),
        contentDescription = null,
        modifier = modifier,
        contentScale = ContentScale.Crop,
    )
}

@Composable
private fun TextBadge(label: String, modifier: Modifier) {
    val badge = label.uppercase().take(5)
    Box(modifier.background(Color(0xFF1C1C1E)), contentAlignment = Alignment.Center) {
        Box(Modifier.fillMaxSize(0.72f), contentAlignment = Alignment.Center) {
            Image(
                painterResource(R.drawable.media_doc_blue),
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Fit,
            )
            Text(
                badge,
                color = Color.White,
                fontSize = 8.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
    }
}

@Composable
private fun PdfBadge(modifier: Modifier, large: Boolean) {
    Box(modifier.background(Color(0xFF1C1C1E)), contentAlignment = Alignment.Center) {
        Box(
            Modifier.fillMaxSize(if (large) 0.56f else 0.82f),
            contentAlignment = Alignment.Center,
        ) {
            Image(
                painterResource(R.drawable.media_doc_blue),
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Fit,
            )
            Text(
                "PDF",
                color = Color.White,
                fontSize = if (large) 13.sp else 7.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(top = if (large) 8.dp else 3.dp),
            )
        }
    }
}

/** Square crop that fills an Instagram-style grid cell. */
@Composable
private fun SquareThumb(id: Long, audio: Boolean = false, image: Boolean = false, pdf: Boolean = false) {
    var bmp by remember(id) { mutableStateOf(GalleryCache.thumb(id, 480)) }
    LaunchedEffect(id) {
        if (bmp != null || pdf) return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            if (image && !RemoteVault.connected()) {
                decodeVaultImage(id, 480)
            } else {
                val jpeg = if (RemoteVault.connected()) {
                    RemoteVault.getBytes("/v1/thumbnail?id=$id&max=480")
                } else {
                    CoreBridge.nativeThumbnail(id, 480)
                }
                jpeg?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
            }
        }
        if (decoded != null) {
            GalleryCache.putThumb(id, 480, decoded)
            bmp = decoded
        }
    }
    val image = bmp
    if (image != null) {
        Image(
            bitmap = image.asImageBitmap(),
            contentDescription = null,
            modifier = Modifier.fillMaxSize(),
            contentScale = ContentScale.Crop,
        )
    } else if (audio) {
        MusicNote(Modifier.fillMaxSize())
    } else if (pdf) {
        PdfBadge(Modifier.fillMaxSize(), large = true)
    } else {
        Box(Modifier.fillMaxSize().background(Mvp.card), contentAlignment = Alignment.Center) {
            CircularProgressIndicator(Modifier.size(18.dp), color = Mvp.accent, strokeWidth = 2.dp)
        }
    }
}

/** The ⋮ affordance shared by every view: opens the per-item action sheet. */
@Composable
private fun RowActions(onClick: () -> Unit) {
    Box(
        Modifier
            .width(40.dp)
            .clickable(onClick = onClick)
            .padding(vertical = 6.dp),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            painterResource(R.drawable.ic_ab_other),
            contentDescription = "Actions",
            tint = Mvp.headerText,
            modifier = Modifier.size(20.dp),
        )
    }
}

/** Joins the fields that actually have a value. A loading placeholder is left out
 *  so a narrow phone never wraps a column into a vertical stack of digits. */
private fun detailLine(vararg parts: String?): String =
    parts.mapNotNull { it?.trim()?.takeIf { part -> part.isNotEmpty() && part != "…" && part != "—" } }
        .joinToString("  ·  ")

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun DetailSortBar() {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Mvp.headerBg)
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        SortKey.entries.forEach { key ->
            val active = GalleryCache.sortKey == key
            val mark = if (!active) "" else if (GalleryCache.sortAscending) " ↑" else " ↓"
            val fill by animateColorAsState(
                if (active) Mvp.accent.copy(alpha = 0.18f) else Color.Transparent,
                tween(180),
                label = "sortChip",
            )
            Text(
                key.label + mark,
                color = if (active) Mvp.accent else Mvp.headerText,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                modifier = Modifier
                    .clip(RoundedCornerShape(14.dp))
                    .background(fill)
                    .clickable { applySort(key) }
                    .padding(horizontal = 10.dp, vertical = 6.dp),
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun DetailsView(
    videos: List<VideoEntry>,
    folders: List<FolderEntry>,
    upParentId: Long?,
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
    onAction: (VideoEntry) -> Unit,
    onOpenFolder: (Long) -> Unit,
    onFolderMenu: (FolderEntry) -> Unit,
    padding: PaddingValues,
    onDrop: (DragAndDropEvent) -> Boolean,
    onDropOn: (DragAndDropEvent, Long) -> Boolean,
    dragOf: (VideoEntry) -> DragAndDropTransferData,
    videoPlace: Map<Long, String> = emptyMap(),
    folderPlace: Map<Long, String> = emptyMap(),
) {
    LazyColumn(
        state = GalleryCache.detailsScroll,
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Color.Black).vaultDropTarget(onActive = {}, onDrop = onDrop),
    ) {
        stickyHeader { DetailSortBar() }
        if (upParentId != null) {
            item(key = "up") {
                FolderDetailRow("..", "Up", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) })
            }
        }
        items(folders, key = { "f${it.id}" }) { folder ->
            val where = folderPlace[folder.id].orEmpty()
            FolderDetailRow(
                folder.name,
                if (where.isEmpty()) "Folder" else where,
                { onOpenFolder(folder.id) },
                { vaultDragData(VaultDrag(emptyList(), listOf(folder.id))) },
                { onDropOn(it, folder.id) },
                { onFolderMenu(folder) },
                Modifier.animateItem(),
            )
        }
        itemsIndexed(videos, key = { _, v -> "${GalleryCache.generation}:${v.id}" }) { index, v ->
            val media = rememberMediaInfo(v.id)
            val picked = v.id in selected
            val stripe = if (index % 2 == 0) Mvp.inputBg else Mvp.alternateRow
            val rowColor by animateColorAsState(
                if (picked) Mvp.selection else stripe,
                tween(180),
                label = "detailRow",
            )
            val facts = detailLine(v.sizeText, media?.durationText, media?.resolution)
            val extra = detailLine(media?.codecText, v.tags.joinToString(", "), v.importedText)
            val motion = if (
                GalleryCache.sortKey == SortKey.Duration ||
                GalleryCache.sortKey == SortKey.Resolution ||
                GalleryCache.sortKey == SortKey.Codec
            ) Modifier else Modifier.animateItem()
            Row(
                motion
                    .fillMaxWidth()
                    .background(rowColor)
                    .vaultDropTarget(onActive = {}, onDrop = onDrop)
                    .explorerItem(
                        onClick = { onOpen(v) },
                        onLongPress = { onLongPress(v) },
                        transfer = { dragOf(v) },
                    )
                    .padding(horizontal = 10.dp, vertical = 10.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FileTile(v, 52, square = false)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(
                        v.name,
                        color = Mvp.text,
                        fontSize = 15.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    videoPlace[v.id]?.let { place ->
                        Text(
                            place,
                            color = Mvp.description,
                            fontSize = 12.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    if (facts.isNotEmpty()) {
                        Text(
                            facts,
                            color = Mvp.description,
                            fontSize = 12.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    if (extra.isNotEmpty()) {
                        Text(
                            extra,
                            color = Mvp.description,
                            fontSize = 12.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                RowActions { onAction(v) }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun IconGridView(
    videos: List<VideoEntry>,
    folders: List<FolderEntry>,
    upParentId: Long?,
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
    onAction: (VideoEntry) -> Unit,
    onOpenFolder: (Long) -> Unit,
    onFolderMenu: (FolderEntry) -> Unit,
    padding: PaddingValues,
    onDrop: (DragAndDropEvent) -> Boolean,
    onDropOn: (DragAndDropEvent, Long) -> Boolean,
    dragOf: (VideoEntry) -> DragAndDropTransferData,
    videoPlace: Map<Long, String> = emptyMap(),
    folderPlace: Map<Long, String> = emptyMap(),
) {
    LazyVerticalGrid(
        columns = GridCells.Fixed(3),
        state = GalleryCache.gridScroll,
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Color.Black).vaultDropTarget(onActive = {}, onDrop = onDrop),
        horizontalArrangement = Arrangement.spacedBy(1.dp),
        verticalArrangement = Arrangement.spacedBy(1.dp),
    ) {
        item(span = { GridItemSpan(3) }) { ListSortBar() }
        if (upParentId != null) {
            item(key = "up") {
                FolderTile("..", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) })
            }
        }
        items(folders, key = { "f${it.id}" }) { folder ->
            FolderTile(
                folder.name,
                { onOpenFolder(folder.id) },
                { vaultDragData(VaultDrag(emptyList(), listOf(folder.id))) },
                { onDropOn(it, folder.id) },
                { onFolderMenu(folder) },
                Modifier.animateItem(),
                folderPlace[folder.id].orEmpty(),
            )
        }
        items(videos, key = { "${GalleryCache.generation}:${it.id}" }) { v ->
            val media = rememberMediaInfo(v.id)
            val motion = if (
                GalleryCache.sortKey == SortKey.Duration ||
                GalleryCache.sortKey == SortKey.Resolution ||
                GalleryCache.sortKey == SortKey.Codec
            ) Modifier else Modifier.animateItem()
            Box(
                motion
                    .fillMaxWidth()
                    .aspectRatio(1f)
                    .vaultDropTarget(onActive = {}, onDrop = onDrop)
                    .explorerItem(
                        onClick = { onOpen(v) },
                        onLongPress = { onLongPress(v) },
                        transfer = { dragOf(v) },
                    ),
            ) {
                FileTile(v, 0, square = true)
                videoPlace[v.id]?.let { place ->
                    Text(
                        place,
                        color = Color.White,
                        fontSize = 10.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .fillMaxWidth()
                            .background(Color.Black.copy(alpha = 0.55f))
                            .padding(horizontal = 6.dp, vertical = 3.dp),
                    )
                }
                if (v.id !in selected) {
                    Box(
                        Modifier
                            .align(Alignment.TopEnd)
                            .padding(4.dp)
                            .size(28.dp)
                            .clip(CircleShape)
                            .background(Color.Black.copy(alpha = 0.45f))
                            .clickable { onAction(v) },
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(
                            painterResource(R.drawable.ic_ab_other),
                            contentDescription = "Actions",
                            tint = Color.White,
                            modifier = Modifier.size(16.dp),
                        )
                    }
                }
                val known = FileKinds.peek(v.id)
                if (known?.route == 3 || (known == null && isPdfName(v.name))) {
                    Box(
                        Modifier
                            .align(Alignment.BottomCenter)
                            .fillMaxWidth()
                            .height(36.dp)
                            .background(
                                Brush.verticalGradient(
                                    listOf(Color.Transparent, Color.Black.copy(alpha = 0.78f)),
                                ),
                            ),
                    )
                    Text(
                        v.name.substringBeforeLast('.').ifBlank { v.name },
                        color = Color.White,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier
                            .align(Alignment.BottomStart)
                            .fillMaxWidth()
                            .padding(start = 6.dp, end = 6.dp, bottom = 6.dp),
                    )
                }
                if (known?.route == 2 || (known == null && isAudioName(v.name))) {
                    val parsed = songLabel(v.name)
                    val artist = media?.artist?.takeIf { it.isNotBlank() } ?: parsed.first
                    val title = media?.title?.takeIf { it.isNotBlank() } ?: parsed.second
                    Box(
                        Modifier
                            .align(Alignment.BottomCenter)
                            .fillMaxWidth()
                            .height(48.dp)
                            .background(
                                Brush.verticalGradient(
                                    listOf(Color.Transparent, Color.Black.copy(alpha = 0.78f)),
                                ),
                            ),
                    )
                    Column(
                        Modifier
                            .align(Alignment.BottomStart)
                            .fillMaxWidth()
                            .padding(start = 6.dp, end = 6.dp, bottom = 5.dp),
                    ) {
                        Text(
                            title,
                            color = Color.White,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        if (artist.isNotBlank()) {
                            Text(
                                artist,
                                color = Color(0xFFDCDCE0),
                                fontSize = 11.sp,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
                val duration = media?.durationText
                if (!isAudioName(v.name) && duration != null && duration != "…" && media.durationMs > 0L) {
                    Box(
                        Modifier
                            .align(Alignment.BottomCenter)
                            .fillMaxWidth()
                            .height(36.dp)
                            .background(
                                Brush.verticalGradient(
                                    listOf(Color.Transparent, Color.Black.copy(alpha = 0.72f)),
                                ),
                            ),
                    )
                    Text(
                        duration,
                        color = Color.White,
                        fontSize = 11.sp,
                        fontWeight = FontWeight.SemiBold,
                        style = TextStyle(
                            shadow = Shadow(Color.Black, Offset(0f, 1f), 3f),
                        ),
                        modifier = Modifier
                            .align(Alignment.BottomStart)
                            .padding(start = 6.dp, bottom = 5.dp),
                    )
                }
                if (v.id in selected) {
                    Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.45f)))
                    Box(
                        Modifier
                            .align(Alignment.TopEnd)
                            .padding(6.dp)
                            .size(22.dp)
                            .clip(CircleShape)
                            .background(Mvp.primary),
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(
                            painterResource(R.drawable.checkbig),
                            contentDescription = null,
                            tint = Color.White,
                            modifier = Modifier.size(14.dp),
                        )
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ListView(
    videos: List<VideoEntry>,
    folders: List<FolderEntry>,
    upParentId: Long?,
    selected: Set<Long>,
    onOpen: (VideoEntry) -> Unit,
    onLongPress: (VideoEntry) -> Unit,
    onAction: (VideoEntry) -> Unit,
    onOpenFolder: (Long) -> Unit,
    onFolderMenu: (FolderEntry) -> Unit,
    padding: PaddingValues,
    onDrop: (DragAndDropEvent) -> Boolean,
    onDropOn: (DragAndDropEvent, Long) -> Boolean,
    dragOf: (VideoEntry) -> DragAndDropTransferData,
    videoPlace: Map<Long, String> = emptyMap(),
    folderPlace: Map<Long, String> = emptyMap(),
) {
    LazyColumn(
        state = GalleryCache.listScroll,
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Color.Black).vaultDropTarget(onActive = {}, onDrop = onDrop),
    ) {
        item { ListSortBar() }
        if (upParentId != null) {
            item(key = "up") {
                FolderListRow("..", "Up", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) })
            }
        }
        items(folders, key = { "f${it.id}" }) { folder ->
            val where = folderPlace[folder.id].orEmpty()
            FolderListRow(
                folder.name,
                if (where.isEmpty()) "Folder" else where,
                { onOpenFolder(folder.id) },
                { vaultDragData(VaultDrag(emptyList(), listOf(folder.id))) },
                { onDropOn(it, folder.id) },
                { onFolderMenu(folder) },
                Modifier.animateItem(),
            )
        }
        items(videos, key = { "${GalleryCache.generation}:${it.id}" }) { v ->
            val picked = v.id in selected
            val stripe = if (videos.indexOf(v) % 2 == 0) Mvp.inputBg else Mvp.alternateRow
            val rowColor by animateColorAsState(
                if (picked) Mvp.selection else stripe,
                tween(180),
                label = "listRow",
            )
            val motion = if (
                GalleryCache.sortKey == SortKey.Duration ||
                GalleryCache.sortKey == SortKey.Resolution ||
                GalleryCache.sortKey == SortKey.Codec
            ) Modifier else Modifier.animateItem()
            Row(
                motion
                    .fillMaxWidth()
                    .background(rowColor)
                    .vaultDropTarget(onActive = {}, onDrop = onDrop)
                    .explorerItem(
                        onClick = { onOpen(v) },
                        onLongPress = { onLongPress(v) },
                        transfer = { dragOf(v) },
                    )
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FileTile(v, 36, square = false)
                Column(Modifier.weight(1f).padding(start = 10.dp)) {
                    Text(v.name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    videoPlace[v.id]?.let { place ->
                        Text(place, color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    Text("${v.sizeText} · ${v.tags.joinToString(", ")}", color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                RowActions { onAction(v) }
            }
        }
    }
}

@Composable
private fun ListSortBar() {
    var open by remember { mutableStateOf(false) }
    val key = GalleryCache.sortKey
    val arrow = if (GalleryCache.sortAscending) "↑" else "↓"
    Box(Modifier.fillMaxWidth()) {
        Row(
            Modifier
                .fillMaxWidth()
                .clickable { open = true }
                .padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Icon(
                painterResource(R.drawable.menu_sort_date),
                contentDescription = null,
                tint = Mvp.description,
                modifier = Modifier.size(16.dp),
            )
            Text("Sort", color = Mvp.description, fontSize = 12.sp)
            Text("$arrow  ${key.label}", color = Mvp.accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            SortKey.entries.forEach { item ->
                val mark = if (item != key) "" else if (GalleryCache.sortAscending) "  ↑" else "  ↓"
                DropdownMenuItem(
                    text = {
                        Text(
                            item.label + mark,
                            color = if (item == key) Mvp.accent else Mvp.text,
                            fontSize = 14.sp,
                        )
                    },
                    onClick = {
                        open = false
                        applySort(item)
                    },
                )
            }
        }
    }
}
