@file:OptIn(androidx.media3.common.util.UnstableApi::class)

package org.megavideoprotect.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.runtime.*
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.PlaybackException
import androidx.media3.datasource.ByteArrayDataSource
import androidx.media3.datasource.DataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.ProgressiveMediaSource
import kotlinx.coroutines.*
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
    private val artworkBitmaps = ConcurrentHashMap.newKeySet<android.graphics.Bitmap>()
    private var generation = 0L
    @Volatile private var closed = false
    private var loading: Job? = null
    private var saving = false
    private var bytes: ByteArray? = null
    private var retired: ByteArray? = null
    var rows by mutableStateOf<List<PluginItem>>(emptyList()); private set
    var heading by mutableStateOf("Search songs, artists or albums"); private set
    var message by mutableStateOf(""); private set
    var busy by mutableStateOf(false); private set
    var selected by mutableStateOf<PluginItem?>(null); private set
    var playing by mutableStateOf(false); private set
    var saved by mutableStateOf(false); private set
    var saveBusy by mutableStateOf(false); private set
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
                message = "Audio playback failed. No auto-save was triggered before playback."
            }
        })
    }
    init { require(options.enabled); PluginRuntime.close(); PluginRuntime.session = this }

    private suspend fun <T> network(block: (PluginHttp) -> T): T {
        val client = PluginHttp(manifest)
        requests.add(client)
        try { return withContext(Dispatchers.IO) { block(client) } }
        finally { client.close(); requests.remove(client) }
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
    fun browse(query: String? = null, item: PluginItem? = null) {
        loading?.cancel(); requests.forEach { it.close() }
        val token = ++generation
        loading = scope.launch {
            busy = true; message = ""
            try {
                val path = provider.cataloguePath(query,item)
                val data = network { it.json(path) }
                ensureActive()
                if (token == generation && !closed) {
                    rows = provider.normalize(data, item?.kind)
                    heading = item?.title ?: "Results · ${manifest.name}"
                    if (rows.isEmpty()) message = "No results"
                }
            } catch (_: CancellationException) { }
            catch (_: Exception) { if (token == generation) message = "Catalogue request failed. Check the connection or try another search." }
            finally { if (token == generation) busy = false }
        }
    }
    fun play(item: PluginItem) {
        if (saving) { message = "Wait for the encrypted import to finish before changing tracks."; return }
        loading?.cancel(); requests.forEach { it.close() }
        player.stop(); player.clearMediaItems(); bytes?.fill(0); bytes = null
        selected = null; saved = false
        val token = ++generation
        loading = scope.launch {
            busy = true; message = ""
            var loaded: ByteArray? = null
            try {
                // Keep ownership even if withContext's prompt cancellation discards its result.
                network { loaded = it.audio(provider.audioPath(item), options.memoryMiB * 1024 * 1024) }
                ensureActive()
                if (token != generation || closed) return@launch
                if (MusicHub.active) context.startService(Intent(context, MusicPlaybackService::class.java).setAction(MusicPlaybackService.ACTION_STOP))
                val audio = checkNotNull(loaded)
                bytes = audio; loaded = null; selected = item
                val source = ProgressiveMediaSource.Factory(DataSource.Factory { ByteArrayDataSource(audio) })
                    .createMediaSource(MediaItem.Builder().setUri(Uri.parse("memory://plugin/${item.id}")).setMimeType("audio/flac").build())
                player.setMediaSource(source); player.prepare(); player.play()
            } catch (_: CancellationException) { }
            catch (e: PluginLimitException) { message = e.message.orEmpty() }
            catch (_: Exception) { message = "Audio could not be loaded or was not verified FLAC. Nothing was cached or saved." }
            finally { loaded?.fill(0); if (token == generation) busy = false }
        }
    }
    fun toggle() { if (player.isPlaying) player.pause() else player.play() }
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
                    message = if (result.ok) "Saved to encrypted vault (existing content is deduplicated)" else "Vault import failed: ${result.error}"
                    if (result.ok) GalleryCache.clear()
                }
            } catch (_: Exception) { if (!closed) message = "Cannot import: vault or destination changed, or encrypted import failed." }
            finally {
                saving = false; saveBusy = false
                retired?.fill(0); retired = null
            }
        }
    }
    fun close() {
        if (closed) return
        closed = true; generation++
        requests.forEach { it.close() }; requests.clear(); scope.cancel()
        player.stop(); player.clearMediaItems(); player.release()
        artworkBitmaps.forEach { runCatching { it.eraseColor(android.graphics.Color.TRANSPARENT) } }
        artworkBitmaps.clear()
        if (saving) retired = bytes else bytes?.fill(0)
        bytes = null; selected = null; rows = emptyList(); heading = ""; message = ""; playing = false
    }
}
