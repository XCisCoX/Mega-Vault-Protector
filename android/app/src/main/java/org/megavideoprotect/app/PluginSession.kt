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
import androidx.media3.exoplayer.DefaultLoadControl
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
    @Volatile private var generation = 0L
    private var catalogueGeneration = 0L
    @Volatile private var closed = false
    private var loading: Job? = null
    private var catalogueLoading: Job? = null
    private var saving = false
    private val autoSaveAttempted = mutableSetOf<String>()
    private var importSink: ProviderImport? = null
    private var remoteReady = false
    var localPlayback by mutableStateOf(false); private set
    var downloadAllowed by mutableStateOf(false); private set
    var clientId by mutableStateOf(""); private set
    fun updateClientId(value: String) { clientId = value.take(256) }
    private fun active(token: Long): Boolean = !closed && token == generation && CoreBridge.nativeIsUnlocked() && !RemoteVault.connected()
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
    private val downloadJobs = mutableMapOf<String, Job>()
    private val downloadClients = ConcurrentHashMap<String, MutableSet<PluginHttp>>()
    val canDownload: Boolean get() = !closed && !playbackBusy && !saveBusy && !saved && selected != null && remoteReady && downloadAllowed
    val player = ExoPlayer.Builder(context).setLoadControl(DefaultLoadControl.Builder()
        .setTargetBufferBytes(8 * 1024 * 1024).setPrioritizeTimeOverSizeThresholds(false).build()).build().apply {
        setAudioAttributes(AudioAttributes.Builder().setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(), true)
        setHandleAudioBecomingNoisy(true)
        addListener(object : Player.Listener {
            override fun onIsPlayingChanged(isPlaying: Boolean) {
                playing = isPlaying
                // Readiness or track selection is NOT playback.
                if (isPlaying && options.autoSave && !saved && !saving && canDownload && selected?.id !in autoSaveAttempted) {
                    selected?.id?.let { autoSaveAttempted.add(it) }; download()
                }
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
        if (manifest.protocol == "soundcloud-tracks-v1" && clientId.isBlank()) {
            catalogueMessage = "Enter your own SoundCloud API client ID below, then Retry. No API identity is bundled or scraped."
            return
        }
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
                val data = network(catalogueRequests) {
                    it.jsonUrl(if (manifest.protocol == "soundcloud-tracks-v1") SoundCloudTracksProvider.withClientId(it.absolute(path), clientId) else path)
                }
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
    private fun loadTrack(item: PluginItem, store: Boolean = false) {
        if (closed) return
        val token = ++generation
        loading?.cancel(); playbackRequests.forEach { it.close() }; importSink?.close(); importSink = null
        player.stop(); player.clearMediaItems(); remoteReady = false
        selected = item; saved = false; localPlayback = false; downloadAllowed = false
        saving = store; saveBusy = store; playbackBusy = true; playbackMessage = ""
        val identity = clientId
        loading = scope.launch {
            try {
                // A persisted identity lookup happens BEFORE any API/CDN request, including rights resolution.
                val found = withContext(Dispatchers.IO) { CoreBridge.Result.parse(CoreBridge.nativeFindProviderTrack(manifest.id, item.id, vaultRoot)) }
                check(found.ok) { "Vault changed or locked" }
                ensureActive()
                if (!active(token)) return@launch
                if (MusicHub.active) context.startService(Intent(context, MusicPlaybackService::class.java).setAction(MusicPlaybackService.ACTION_STOP))
                val media = MediaItem.Builder().setMediaId(item.id)
                    .setMediaMetadata(MediaMetadata.Builder().setTitle(item.title).setArtist(item.subtitle).build())
                if (found.id > 0) {
                    localPlayback = true; saved = true; saving = false; saveBusy = false
                    player.setMediaSource(ProgressiveMediaSource.Factory(VaultDataSource.Factory())
                        .createMediaSource(media.setUri(VaultDataSource.uriFor(found.id)).build()))
                    playbackMessage = "Offline · playing the encrypted vault copy."
                } else {
                    val plan = network(playbackRequests) { http -> ProviderHls.plan(http, provider.resolve(http, item, identity, store), measureParts = store) }
                    ensureActive()
                    if (!active(token)) return@launch
                    downloadAllowed = plan.stream.downloadable
                    val sink = if (store) newImport(item, plan.stream, token) else null
                    importSink = sink
                    val factory = DataSource.Factory { ProviderDataSource(manifest, plan, sink, { active(token) }) { client, add ->
                        if (add) { requests.add(client); playbackRequests.add(client); if (!active(token)) client.close() }
                        else { requests.remove(client); playbackRequests.remove(client) }
                    } }
                    player.setMediaSource(ProgressiveMediaSource.Factory(factory)
                        .createMediaSource(media.setUri(Uri.parse("provider://track/${item.id}")).build()))
                    remoteReady = true
                    if (!downloadAllowed) playbackMessage = "Streaming only · the artist has not authorized offline storage."
                }
                player.prepare(); player.play()
            } catch (_: CancellationException) { }
            catch (e: Exception) {
                if (token == generation && !closed) {
                    playbackMessage = if (e is IllegalArgumentException || e is IllegalStateException) e.message.orEmpty().take(240) else "Provider audio failed. Check API access and connection."
                    saving = false; saveBusy = false
                }
            } finally { if (token == generation && !closed) playbackBusy = false }
        }
    }
    private fun newImport(item: PluginItem, stream: ProviderStream, token: Long): ProviderImport =
        ProviderImport(manifest, item, vaultRoot, options.folderId, stream) { ok, text ->
            scope.launch {
                if (!closed && token == generation) {
                    saving = false; saveBusy = false; saved = ok; playbackMessage = text
                    if (ok) { GalleryCache.clear(); SyncV4Auto.markDirty() }
                }
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
        // One progressive or local track in ExoPlayer; list repeat is implemented by advance().
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
        else if (player.playerError != null || (!remoteReady && !localPlayback)) selected?.let { loadTrack(it) }
        else { if (player.playbackState == Player.STATE_ENDED) player.seekTo(0L); player.play() }
    }
    /** Two bounded sequential import workers; they never buffer an entire track. */
    fun downloadTrack(item: PluginItem) {
        if (closed || item.kind != "track" || !item.playable) return
        if (downloadStates[item.id] in setOf("downloading", "saving")) {
            downloadClients[item.id]?.forEach { it.close() }; downloadJobs[item.id]?.cancel()
            return
        }
        if (downloadStates[item.id] == "saved") return
        val group = ConcurrentHashMap.newKeySet<PluginHttp>()
        downloadClients[item.id] = group
        setDownload(item.id, "downloading")
        downloadJobs[item.id] = scope.launch {
            try {
                downloadPermits.withPermit {
                    withContext(Dispatchers.IO) {
                        val found = CoreBridge.Result.parse(CoreBridge.nativeFindProviderTrack(manifest.id, item.id, vaultRoot))
                        check(found.ok)
                        if (found.id > 0) {
                            withContext(Dispatchers.Main) { if (!closed) setDownload(item.id, "saved") }
                            return@withContext
                        }
                        val plan = network(group) { http -> ProviderHls.plan(http, provider.resolve(http, item, clientId, true), measureParts = true) }
                        var succeeded = false
                        var outcome = "Offline copy failed"
                        val sink = ProviderImport(manifest, item, vaultRoot, options.folderId, plan.stream) { ok, text -> succeeded = ok; outcome = text }
                        val job = currentCoroutineContext()[Job]
                        val source = ProviderDataSource(manifest, plan, sink,
                            { !closed && job?.isActive != false && CoreBridge.nativeIsUnlocked() && !RemoteVault.connected() }) { client, add ->
                            if (add) { requests.add(client); group.add(client); if (closed || job?.isActive == false) client.close() }
                            else { requests.remove(client); group.remove(client) }
                        }
                        val chunk = ByteArray(65536)
                        try {
                            source.open(androidx.media3.datasource.DataSpec(Uri.parse("provider://download/${item.id}")))
                            while (true) {
                                ensureActive()
                                if (source.read(chunk, 0, chunk.size) < 0) break
                                chunk.fill(0)
                            }
                        } finally { chunk.fill(0); source.close(); sink.close() }
                        withContext(Dispatchers.Main) {
                            if (!closed) {
                                setDownload(item.id, if (succeeded) "saved" else outcome.take(120))
                                if (succeeded) { GalleryCache.clear(); SyncV4Auto.markDirty() }
                            }
                        }
                    }
                }
            } catch (_: CancellationException) { if (!closed) setDownload(item.id, "") }
            catch (_: Exception) { if (!closed) setDownload(item.id, "Download failed or not authorized") }
            finally { downloadClients.remove(item.id, group); downloadJobs.remove(item.id) }
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
        importSink?.close(); importSink = null; saving = false; saveBusy = false
        remoteReady = false; localPlayback = false; selected = null; saved = false
        playableRows = emptyList(); playOrder = emptyList()
        playbackBusy = false; playbackMessage = ""
    }
    fun download() {
        val track = selected ?: return
        if (!canDownload) return
        // No retained plaintext history: re-open from byte zero, then tee those SAME
        // incoming authorized bytes to playback and the encrypted core writer.
        loadTrack(track, store = true)
    }
    fun cancelDownload() {
        if (saving && importSink == null) {
            // Cancellation during URL/rights resolution must prevent a later sink.
            generation++; loading?.cancel(); playbackRequests.forEach { it.close() }
            playbackBusy = false; remoteReady = false
        }
        importSink?.close(); importSink = null; saving = false; saveBusy = false
        playbackMessage = "Offline copy cancelled; streaming can continue."
    }
    fun close() {
        if (closed) return
        closed = true; generation++; catalogueGeneration++
        requests.forEach { it.close() }; requests.clear(); scope.cancel()
        player.stop(); player.clearMediaItems(); player.release()
        artworkBitmaps.forEach { runCatching { it.eraseColor(android.graphics.Color.TRANSPARENT) } }
        artworkBitmaps.clear()
        importSink?.close(); importSink = null; saving = false; saveBusy = false; clientId = ""
        remoteReady = false; selected = null; rows = emptyList(); playableRows = emptyList(); playOrder = emptyList()
        heading = ""; catalogueMessage = ""; playbackMessage = ""; catalogueBusy = false; playbackBusy = false; playing = false
    }
}
