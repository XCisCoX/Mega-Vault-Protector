@file:OptIn(androidx.media3.common.util.UnstableApi::class)

package org.megavideoprotect.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.runtime.*
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.PlaybackException
import androidx.media3.datasource.ByteArrayDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.withPermit
import java.util.concurrent.ConcurrentHashMap

/** The session is never shared with MusicPlaybackService or the normal library. */
object PluginRuntime {
    var session: PluginSession? = null
    var actions: ExtensionActionSession? = null
    fun close() { session?.close(); session = null; actions?.close(); actions = null }
}

class PluginSession(
    private val context: Context,
    val manifest: PluginManifest,
    private val vaultRoot: String,
    private val options: PluginOptions,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val provider = PluginProviderRegistry.adapter(manifest.protocol)
    private val requests = ConcurrentHashMap.newKeySet<PluginHttp>()
    private val catalogueRequests = ConcurrentHashMap.newKeySet<PluginHttp>()
    private val playbackRequests = ConcurrentHashMap.newKeySet<PluginHttp>()
    private val artworkBitmaps = ConcurrentHashMap.newKeySet<android.graphics.Bitmap>()
    private var generation = 0L
    private var catalogueGeneration = 0L
    @Volatile private var closed = false
    private var loading: Job? = null
    private var catalogueLoading: Job? = null
    private var saving = false
    private var bytes: ByteArray? = null
    private var retired: ByteArray? = null
    private var playOrder = emptyList<PluginItem>()
    var rows by mutableStateOf<List<PluginItem>>(emptyList()); private set
    var heading by mutableStateOf("Search songs, artists or albums"); private set
    var catalogueMessage by mutableStateOf(""); private set
    var playbackMessage by mutableStateOf(""); private set
    var catalogueBusy by mutableStateOf(false); private set
    var playbackBusy by mutableStateOf(false); private set
    val message: String get() = catalogueMessage.ifBlank { playbackMessage }
    val busy: Boolean get() = catalogueBusy || playbackBusy
    var selected by mutableStateOf<PluginItem?>(null); private set
    /** A playback snapshot: later browsing never replaces this playlist. */
    var playableRows by mutableStateOf<List<PluginItem>>(emptyList()); private set
    var shuffleEnabled by mutableStateOf(false); private set
    var repeatMode by mutableStateOf(Player.REPEAT_MODE_OFF); private set
    var playing by mutableStateOf(false); private set
    var saved by mutableStateOf(false); private set
    var saveBusy by mutableStateOf(false); private set
    /**
     * Per-track download state for rows in the library folder, independent of
     * whatever is playing: "downloading", "saving", "saved" or a failure text.
     */
    var downloadStates by mutableStateOf<Map<String, String>>(emptyMap()); private set
    private val downloadPermits = kotlinx.coroutines.sync.Semaphore(2)
    val canDownload: Boolean get() = !closed && !playbackBusy && !saveBusy && !saved && selected != null && bytes != null
    val player = ExoPlayer.Builder(context).build().apply {
        setAudioAttributes(AudioAttributes.Builder().setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(), true)
        setHandleAudioBecomingNoisy(true)
        addListener(object : Player.Listener {
            override fun onIsPlayingChanged(isPlaying: Boolean) {
                playing = isPlaying
                // Readiness or track selection is NOT playback.
                if (isPlaying && options.autoSave && !saved && !saving) download()
            }
            override fun onPlayerError(error: PlaybackException) {
                if (!closed) playbackMessage = "Audio playback failed. Try loading the track again."
            }
            override fun onPlaybackStateChanged(playbackState: Int) {
                if (!closed && playbackState == Player.STATE_ENDED && !saving && !playbackBusy) advance(1)
            }
        })
    }
    init { require(options.enabled); PluginRuntime.close(); PluginRuntime.session = this }

    private suspend fun <T> network(group: MutableSet<PluginHttp>? = null, block: (PluginHttp) -> T): T {
        currentCoroutineContext().ensureActive()
        check(!closed)
        val client = PluginHttp(manifest)
        requests.add(client); group?.add(client)
        try { return withContext(Dispatchers.IO) { block(client) } }
        finally { client.close(); requests.remove(client); group?.remove(client) }
    }
    suspend fun artwork(url: String): ByteArray {
        var data: ByteArray? = null
        try {
            network { data = it.image(url) }
            currentCoroutineContext().ensureActive()
            return checkNotNull(data).also { data = null }
        } finally { data?.fill(0) }
    }
    fun ownArtwork(bitmap: android.graphics.Bitmap) {
        if (closed) bitmap.eraseColor(android.graphics.Color.TRANSPARENT)
        else artworkBitmaps.add(bitmap)
    }
    fun releaseArtwork(bitmap: android.graphics.Bitmap) {
        artworkBitmaps.remove(bitmap)
        runCatching { bitmap.eraseColor(android.graphics.Color.TRANSPARENT) }
    }
    /** Clear stale rows immediately; failure/timeout leaves an empty, retryable catalogue. */
    fun browse(query: String? = null, item: PluginItem? = null) {
        if (closed) return
        val token = ++catalogueGeneration
        catalogueLoading?.cancel(); catalogueRequests.forEach { it.close() }
        rows = emptyList(); heading = item?.title ?: "Results · ${manifest.name}"
        catalogueBusy = true; catalogueMessage = ""
        catalogueLoading = scope.launch {
            // Sibling watchdog clears UI even if platform DNS/IO ignores cancellation.
            // Only catalogue clients are closed: audio and artwork are independent.
            val deadline = scope.launch {
                delay(15_000)
                if (token == catalogueGeneration && !closed) {
                    catalogueGeneration++
                    catalogueBusy = false
                    catalogueMessage = "Catalogue request timed out after 15 seconds. Try again."
                    catalogueLoading?.cancel()
                    catalogueRequests.forEach { it.close() }
                }
            }
            try {
                val path = provider.cataloguePath(query, item)
                val data = network(catalogueRequests) { it.json(path) }
                ensureActive()
                if (token == catalogueGeneration && !closed) {
                    rows = provider.normalize(data, item?.kind)
                    if (rows.isEmpty()) catalogueMessage = "No results"
                }
            } catch (_: CancellationException) { }
            catch (_: Exception) {
                if (token == catalogueGeneration && !closed) catalogueMessage = "Catalogue request failed. Check the connection or try another search."
            } finally {
                deadline.cancel()
                if (token == catalogueGeneration && !closed) catalogueBusy = false
            }
        }
    }
    /** Only provider tracks enter this queue; their identifiers never become vault IDs. */
    fun play(item: PluginItem, playlist: List<PluginItem> = rows) {
        if (closed || item.kind != "track" || !item.playable) return
        if (saving) { playbackMessage = "Wait for the encrypted import to finish before changing tracks."; return }
        val nextRows = (playlist.filter { it.kind == "track" && it.playable } + item).distinctBy { it.id }
        if (nextRows != playableRows) {
            playableRows = nextRows
            rebuildOrder(item)
        }
        loadTrack(item)
    }
    private fun loadTrack(item: PluginItem) {
        if (closed) return
        val token = ++generation
        loading?.cancel(); playbackRequests.forEach { it.close() }
        player.stop(); player.clearMediaItems(); bytes?.fill(0); bytes = null
        selected = item; saved = false; playbackBusy = true; playbackMessage = ""
        loading = scope.launch {
            var loaded: ByteArray? = null
            try {
                // Keep ownership even if withContext's prompt cancellation discards its result.
                network(playbackRequests) { loaded = it.audio(provider.audioPath(item), options.memoryMiB * 1024 * 1024) }
                ensureActive()
                if (token != generation || closed) return@launch
                if (MusicHub.active) context.startService(Intent(context, MusicPlaybackService::class.java).setAction(MusicPlaybackService.ACTION_STOP))
                val audio = checkNotNull(loaded)
                bytes = audio; loaded = null
                val source = ProgressiveMediaSource.Factory(DataSource.Factory { ByteArrayDataSource(audio) })
                    .createMediaSource(MediaItem.Builder().setMediaId(item.id)
                        .setUri(Uri.parse("memory://plugin/${item.id}")).setMimeType("audio/flac")
                        .setMediaMetadata(MediaMetadata.Builder().setTitle(item.title).setArtist(item.subtitle).build()).build())
                player.setMediaSource(source); player.prepare(); player.play()
            } catch (_: CancellationException) { }
            catch (e: PluginLimitException) {
                if (token == generation && !closed) playbackMessage = e.message.orEmpty()
            } catch (_: Exception) {
                if (token == generation && !closed) playbackMessage = "Audio could not be loaded or was not verified FLAC. Nothing was cached or saved."
            } finally { loaded?.fill(0); if (token == generation && !closed) playbackBusy = false }
        }
    }
    private fun rebuildOrder(anchor: PluginItem? = selected) {
        playOrder = if (shuffleEnabled) {
            val rest = playableRows.filter { it.id != anchor?.id }.shuffled()
            playableRows.firstOrNull { it.id == anchor?.id }?.let { listOf(it) + rest } ?: rest
        } else playableRows
    }
    fun setShuffle(enabled: Boolean) {
        if (closed) return
        shuffleEnabled = enabled; rebuildOrder()
    }
    fun setRepeat(mode: Int) {
        require(mode in setOf(Player.REPEAT_MODE_OFF, Player.REPEAT_MODE_ONE, Player.REPEAT_MODE_ALL))
        if (closed) return
        repeatMode = mode
        // One RAM track in ExoPlayer; list repeat is implemented by advance().
        player.repeatMode = if (mode == Player.REPEAT_MODE_ONE) Player.REPEAT_MODE_ONE else Player.REPEAT_MODE_OFF
    }
    fun reversePlaylist() {
        if (closed) return
        playableRows = playableRows.asReversed(); playOrder = playOrder.asReversed()
    }
    fun previous() {
        if (closed) return
        if (!playbackBusy && player.currentPosition > 3000L) player.seekTo(0L) else advance(-1)
    }
    fun next() { advance(1) }
    private fun advance(direction: Int) {
        if (closed || playOrder.isEmpty()) return
        if (saving) { playbackMessage = "Wait for the encrypted import to finish before changing tracks."; return }
        val at = playOrder.indexOfFirst { it.id == selected?.id }
        if (at < 0) return
        val target = at + direction
        val next = when {
            target in playOrder.indices -> playOrder[target]
            repeatMode == Player.REPEAT_MODE_ALL -> if (direction > 0) playOrder.first() else playOrder.last()
            else -> return
        }
        loadTrack(next)
    }
    fun toggle() {
        if (closed || playbackBusy) return
        if (player.isPlaying) player.pause()
        else if (player.playerError != null || bytes == null) selected?.let { loadTrack(it) }
        else { if (player.playbackState == Player.STATE_ENDED) player.seekTo(0L); player.play() }
    }
    /**
     * Downloads one row's track without playing it: RAM only, verified FLAC,
     * straight into the encrypted vault. At most two rows download at once.
     */
    fun downloadTrack(item: PluginItem) {
        if (closed || item.kind != "track" || !item.playable) return
        val state = downloadStates[item.id]
        if (state == "downloading" || state == "saving" || state == "saved") return
        setDownload(item.id, "downloading")
        scope.launch {
            var audio: ByteArray? = null
            try {
                downloadPermits.withPermit {
                    network { audio = it.audio(provider.audioPath(item), options.memoryMiB * 1024 * 1024) }
                    val bytes = checkNotNull(audio)
                    if (closed) return@withPermit
                    setDownload(item.id, "saving")
                    val result = withContext(Dispatchers.IO + NonCancellable) {
                        require(!RemoteVault.connected() && CoreBridge.nativeIsUnlocked())
                        require((VaultLocation.saved(context) ?: context.filesDir.absolutePath) == vaultRoot)
                        val folders = Json.folders(CoreBridge.nativeListFolders())
                        require(options.folderId == 0L || folders.any { it.id == options.folderId }) { "Destination folder was removed" }
                        CoreBridge.Result.parse(CoreBridge.nativeImportBytesForVault(
                            bytes, PluginAudio.fileName(item.title, item.subtitle), options.folderId, vaultRoot))
                    }
                    if (!closed) {
                        GalleryCache.clear()
                        setDownload(item.id, if (result.ok) "saved" else result.error.ifBlank { "failed" }.take(120))
                    }
                }
            } catch (e: PluginLimitException) {
                if (!closed) setDownload(item.id, e.message.orEmpty().ifBlank { "too large" }.take(120))
            } catch (_: CancellationException) {
                if (!closed) setDownload(item.id, "")
            } catch (_: Exception) {
                if (!closed) setDownload(item.id, "failed")
            } finally { audio?.fill(0) }
        }
    }
    private fun setDownload(id: String, value: String) {
        downloadStates = downloadStates + (id to value)
    }
    fun stopPlayback() {
        if (closed) return
        generation++
        loading?.cancel(); playbackRequests.forEach { it.close() }
        player.stop(); player.clearMediaItems()
        // An import in flight still owns these bytes; it erases them when it returns.
        if (saving) retired = bytes else bytes?.fill(0)
        bytes = null; selected = null; saved = false
        playableRows = emptyList(); playOrder = emptyList()
        playbackBusy = false; playbackMessage = ""
    }
    fun download() {
        val audio = bytes ?: return
        val track = selected ?: return
        if (saving || saved || closed) return
        val token = generation
        val folder = options.folderId
        saving = true; saveBusy = true
        scope.launch {
            try {
                val result = withContext(Dispatchers.IO + NonCancellable) {
                    // Pin plugin, vault root, folder and bytes from the selected playback.
                    require(!closed && !RemoteVault.connected() && CoreBridge.nativeIsUnlocked())
                    require((VaultLocation.saved(context) ?: context.filesDir.absolutePath) == vaultRoot)
                    val folders = Json.folders(CoreBridge.nativeListFolders())
                    require(folder == 0L || folders.any { it.id == folder }) { "Destination folder was removed" }
                    val name = PluginAudio.fileName(track.title, track.subtitle)
                    CoreBridge.Result.parse(CoreBridge.nativeImportBytesForVault(audio, name, folder, vaultRoot))
                }
                if (!closed && token == generation) {
                    saved = result.ok
                    playbackMessage = if (result.ok) "Saved to encrypted vault (existing content is deduplicated)" else "Vault import failed: ${result.error}"
                    if (result.ok) GalleryCache.clear()
                }
            } catch (_: Exception) { if (!closed && token == generation) playbackMessage = "Cannot import: vault or destination changed, or encrypted import failed." }
            finally {
                saving = false; saveBusy = false
                retired?.fill(0); retired = null
                if (!closed && player.playbackState == Player.STATE_ENDED) advance(1)
            }
        }
    }
    fun close() {
        if (closed) return
        closed = true; generation++; catalogueGeneration++
        requests.forEach { it.close() }; requests.clear(); scope.cancel()
        player.stop(); player.clearMediaItems(); player.release()
        artworkBitmaps.forEach { runCatching { it.eraseColor(android.graphics.Color.TRANSPARENT) } }
        artworkBitmaps.clear()
        if (saving) retired = bytes else bytes?.fill(0)
        bytes = null; selected = null; rows = emptyList(); playableRows = emptyList(); playOrder = emptyList()
        heading = ""; catalogueMessage = ""; playbackMessage = ""; catalogueBusy = false; playbackBusy = false; playing = false
    }
}
