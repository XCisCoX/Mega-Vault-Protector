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
import androidx.compose.runtime.mutableStateMapOf
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
import androidx.compose.ui.text.style.TextAlign
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
    var filterTagIds: Set<Long> = emptySet()
    var search = ""
    var sortKey by mutableStateOf(SortKey.Name)
    var sortAscending by mutableStateOf(true)
    var mediaVersion by mutableStateOf(0)
    var generation by mutableStateOf(0)
    val media = HashMap<Long, MediaInfo>()
    // Ids whose container probe failed. Without this, a duration sort scrolls
    // the row away, the lazy list recreates it, and the probe starts again.
    val probeMiss = HashSet<Long>()
    // Scroll position per folder *and* view mode. One state shared by every
    // folder left the parent list wherever the child had been scrolled to, so
    // going up never came back to the rows you left from.
    private val listScrolls = HashMap<String, androidx.compose.foundation.lazy.LazyListState>()
    private val gridScrolls = HashMap<String, androidx.compose.foundation.lazy.grid.LazyGridState>()
    private val thumbs = HashMap<String, android.graphics.Bitmap>()

    /** The scroll state of one folder in one view; created on first use. */
    fun listScroll(scope: String): androidx.compose.foundation.lazy.LazyListState =
        listScrolls.getOrPut(scope) { androidx.compose.foundation.lazy.LazyListState() }

    fun gridScroll(scope: String): androidx.compose.foundation.lazy.grid.LazyGridState =
        gridScrolls.getOrPut(scope) { androidx.compose.foundation.lazy.grid.LazyGridState() }

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
        listScrolls.clear()
        gridScrolls.clear()
        media.clear()
        probeMiss.clear()
        mediaVersion++
    }
}

/**
 * Sort choices for the folder on screen. [firstAscending] is the direction the
 * first tap means: "Newest" has to start at the newest file, not the oldest.
 * [Newest] uses the import time — the vault keeps no modification time.
 */
enum class SortKey(val label: String, val firstAscending: Boolean = true) {
    Name("Name"),
    Newest("Newest", firstAscending = false),
    Size("Size"),
    Duration("Duration"),
    Resolution("Resolution"),
    Codec("Codec"),
    Tags("Tags"),
}

private fun applySort(key: SortKey) {
    if (GalleryCache.sortKey == key) {
        GalleryCache.sortAscending = !GalleryCache.sortAscending
    } else {
        GalleryCache.sortKey = key
        GalleryCache.sortAscending = key.firstAscending
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
    onOpenSync: () -> Unit = {},
    onOpenPlugins: () -> Unit = {},
    onPlayPlugin: (PluginSession) -> Unit = {},
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
    var filterTagIds by remember { mutableStateOf(GalleryCache.filterTagIds) }
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
    val pluginManager = remember(vaultLocation) { PluginManager(context.applicationContext, vaultLocation) }
    var providerSearch by remember(vaultLocation) { mutableStateOf<List<PluginManifest>>(emptyList()) }
    LaunchedEffect(vaultLocation, showSettings) {
        if (!showSettings) {
            providerSearch = if (RemoteVault.connected()) emptyList() else withContext(Dispatchers.IO) {
                pluginManager.manifests().filter {
                    PluginProviderRegistry.protocol(it.protocol).music && pluginManager.options(it).enabled
                }
            }
            if (PluginLibrary.session?.manifest?.id?.let { id -> providerSearch.none { it.id == id } } == true) {
                PluginLibrary.close()
            }
        }
    }
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

    // A background sync changed what this vault holds: re-read the index instead
    // of leaving the grid showing yesterday's rows until the next visit.
    LaunchedEffect(SyncV4Auto.remoteRevision) {
        if (SyncV4Auto.remoteRevision > 0) refresh()
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
                SyncV4Auto.markDirty()
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
    var stagedUpdateApk by remember { mutableStateOf<File?>(null) }
    var updateCardHidden by remember { mutableStateOf(false) }
    var packagePlan by remember { mutableStateOf<PackagePlan?>(null) }
    var armedPackage by remember { mutableStateOf<PackagePlan?>(null) }
    var packagePassword by remember { mutableStateOf("") }
    var packageCompress by remember { mutableStateOf(true) }
    var packageImportUri by remember { mutableStateOf<Uri?>(null) }
    LaunchedEffect(Unit) {
        val release = withContext(Dispatchers.IO) {
            runCatching { checkForUpdate(context) }.getOrNull()
        }
        pendingRelease = release
        if (release == null) return@LaunchedEffect
        val cached = File(context.cacheDir, "update-${release.version}.apk")
        if (cached.isFile && cached.length() > 0L) stagedUpdateApk = cached
        // "Download automatically" prepares the package quietly; installing
        // still needs the phone's own installer confirmation, exactly like a
        // manual update tap.
        if (stagedUpdateApk == null && ThemePrefs.downloadAutomatically(context)) {
            withContext(Dispatchers.IO) {
                runCatching {
                    val apk = File(context.cacheDir, "update-${release.version}.apk")
                    if (!apk.isFile || apk.length() <= 0L) {
                        downloadRelease(release, apk) { }
                    }
                    stagedUpdateApk = apk
                }
            }
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
            if (failure != null) {
                error = failure
            } else {
                SyncV4Auto.markDirty()
                refresh()
            }
        }
    }
    /**
     * Downloads only. Installing is a separate, deliberate tap: the system
     * installer must not appear out of a progress bar that never ends.
     */
    fun downloadAppUpdate() {
        val release = pendingRelease ?: return
        if (updateProgress != null) return
        updateCardHidden = false
        updateProgress = 0f
        scope.launch {
            val failure = withContext(Dispatchers.IO) {
                try {
                    val apk = File(context.cacheDir, "update-${release.version}.apk")
                    downloadRelease(release, apk) { progress -> updateProgress = progress }
                    stagedUpdateApk = apk
                    null
                } catch (e: Exception) {
                    e.message ?: "Download failed"
                }
            }
            // The bar is about bytes; when they are all here it goes away and
            // the Install action takes its place.
            updateProgress = null
            if (failure != null) error = failure
        }
    }

    fun installStagedUpdate() {
        val apk = stagedUpdateApk ?: return
        runCatching { installRelease(context, apk) }
            .onFailure { error = it.message ?: "The installer could not start" }
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
        backgroundColor = Mvp.barBg,
        tints = listOf(HazeTint(Mvp.barTint)),
        blurRadius = 32.dp,
        noiseFactor = 0.03f,
    )

    var folderForward by remember { mutableStateOf(true) }
    // Selection per folder, so stepping into a subfolder and pressing Back puts
    // you back in front of the rows you had picked, not a blank toolbar.
    val selectionByFolder = remember { mutableStateMapOf<Long, Set<Long>>() }
    fun openFolder(id: Long) {
        if (id == PEER_FOLDER_ID) {
            selectedIds = emptySet()
            if (PeerLink.linked()) onOpenPeer() else error = "The PC is no longer connected."
            return
        }
        selectionByFolder[currentFolderId] = selectedIds
        val leaving = folders.firstOrNull { it.id == currentFolderId }
        folderForward = when {
            leaving?.parentId == id -> false
            id == 0L && currentFolderId != 0L -> false
            else -> true
        }
        currentFolderId = id
        GalleryCache.currentFolderId = id
        // Only rows that still live in the folder being opened can come back
        // selected; anything moved or removed in the meantime drops out.
        selectedIds = selectionByFolder[id].orEmpty()
            .filterTo(HashSet()) { videoId -> videos.any { it.id == videoId && it.folderId == id } }
    }

    // Every change of the selection is remembered for the folder it was made
    // in, so the map never holds a stale set for the folder on screen.
    LaunchedEffect(selectedIds, currentFolderId) {
        selectionByFolder[currentFolderId] = selectedIds
    }

    // Sorting reshuffles the rows under the tap targets. If a selection is
    // live, it is dropped first — the list never reorders silently around a
    // selection the user cannot see move.
    fun chooseSort(key: SortKey) {
        if (selectedIds.isNotEmpty()) selectedIds = emptySet()
        applySort(key)
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
        if (PluginLibrary.folderOpen) {
            PluginLibrary.back()
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

    /**
     * The one move path, used by the picker and by drag-and-drop. Files are
     * content-addressed and stored once, so this is a database move — it never
     * copies and can never duplicate bytes. Nothing is deleted: an item the core
     * refuses stays where it was, stays selected, and is named in the message.
     */
    fun moveItems(videoIds: List<Long>, folderIds: List<Long>, destination: Long) {
        if (RemoteVault.connected()) {
            error = "This vault is open on your PC. Move it on the PC."
            return
        }
        if (destination == PEER_FOLDER_ID) {
            error = "The PC's library is read-only. Open it to copy from it."
            return
        }
        val drag = VaultDrag(videoIds.distinct(), folderIds.distinct())
        // A folder can never be moved into itself or into one of its own
        // subfolders. The picker greys those out; a drag is refused here.
        val blocked = drag.folders.filter { id ->
            id > 0L && (id == destination || folderContains(folders, id, destination))
        }.toSet()
        val movingFolders = drag.folders.filter { id ->
            id > 0L && id !in blocked &&
                (folders.firstOrNull { it.id == id }?.parentId ?: 0L) != destination
        }
        val movingVideos = drag.videos.filter { id ->
            videos.firstOrNull { it.id == id }?.folderId != destination
        }
        if (movingFolders.isEmpty() && movingVideos.isEmpty()) {
            // Nothing was written, so nothing is lost: explain the refusal and
            // leave the selection exactly as it was.
            error = dropRefusal(drag, destination) ?: "Nothing to move."
            return
        }
        scope.launch {
            val failedVideos = ArrayList<Long>()
            val failedFolders = ArrayList<Long>()
            var failure = ""
            SyncV4Auto.markDirty()
            withContext(Dispatchers.Default) {
                for (folderId in movingFolders) {
                    val result = runCatching {
                        CoreBridge.Result.parse(CoreBridge.nativeMoveFolder(folderId, destination))
                    }.getOrElse { CoreBridge.Result(false, "Move failed", it.message ?: "") }
                    if (!result.ok) {
                        failedFolders.add(folderId)
                        if (failure.isEmpty()) failure = result.detail.ifBlank { result.error }
                    }
                }
                for (videoId in movingVideos) {
                    val result = runCatching {
                        CoreBridge.Result.parse(CoreBridge.nativeMoveVideo(videoId, destination))
                    }.getOrElse { CoreBridge.Result(false, "Move failed", it.message ?: "") }
                    if (!result.ok) {
                        failedVideos.add(videoId)
                        if (failure.isEmpty()) failure = result.detail.ifBlank { result.error }
                    }
                }
            }
            // Everything that moved leaves the selection; whatever the core
            // refused stays selected so the next action still points at it.
            selectedIds = failedVideos.toSet()
            if (failedVideos.isNotEmpty() || failedFolders.isNotEmpty()) {
                val names = failedVideos.map { id -> videos.firstOrNull { it.id == id }?.name ?: "#$id" } +
                    failedFolders.map { id -> folders.firstOrNull { it.id == id }?.name ?: "#$id" }
                val listed = if (names.size <= 3) {
                    names.joinToString(", ")
                } else {
                    names.take(3).joinToString(", ") + " and ${names.size - 3} more"
                }
                error = "Could not move $listed" + if (failure.isNotBlank()) ": $failure" else ""
            }
            refresh()
        }
    }

    fun moveDragged(drag: VaultDrag, destination: Long) {
        moveItems(drag.videos, drag.folders, destination)
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
    // The reason a row under the drag cannot take it, so the row can paint the
    // refusal while the finger is still down.
    val refusalOn: (DragAndDropEvent, Long) -> String? = { event, destination ->
        dragPayload(event)?.let { dropRefusal(it, destination) }
    }

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
            .background(Mvp.window),
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
                onDismiss = {
                    showSettings = false
                    // Settings can run a forced check ("Check now"); adopt its
                    // answer so the update action appears without a restart.
                    pendingRelease = cachedRelease(context)
                    if (pendingRelease != null) {
                        val cached = File(context.cacheDir, "update-${pendingRelease?.version}.apk")
                        if (cached.isFile && cached.length() > 0L) stagedUpdateApk = cached
                    }
                },
                onChanged = { refresh() },
            )
        }

        val searching = search.isNotBlank()
        val scope = if (searching) folderSubtree(folders, currentFolderId) else emptySet()
        // Any selected tag is enough: the filter narrows, it never demands all.
        val filterNames = tags.filter { it.id in filterTagIds }
            .map { it.name.lowercase() }
        val filtered = videos.filter { v ->
            val here = if (searching) v.folderId in scope else v.folderId == currentFolderId
            here &&
                (filterNames.isEmpty() || v.tags.any { it.lowercase() in filterNames }) &&
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
            // A probe landing must not reshuffle rows under a live selection:
            // the tick and the tap target have to stay on the same row.
            if (selectedIds.isNotEmpty()) return@LaunchedEffect
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
                    SortKey.Newest -> directed(a.importedAt.compareTo(b.importedAt))
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
                // This file's local copy goes first, so the tick is there when
                // the viewer comes back, without the rest of the sync competing.
                CloudFiles.request(v.id)
                onPlay(v.id, v.name, shown)
            }
        }
        val pressItem: (VideoEntry) -> Unit = { v ->
            selectedIds = if (v.id in selectedIds) selectedIds - v.id else selectedIds + v.id
        }
        val discoverProviders = if (search.isNotBlank()) providerSearch else emptyList()
        val searchProvider: (PluginManifest) -> Unit = { manifest ->
            selectedIds = emptySet()
            PluginLibrary.open(context, manifest, vaultLocation, pluginManager.options(manifest), search)
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
            if (PluginLibrary.folderOpen) {
                PluginLibraryView(viewMode, contentPad, onPlayPlugin)
            } else when (viewMode) {
                0 -> DetailsView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                    videoPlace, folderPlace,
                    currentFolderId.toString(), { chooseSort(it) }, refusalOn,
                    discoverProviders, search, searchProvider,
                )
                1 -> IconGridView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                    videoPlace, folderPlace,
                    currentFolderId.toString(), { chooseSort(it) }, refusalOn,
                    discoverProviders, search, searchProvider,
                )
                else -> ListView(
                    shown, orderedFolders, upParentId, selectedIds,
                    openItem, pressItem, openActions, { openFolder(it) }, { folderMenu = it },
                    contentPad, dropHere, { event, id -> handleDrop(event, id) }, dragVideo,
                    videoPlace, folderPlace,
                    currentFolderId.toString(), { chooseSort(it) }, refusalOn,
                    discoverProviders, search, searchProvider,
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
                filterTagIds = filterTagIds,
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
                onToggleTag = { id ->
                    filterTagIds = if (id in filterTagIds) filterTagIds - id else filterTagIds + id
                    GalleryCache.filterTagIds = filterTagIds
                },
                onClearTags = {
                    filterTagIds = emptySet()
                    GalleryCache.filterTagIds = emptySet()
                },
                onSearch = { text ->
                    if (PluginLibrary.folderOpen) PluginLibrary.leave()
                    search = text
                    GalleryCache.search = text
                },
                onMusic = { showMusic = true },
                onUpdate = { downloadAppUpdate() },
                updateReady = pendingRelease != null &&
                    stagedUpdateApk == null && updateProgress == null,
                updateStaged = stagedUpdateApk != null && updateProgress == null,
                onInstallUpdate = { installStagedUpdate() },
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
                        moveTarget = MoveRequest(videos = selectedIds.toList())
                    }
                },
                onRemove = { confirmRemove = true },
                onRestore = { restoreLauncher.launch(FolderPicker.storageRootUri()) },
                onTag = { showTags = true },
                onClearSelection = { selectedIds = emptySet() },
            )
            CloudBar()
            TelegramMusicBar(onOpen = onOpenMusic)
            if (PluginLibrary.folderOpen) PluginLibraryBar() else FolderBar(
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
            // A path here would tell anyone reading the screen where the
            // encrypted vault lives. The footer names the location instead;
            // the real path stays on the Settings page, by choice.
            val place = remember(vaultLocation) {
                if (vaultLocation.startsWith("/")) {
                    VaultLocation.shortName(context, vaultLocation).substringBefore(" (")
                } else {
                    vaultLocation
                }
            }
            Text(place, color = Mvp.barText.copy(alpha = 0.86f), fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            val itemCount = if (PluginLibrary.folderOpen) PluginLibrary.target?.visibleRows(PluginLibrary.session?.rows.orEmpty())?.size ?: 0
                else childFolders.size + filtered.size + discoverProviders.size
            SyncStatusPill(onOpen = onOpenSync)
            Text(
                "$itemCount items",
                color = Mvp.barText.copy(alpha = 0.86f),
                fontSize = 12.sp,
                modifier = Modifier.padding(start = 10.dp),
            )
        }
        }

        if (updateProgress != null) {
            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(16.dp)
                    .background(Mvp.card, RoundedCornerShape(14.dp))
                    .border(1.dp, Mvp.cardBorder, RoundedCornerShape(14.dp))
                    .padding(14.dp),
            ) {
                Text("Downloading update…", color = Mvp.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                LinearProgressIndicator(
                    progress = { (updateProgress ?: 0f).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                    color = Mvp.accent,
                    trackColor = Mvp.inputBg,
                )
            }
        } else if (stagedUpdateApk != null && !updateCardHidden) {
            Row(
                Modifier
                    .align(Alignment.BottomCenter)
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(16.dp)
                    .background(Mvp.card, RoundedCornerShape(14.dp))
                    .border(1.dp, Mvp.cardBorder, RoundedCornerShape(14.dp))
                    .padding(14.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text("Update ready", color = Mvp.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                    Text(
                        pendingRelease?.version?.let { "Version $it is downloaded." } ?: "The update is downloaded.",
                        color = Mvp.description,
                        fontSize = 12.sp,
                    )
                }
                Text(
                    "Later",
                    color = Mvp.description,
                    fontSize = 14.sp,
                    modifier = Modifier
                        .clip(RoundedCornerShape(16.dp))
                        .clickable { updateCardHidden = true }
                        .padding(horizontal = 10.dp, vertical = 8.dp),
                )
                Text(
                    "Install",
                    color = Mvp.primary,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .clip(RoundedCornerShape(16.dp))
                        .background(Mvp.primary.copy(alpha = 0.16f))
                        .clickable { installStagedUpdate() }
                        .padding(horizontal = 14.dp, vertical = 8.dp),
                )
            }
        }
        // An empty vault used to be a blank field with nothing to read: the one
        // moment someone needs a sentence, and there was none.
        val nothingHere = !PluginLibrary.folderOpen &&
            childFolders.isEmpty() &&
            filtered.isEmpty() &&
            discoverProviders.isEmpty() &&
            search.isBlank()
        if (nothingHere) {
            Column(
                Modifier
                    .align(Alignment.Center)
                    .padding(horizontal = 40.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    "Nothing here yet",
                    color = Mvp.title,
                    fontSize = 20.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    "Photos, videos, music and documents you add are locked with your password and stay on this phone.",
                    color = Mvp.description,
                    fontSize = 14.sp,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.padding(top = 10.dp),
                )
                Text(
                    "Add your first file",
                    color = Color.White,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .padding(top = 22.dp)
                        .clip(RoundedCornerShape(22.dp))
                        .background(Mvp.primary)
                        .clickable { showAdd = true }
                        .padding(horizontal = 22.dp, vertical = 12.dp),
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
                color = Mvp.barText,
                fontSize = 13.sp,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = with(density) { bottomInset.toDp() } + 10.dp)
                    .padding(horizontal = 28.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .hazeEffect(state = haze, style = glass)
                    .border(1.dp, Mvp.cardBorder, RoundedCornerShape(16.dp))
                    .clickable { error = null }
                    .padding(horizontal = 16.dp, vertical = 10.dp),
            )
        }
    }

    if (showTags) {
        val chosen = videos.filter { it.id in selectedIds }
        Dialog(onDismissRequest = { if (!actionBusy) showTags = false }) {
            BlurredDialogWindow()
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
            BlurredDialogWindow()
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
            BlurredDialogWindow()
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
                            moveTarget = MoveRequest(videos = listOf(target.id))
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
            BlurredDialogWindow()
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
            BlurredDialogWindow()
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
                    else moveTarget = MoveRequest(folders = listOf(folder.id))
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
            BlurredDialogWindow()
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
            BlurredDialogWindow()
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
                if (!RemoteVault.connected()) MvpButton("Extensions…", onClick = {
                    showAdd = false
                    onOpenPlugins()
                }, modifier = Modifier.fillMaxWidth())
                if (!RemoteVault.connected()) MvpButton("Encrypted sync…", onClick = {
                    showAdd = false
                    onOpenSync()
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
            BlurredDialogWindow()
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
            BlurredDialogWindow()
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
            BlurredDialogWindow()
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
            BlurredDialogWindow()
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
                            SyncV4Auto.markDirty()
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
        val choices = remember(request, folders, currentFolderId) {
            moveChoices(folders, currentFolderId, request.folders)
        }
        Dialog(onDismissRequest = { moveTarget = null }) {
            BlurredDialogWindow()
            MvpCard(Modifier.fillMaxWidth()) {
                MvpTitle("Move to")
                MvpDescription(
                    when {
                        request.count == 1 && request.folders.isNotEmpty() -> "Pick where this folder should live. Library is the vault root."
                        request.count == 1 -> "Pick a folder for this item. Library is the vault root."
                        else -> "Pick a folder for these ${request.count} items. Library is the vault root."
                    },
                )
                Column(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(max = 340.dp)
                        .verticalScroll(rememberScrollState()),
                ) {
                    choices.forEach { choice ->
                        MoveChoiceRow(choice) {
                            val chosen = choice.id
                            val moving = request
                            moveTarget = null
                            moveItems(moving.videos, moving.folders, chosen)
                        }
                    }
                }
                MvpDescription("Greyed rows can't take them — the reason is under the name.")
                MvpButton("Cancel", onClick = { moveTarget = null }, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}

/** One folder in the "Move to" picker: indented by its depth in the tree. */
@Composable
private fun MoveChoiceRow(choice: MoveChoice, onPick: () -> Unit) {
    val enabled = !choice.refused
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .clickable(enabled = enabled, onClick = onPick)
            .padding(start = (6 + choice.depth * 16).dp, end = 6.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        FolderGlyph(Modifier.size(if (choice.depth == 0) 22.dp else 18.dp))
        Column(Modifier.weight(1f)) {
            Text(
                choice.label,
                color = if (enabled) Mvp.text else Mvp.placeholder,
                fontSize = 15.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            choice.reason?.let { reason ->
                Text(reason, color = Mvp.errorText, fontSize = 11.sp)
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
            .background(Mvp.window)
            .statusBarsPadding()
            .navigationBarsPadding(),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(start = 8.dp, end = 16.dp, top = 8.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            HeaderIcon("Close", R.drawable.ic_lucide_x, onClose)
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
                painterResource(R.drawable.ic_lucide_search),
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
                textStyle = TextStyle(color = Mvp.title, fontSize = 14.sp),
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
    filterTagIds: Set<Long>,
    search: String,
    selectionTitle: String?,
    actionsEnabled: Boolean,
    onViewMode: (Int) -> Unit,
    onToggleTag: (Long) -> Unit,
    onClearTags: () -> Unit,
    onSearch: (String) -> Unit,
    onMusic: () -> Unit,
    onUpdate: () -> Unit,
    updateReady: Boolean,
    updateStaged: Boolean,
    onInstallUpdate: () -> Unit,
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
                HeaderIcon("Clear selection", R.drawable.ic_lucide_x, onClearSelection)
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
                HeaderIcon("Restore", R.drawable.ic_lucide_download, onRestore, enabled = actionsEnabled)
                HeaderIcon("Remove", R.drawable.ic_lucide_trash, onRemove, enabled = actionsEnabled, iconTint = Mvp.errorText)
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
            if (updateStaged) {
                Text(
                    "Install",
                    color = Mvp.accent,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier
                        .clip(RoundedCornerShape(16.dp))
                        .clickable(onClick = onInstallUpdate)
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                )
            }
            if (updateReady) {
                HeaderIcon("Update", R.drawable.ic_lucide_download, onUpdate)
            }
            // "Music" used to be a bare accent word sitting between icon
            // buttons, so it read like a caption for the plus beside it. A chip
            // with a mark and a tint says "this opens a place".
            Row(
                Modifier
                    .clip(RoundedCornerShape(16.dp))
                    .background(Mvp.accent.copy(alpha = 0.12f))
                    .clickable(onClick = onMusic)
                    .padding(start = 8.dp, end = 10.dp, top = 6.dp, bottom = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    painterResource(R.drawable.msg_media),
                    contentDescription = null,
                    tint = Mvp.accent,
                    modifier = Modifier.size(16.dp),
                )
                Text(
                    "Music",
                    color = Mvp.accent,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.padding(start = 5.dp),
                )
            }
            HeaderIcon("Add", R.drawable.ic_lucide_plus, onImport, filled = true)
            HeaderIcon("Settings", R.drawable.ic_lucide_settings, onSettings)
            HeaderIcon("Lock", R.drawable.ic_lucide_lock, onLock)
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
                    .clip(RoundedCornerShape(10.dp))
                    .background(Mvp.inputBg)
                    .border(1.dp, Mvp.inputBorder, RoundedCornerShape(10.dp))
                    .padding(horizontal = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    painterResource(R.drawable.ic_lucide_search),
                    contentDescription = null,
                    tint = Mvp.placeholder,
                    modifier = Modifier.size(16.dp),
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
                                Text("Search names or tags", color = Mvp.placeholder, fontSize = 14.sp,
                                    maxLines = 1, overflow = TextOverflow.Ellipsis)
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
                            painterResource(R.drawable.ic_lucide_x),
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
                TagFilterButton(tags, filterTagIds, onToggleTag, onClearTags)
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
        if (selected) Mvp.title else Mvp.description,
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

/**
 * The cloud line in the footer: what the background sync is doing, in one
 * glance. It is silent when nothing is linked, quiet while up to date, and
 * turns into a retry when the relay cannot be reached. Tapping it opens the
 * full Sync page — the pill never interrupts browsing.
 */
@Composable
private fun SyncStatusPill(onOpen: () -> Unit) {
    val phase = SyncV4Auto.phase
    if (phase is SyncV4Auto.Phase.Off) return
    var now by remember { mutableStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(5_000)
            now = System.currentTimeMillis()
        }
    }
    val busy = phase is SyncV4Auto.Phase.Busy
    val failed = phase is SyncV4Auto.Phase.Failed
    val tint = when {
        failed -> Mvp.errorText
        busy -> Mvp.accent
        else -> Mvp.barText.copy(alpha = 0.72f)
    }
    val text = when (phase) {
        is SyncV4Auto.Phase.Busy -> phase.label
        is SyncV4Auto.Phase.Failed -> "Sync paused · tap"
        else -> SyncV4Auto.statusText(now)
    }
    Row(
        Modifier
            .clip(RoundedCornerShape(8.dp))
            .clickable(onClick = onOpen)
            .padding(horizontal = 6.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            if (busy) "\u27F3" else if (failed) "\u26A0" else "\u2713",
            color = tint,
            fontSize = 11.sp,
        )
        Text(
            text,
            color = tint,
            fontSize = 11.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(start = 4.dp),
        )
    }
}

@Composable
private fun TagFilterButton(
    tags: List<TagEntry>,
    selected: Set<Long>,
    onToggle: (Long) -> Unit,
    onClear: () -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }
    Box {
        ViewModeButton(R.drawable.ic_lucide_list, "Filter by tags", selected.isNotEmpty()) { open = true }
        if (open) {
            Dialog(onDismissRequest = { open = false }) {
                BlurredDialogWindow()
                Column(
                    Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(16.dp))
                        .background(Mvp.card)
                        .border(1.dp, Mvp.cardBorder, RoundedCornerShape(16.dp))
                        .padding(14.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Tags", color = Mvp.title, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
                        Spacer(Modifier.weight(1f))
                        Text(
                            "Clear",
                            color = if (selected.isEmpty()) Mvp.placeholder else Mvp.accent,
                            fontSize = 15.sp,
                            fontWeight = FontWeight.SemiBold,
                            modifier = Modifier
                                .clip(RoundedCornerShape(8.dp))
                                .clickable(enabled = selected.isNotEmpty()) { onClear() }
                                .padding(horizontal = 8.dp, vertical = 6.dp),
                        )
                    }
                    Spacer(Modifier.height(10.dp))
                    BasicTextField(
                        value = query,
                        onValueChange = { query = it },
                        singleLine = true,
                        textStyle = TextStyle(color = Mvp.text, fontSize = 15.sp),
                        cursorBrush = SolidColor(Mvp.accent),
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(10.dp))
                            .background(Mvp.inputBg)
                            .border(1.dp, Mvp.inputBorder, RoundedCornerShape(10.dp))
                            .padding(horizontal = 12.dp, vertical = 10.dp),
                        decorationBox = { inner ->
                            Box(contentAlignment = Alignment.CenterStart) {
                                if (query.isEmpty()) {
                                    Text("Search tags…", color = Mvp.placeholder, fontSize = 15.sp)
                                }
                                inner()
                            }
                        },
                    )
                    Spacer(Modifier.height(6.dp))
                    val shown = tags.filter {
                        query.isBlank() || it.name.contains(query.trim(), ignoreCase = true)
                    }
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(max = 300.dp)
                            .verticalScroll(rememberScrollState()),
                    ) {
                        if (shown.isEmpty()) {
                            Text(
                                if (tags.isEmpty()) "No tags yet" else "No matching tags",
                                color = Mvp.description,
                                fontSize = 14.sp,
                                modifier = Modifier.padding(horizontal = 8.dp, vertical = 16.dp),
                            )
                        }
                        shown.forEach { tag ->
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .clip(RoundedCornerShape(8.dp))
                                    .clickable { onToggle(tag.id) }
                                    .padding(horizontal = 8.dp, vertical = 12.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(
                                    tag.name,
                                    color = Mvp.text,
                                    fontSize = 15.sp,
                                    modifier = Modifier.weight(1f),
                                )
                                Text("${tag.videoCount}", color = Mvp.description, fontSize = 13.sp)
                                Spacer(Modifier.width(10.dp))
                                if (tag.id in selected) {
                                    Icon(
                                        painterResource(R.drawable.ic_lucide_check),
                                        contentDescription = "Selected",
                                        tint = Mvp.accent,
                                        modifier = Modifier.size(18.dp),
                                    )
                                } else {
                                    Box(
                                        Modifier
                                            .size(18.dp)
                                            .clip(CircleShape)
                                            .border(1.dp, Mvp.placeholder, CircleShape),
                                    )
                                }
                            }
                        }
                    }
                    Text(
                        if (selected.isEmpty()) "No selection shows everything."
                        else "${selected.size} tag${if (selected.size == 1) "" else "s"} selected — items with any of them are shown.",
                        color = Mvp.description,
                        fontSize = 12.sp,
                        modifier = Modifier.padding(top = 6.dp, start = 4.dp),
                    )
                }
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
            modifier = Modifier.size(20.dp),
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

/**
 * What the "Move to…" picker is about to place in a folder. Both kinds are
 * carried because a folder row and a file selection use the same picker.
 */
private data class MoveRequest(val videos: List<Long> = emptyList(), val folders: List<Long> = emptyList()) {
    val count: Int get() = videos.size + folders.size
}

/** One row of the picker: a folder in the tree, indented by [depth]. */
private data class MoveChoice(val id: Long, val label: String, val depth: Int, val reason: String?) {
    val refused: Boolean get() = reason != null
}

/**
 * The whole vault tree, depth first from the library root, with the reason each
 * row cannot take the items. Refused rows stay in the list on purpose: a folder
 * that simply vanished from the picker explains nothing, a row that says "this
 * is the current folder" does.
 */
private fun moveChoices(
    folders: List<FolderEntry>,
    currentFolderId: Long,
    moving: List<Long>,
): List<MoveChoice> {
    val blocked = HashSet<Long>()
    for (id in moving) if (id > 0L) blocked.addAll(folderSubtree(folders, id))
    val children = folders.groupBy { it.parentId }
    fun reasonFor(id: Long): String? = when {
        id in blocked && id in moving -> "This is the folder being moved."
        id in blocked -> "Inside a folder being moved."
        id == currentFolderId -> "This is the current folder."
        else -> null
    }
    val out = ArrayList<MoveChoice>()
    out.add(MoveChoice(0L, "Library", 0, reasonFor(0L)))
    val seen = HashSet<Long>()
    fun walk(parent: Long, depth: Int) {
        if (depth > 32) return
        children[parent].orEmpty().sortedBy { it.name.lowercase() }.forEach { folder ->
            if (!seen.add(folder.id)) return@forEach
            out.add(MoveChoice(folder.id, folder.name, depth, reasonFor(folder.id)))
            walk(folder.id, depth + 1)
        }
    }
    walk(0L, 1)
    return out
}

/** The payload of an in-flight drag, from this app or from another one. */
private fun dragPayload(event: DragAndDropEvent): VaultDrag? {
    val drag = event.toAndroidDragEvent()
    (drag.localState as? VaultDrag)?.let { return it }
    val clip = drag.clipData ?: return null
    for (index in 0 until clip.itemCount) {
        val text = clip.getItemAt(index).text?.toString() ?: continue
        parseVaultDrag(text)?.let { return it }
    }
    return null
}

/**
 * Why a drop on [destination] cannot happen, or null when it can. The folder
 * row highlight and the message after the drop both come from here, so what the
 * row promises and what the drop does cannot disagree.
 */
private fun dropRefusal(drag: VaultDrag, destination: Long): String? {
    if (destination == PEER_FOLDER_ID) return "The PC's library is read-only."
    val allFolders = GalleryCache.folders
    for (id in drag.folders) {
        if (id <= 0L) continue
        if (id == destination) return "A folder can't be moved into itself."
        if (folderContains(allFolders, id, destination)) return "A folder can't be moved into its own subfolder."
    }
    val total = drag.videos.size + drag.folders.size
    if (total == 0) return null
    val allVideos = GalleryCache.videos
    val here = drag.videos.count { id -> allVideos.firstOrNull { it.id == id }?.folderId == destination } +
        drag.folders.count { id -> allFolders.firstOrNull { it.id == id }?.parentId == destination }
    if (here == total) return "Already in this folder."
    return null
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
 * A folder row's own drop target. The hint belongs to the row so it can paint
 * the drag that is over it: blue when the drop will land, red with the reason
 * when it is refused — the same reason the message gives after the drop.
 */
private class DropHint {
    var hot by mutableStateOf(false)
    var reason by mutableStateOf<String?>(null)
}

@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.folderDropTarget(
    hint: DropHint,
    refusal: (DragAndDropEvent) -> String?,
    onDrop: (DragAndDropEvent) -> Boolean,
): Modifier = this.dragAndDropTarget(
    shouldStartDragAndDrop = ::acceptsVaultDrop,
    target = object : DragAndDropTarget {
        private fun clear() {
            hint.hot = false
            hint.reason = null
        }

        override fun onEntered(event: DragAndDropEvent) {
            hint.hot = true
            hint.reason = refusal(event)
        }

        override fun onExited(event: DragAndDropEvent) { clear() }
        override fun onEnded(event: DragAndDropEvent) { clear() }

        override fun onDrop(event: DragAndDropEvent): Boolean {
            clear()
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
        drawRoundRect(color = Mvp.accent.copy(alpha = 0.8f), cornerRadius = CornerRadius(24f, 24f))
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
    val upId = if (currentFolderId == 0L) {
        null
    } else {
        crumb.lastOrNull()?.parentId ?: 0L
    }
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Back goes up a folder, but a phone has no address bar: keep an
        // explicit target on screen for the level above.
        if (upId != null) {
            Row(
                Modifier
                    .clip(RoundedCornerShape(6.dp))
                    .background(Mvp.glass)
                    .clickable { onOpen(upId) }
                    .padding(horizontal = 6.dp, vertical = 3.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                Icon(
                    painterResource(R.drawable.ic_lucide_arrow_left),
                    contentDescription = "Up",
                    tint = Mvp.accent,
                    modifier = Modifier.size(14.dp),
                )
                Text("Up", color = Mvp.accent, fontSize = 13.sp)
            }
            Text("/", color = Mvp.description, fontSize = 13.sp)
        }
        // At the root the page title already says "Library"; a crumb that
        // repeats it is noise. Inside a folder the trail is the way back.
        if (currentFolderId != 0L) {
            Crumb("Library", current = false, onClick = { onOpen(0L) }, onDrop = { onDrop(it, 0L) })
        }
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
        color = if (hot) Color.White else if (current) Mvp.title else Mvp.accent,
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
        painterResource(R.drawable.ic_lucide_folder),
        contentDescription = null,
        tint = Mvp.accent,
        modifier = modifier,
    )
}

private fun folderModifier(
    onClick: () -> Unit,
    onLongPress: (() -> Unit)?,
    transfer: (() -> DragAndDropTransferData)?,
    hint: DropHint,
    refusal: (DragAndDropEvent) -> String?,
    onDrop: (DragAndDropEvent) -> Boolean,
): Modifier {
    val base = Modifier.folderDropTarget(hint, refusal, onDrop)
    return if (transfer != null && onLongPress != null) {
        base.explorerItem(onClick = onClick, onLongPress = onLongPress, transfer = transfer)
    } else {
        base.clickable(onClick = onClick)
    }
}

@Composable
internal fun FolderDetailRow(
    name: String,
    meta: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
    refusal: (DragAndDropEvent) -> String? = { null },
) {
    val hint = remember { DropHint() }
    val refused = hint.reason
    val rowColor by animateColorAsState(
        when {
            refused != null -> Mvp.errorBg
            hint.hot -> Mvp.accent.copy(alpha = 0.35f)
            else -> Mvp.inputBg
        },
        tween(160),
        label = "folderRow",
    )
    Row(
        modifier
            .fillMaxWidth()
            .background(rowColor)
            .then(folderModifier(onClick, onLongPress, transfer, hint, refusal, onDrop))
            .padding(horizontal = 10.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FolderGlyph(Modifier.size(44.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(name, color = Mvp.text, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                refused ?: meta,
                color = if (refused != null) Mvp.errorText else Mvp.description,
                fontSize = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (onLongPress != null) {
            RowActions(onLongPress)
        }
    }
}

@Composable
internal fun FolderListRow(
    name: String,
    meta: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
    refusal: (DragAndDropEvent) -> String? = { null },
) {
    val hint = remember { DropHint() }
    val refused = hint.reason
    val rowColor by animateColorAsState(
        when {
            refused != null -> Mvp.errorBg
            hint.hot -> Mvp.accent.copy(alpha = 0.35f)
            else -> Mvp.inputBg
        },
        tween(160),
        label = "folderListRow",
    )
    Row(
        modifier
            .fillMaxWidth()
            .background(rowColor)
            .then(folderModifier(onClick, onLongPress, transfer, hint, refusal, onDrop))
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FolderGlyph(Modifier.size(36.dp))
        Column(Modifier.weight(1f).padding(start = 10.dp)) {
            Text(name, color = Mvp.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                refused ?: meta,
                color = if (refused != null) Mvp.errorText else Mvp.description,
                fontSize = 11.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (onLongPress != null) {
            RowActions(onLongPress)
        }
    }
}

@Composable
internal fun FolderTile(
    name: String,
    onClick: () -> Unit,
    transfer: (() -> DragAndDropTransferData)?,
    onDrop: (DragAndDropEvent) -> Boolean,
    onLongPress: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
    place: String = "",
    refusal: (DragAndDropEvent) -> String? = { null },
) {
    val hint = remember { DropHint() }
    val refused = hint.reason
    Box(
        modifier
            .fillMaxWidth()
            .aspectRatio(1f)
            .background(
                when {
                    refused != null -> Mvp.errorBg
                    hint.hot -> Mvp.accent.copy(alpha = 0.35f)
                    else -> Mvp.tileBg
                },
            )
            .then(
                if (refused != null) {
                    Modifier.border(2.dp, Mvp.errorText, RoundedCornerShape(2.dp))
                } else {
                    Modifier
                },
            )
            .then(folderModifier(onClick, onLongPress, transfer, hint, refusal, onDrop)),
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
                color = Mvp.tileText,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (place.isNotEmpty()) {
                Text(
                    place,
                    color = Mvp.tileText,
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
        if (hint.hot) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(
                        if (refused != null) Color.Transparent else Mvp.accent.copy(alpha = 0.35f),
                    ),
            )
        }
        refused?.let { reason ->
            Text(
                reason,
                color = Mvp.errorText,
                fontSize = 10.sp,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .fillMaxWidth()
                    .background(Mvp.errorBg.copy(alpha = 0.94f))
                    .padding(horizontal = 6.dp, vertical = 3.dp),
            )
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
                    SyncV4Auto.markDirty()
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
            Text(people, color = Mvp.text.copy(alpha = 0.9f), fontSize = 12.sp, maxLines = 2)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            listOf("slow" to "Slow", "normal" to "Normal", "fast" to "Fast").forEach { (id, label) ->
                Text(
                    label,
                    color = if (speed == id) Mvp.accent else Mvp.text.copy(alpha = 0.55f),
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
                    .background(Mvp.primary, CircleShape),
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
            // Background decodes must not crowd out playback: at most two at a
            // time, and none at all while a player is on screen.
            if (VaultWork.playback) return@withContext null
            VaultWork.thumbs.withPermit {
                if (VaultWork.playback) return@withPermit null
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
    Box(modifier.background(Mvp.tileBg), contentAlignment = Alignment.Center) {
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
    Box(modifier.background(Mvp.tileBg), contentAlignment = Alignment.Center) {
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
            // Same gate as the list rows: playback keeps the vault.
            if (VaultWork.playback) return@withContext null
            VaultWork.thumbs.withPermit {
                if (VaultWork.playback) return@withPermit null
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
private fun DetailSortBar(onSort: (SortKey) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Mvp.headerBg)
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // The tap target is the chip; the label says what the row of chips is.
        Text("Sort", color = Mvp.description, fontSize = 12.sp, modifier = Modifier.padding(start = 2.dp))
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
                    .clickable { onSort(key) }
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
    scrollKey: String = "0",
    onSort: (SortKey) -> Unit = {},
    dropRefusalOn: (DragAndDropEvent, Long) -> String? = { _, _ -> null },
    providerSearch: List<PluginManifest> = emptyList(),
    searchQuery: String = "",
    onSearchProvider: (PluginManifest) -> Unit = {},
) {
    LazyColumn(
        state = GalleryCache.listScroll("details:$scrollKey"),
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Mvp.window).vaultDropTarget(onActive = {}, onDrop = onDrop),
    ) {
        stickyHeader { DetailSortBar(onSort) }
        items(providerSearch, key = { "provider:${it.id}" }) { manifest ->
            PluginSearchEntry(manifest, searchQuery, 0) { onSearchProvider(manifest) }
        }
        if (upParentId != null) {
            item(key = "up") {
                FolderDetailRow(
                    "..", "Up", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) },
                    refusal = { event -> dropRefusalOn(event, upParentId) },
                )
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
                refusal = { event -> dropRefusalOn(event, folder.id) },
            )
        }
        itemsIndexed(videos, key = { _, v -> "${GalleryCache.generation}:${v.id}" }) { index, v ->
            val media = rememberMediaInfo(v.id)
            val picked = v.id in selected
            val stripe = if (index % 2 == 0) Mvp.alternateRow else Mvp.window
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
                    .padding(horizontal = 12.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FileTile(v, 44, square = false)
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
                Text(
                    v.sizeText,
                    color = Mvp.description,
                    fontSize = 12.sp,
                    maxLines = 1,
                )
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
    scrollKey: String = "0",
    onSort: (SortKey) -> Unit = {},
    dropRefusalOn: (DragAndDropEvent, Long) -> String? = { _, _ -> null },
    providerSearch: List<PluginManifest> = emptyList(),
    searchQuery: String = "",
    onSearchProvider: (PluginManifest) -> Unit = {},
) {
    LazyVerticalGrid(
        columns = GridCells.Fixed(3),
        state = GalleryCache.gridScroll("icons:$scrollKey"),
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Mvp.window).vaultDropTarget(onActive = {}, onDrop = onDrop),
        horizontalArrangement = Arrangement.spacedBy(0.dp),
        verticalArrangement = Arrangement.spacedBy(0.dp),
    ) {
        item(span = { GridItemSpan(3) }) { ListSortBar(onSort) }
        items(providerSearch, key = { "provider:${it.id}" }, span = { GridItemSpan(3) }) { manifest ->
            PluginSearchEntry(manifest, searchQuery, 2) { onSearchProvider(manifest) }
        }
        if (upParentId != null) {
            item(key = "up") {
                FolderTile(
                    "..", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) },
                    refusal = { event -> dropRefusalOn(event, upParentId) },
                )
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
                refusal = { event -> dropRefusalOn(event, folder.id) },
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
                    .padding(3.dp)
                    .clip(RoundedCornerShape(8.dp))
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
                                color = Mvp.tileText,
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
                            painterResource(R.drawable.ic_lucide_check),
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
    scrollKey: String = "0",
    onSort: (SortKey) -> Unit = {},
    dropRefusalOn: (DragAndDropEvent, Long) -> String? = { _, _ -> null },
    providerSearch: List<PluginManifest> = emptyList(),
    searchQuery: String = "",
    onSearchProvider: (PluginManifest) -> Unit = {},
) {
    LazyColumn(
        state = GalleryCache.listScroll("list:$scrollKey"),
        contentPadding = padding,
        modifier = Modifier.fillMaxSize().background(Mvp.window).vaultDropTarget(onActive = {}, onDrop = onDrop),
    ) {
        item { ListSortBar(onSort) }
        items(providerSearch, key = { "provider:${it.id}" }) { manifest ->
            PluginSearchEntry(manifest, searchQuery, 2) { onSearchProvider(manifest) }
        }
        if (upParentId != null) {
            item(key = "up") {
                FolderListRow(
                    "..", "Up", { onOpenFolder(upParentId) }, null, { onDropOn(it, upParentId) },
                    refusal = { event -> dropRefusalOn(event, upParentId) },
                )
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
                refusal = { event -> dropRefusalOn(event, folder.id) },
            )
        }
        items(videos, key = { "${GalleryCache.generation}:${it.id}" }) { v ->
            val picked = v.id in selected
            val stripe = if (videos.indexOf(v) % 2 == 0) Mvp.alternateRow else Mvp.window
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
                    .padding(horizontal = 12.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FileTile(v, 40, square = false)
                Column(Modifier.weight(1f).padding(start = 10.dp)) {
                    Text(v.name, color = Mvp.text, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    videoPlace[v.id]?.let { place ->
                        Text(place, color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    if (v.tags.isNotEmpty()) {
                        Text(v.tags.joinToString(", "), color = Mvp.description, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                }
                Text(v.sizeText, color = Mvp.description, fontSize = 12.sp, maxLines = 1)
                RowActions { onAction(v) }
            }
        }
    }
}

@Composable
private fun ListSortBar(onSort: (SortKey) -> Unit) {
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
                        onSort(item)
                    },
                )
            }
        }
    }
}
