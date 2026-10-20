package org.megavideoprotect.app

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.core.view.WindowCompat

/** Survives leaving the app, as long as the process is still alive. */
private object AppSession {
    var screen: Screen = Screen.Login
    var leftAt: Long = 0L
}

class MainActivity : ComponentActivity() {
    private var screen by mutableStateOf<Screen>(Screen.Login)
    /** Files handed over from another app's Share / Send menu. Held until the vault is open. */
    private var pendingShare by mutableStateOf<List<Uri>>(emptyList())
    /** The lock screen is up, but the vault stays open so a swap can keep going. */
    private var shareHoldsVault = false

    private fun show(next: Screen) {
        // The library and its provider player share the same RAM-only session.
        if (next !is Screen.Vault && next !is Screen.PluginMusic) PluginLibrary.close()
        if (next !is Screen.Plugins && next !is Screen.Vault && next !is Screen.PluginMusic) PluginRuntime.close()
        screen = next
        AppSession.screen = next
    }

    private fun unlocked(): Boolean =
        RemoteVault.connected() || runCatching { CoreBridge.nativeIsUnlocked() }.getOrDefault(false)

    private fun awayTooLong(): Boolean {
        val limit = VaultLocation.idleLockMs(this)
        if (limit <= 0L || AppSession.leftAt == 0L) return false
        return SystemClock.elapsedRealtime() - AppSession.leftAt >= limit
    }

    private fun lockNow() {
        PluginLibrary.close()
        PluginRuntime.close()
        if (MusicHub.active) {
            startService(
                Intent(this, MusicPlaybackService::class.java)
                    .setAction(MusicPlaybackService.ACTION_STOP),
            )
        }
        // A swap outlives the lock screen. It ends when someone disconnects
        // or the PC stops sharing, and the vault locks then.
        if (PeerLink.linked()) {
            shareHoldsVault = true
        } else {
            shareHoldsVault = false
            if (RemoteVault.connected()) RemoteVault.disconnect()
            else runCatching { CoreBridge.nativeLock() }
        }
        GalleryCache.clear()
        show(Screen.Login)
    }

    private fun openMusicFromNotification(source: Intent?) {
        if (source?.getBooleanExtra(EXTRA_OPEN_MUSIC, false) != true) return
        if (!unlocked() || MusicHub.tracks.isEmpty()) return
        val songs = MusicHub.tracks
        val id = MusicHub.anchorId.takeIf { anchor -> songs.any { it.id == anchor } } ?: songs.first().id
        val title = songs.firstOrNull { it.id == id }?.name ?: songs.first().name
        show(Screen.Music(id, title, songs, startFresh = false))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AppProxy.initialize(this)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        // The saved appearance is read once, before the first frame.
        MvpThemeState.loadOnce(this)

        val vaultRoot = VaultLocation.saved(this) ?: filesDir.absolutePath
        val locationOptions = runCatching { VaultLocation.options(this) }
            .getOrDefault(listOf(filesDir.absolutePath))
        val vaultExists = runCatching { CoreBridge.nativeVaultExists(vaultRoot) }.getOrDefault(false)
        val open = unlocked()
        if (open && awayTooLong()) {
            lockNow()
        } else if (open && AppSession.screen !is Screen.Login && AppSession.screen !is Screen.Setup) {
            show(AppSession.screen)
        } else if (vaultExists) {
            show(Screen.Login)
        } else {
            show(Screen.Setup)
        }
        acceptShare(intent)
        openMusicFromNotification(intent)

        setContent {
            MvpTheme {
                // A player, picture, PDF, or song returns to the library. The library
                // walks up one folder, then leaves only after a second press.
                // Login leaves on one press. Setup has nowhere to go back to.
                BackHandler(enabled = screen !is Screen.Setup && screen !is Screen.Vault) {
                    when (screen) {
                        is Screen.Player, is Screen.Image, is Screen.Music, is Screen.PluginMusic, is Screen.Pdf, is Screen.Text, is Screen.Peer, is Screen.Sync, is Screen.Plugins -> show(Screen.Vault)
                        else -> moveTaskToBack(true)
                    }
                }

                LaunchedEffect(PeerLink.pcName) {
                    if (shareHoldsVault && PeerLink.pcName == null && screen is Screen.Login) {
                        shareHoldsVault = false
                        runCatching { CoreBridge.nativeLock() }
                    }
                }
                // The cloud copy of the library waits while anything is open, so
                // a video, a still or a song never shares the link with the sync.
                LaunchedEffect(screen) {
                    CloudFiles.viewerOpen = screen !is Screen.Vault
                }
                var notice by remember { mutableStateOf<String?>(null) }
                LaunchedEffect(notice) {
                    if (notice != null) {
                        delay(2600)
                        notice = null
                    }
                }
                Box(Modifier.fillMaxSize()) {
                    AnimatedContent(
                        targetState = screen,
                        modifier = Modifier.fillMaxSize().clipToBounds(),
                        transitionSpec = {
                            val opening = targetState.depth() > initialState.depth()
                            when {
                                opening && targetState is Screen.Peer ->
                                    (slideInHorizontally(tween(280)) { it / 3 } + fadeIn(tween(200))) togetherWith
                                        (slideOutHorizontally(tween(220)) { -it / 10 } + fadeOut(tween(160)))
                                !opening && initialState is Screen.Peer ->
                                    (slideInHorizontally(tween(240)) { -it / 10 } + fadeIn(tween(200))) togetherWith
                                        (slideOutHorizontally(tween(260)) { it / 3 } + fadeOut(tween(180)))
                                opening && targetState.depth() >= 3 ->
                                    (slideInVertically(tween(300)) { it / 4 } + fadeIn(tween(200))) togetherWith
                                        fadeOut(tween(160))
                                !opening && initialState.depth() >= 3 ->
                                    fadeIn(tween(160)) togetherWith
                                        (slideOutVertically(tween(260)) { it / 4 } + fadeOut(tween(180)))
                                else -> fadeIn(tween(220)) togetherWith fadeOut(tween(160))
                            }
                        },
                        label = "screen",
                    ) { current ->
                    when (current) {
                        is Screen.Setup -> SetupScreen(
                            vaultLocation = vaultRoot,
                            locationOptions = locationOptions,
                            onCreated = { show(Screen.Vault) },
                            onOpenExisting = { show(Screen.Login) },
                        )
                        is Screen.Login -> LoginScreen(
                            vaultLocation = vaultRoot,
                            locationOptions = locationOptions,
                            onUnlocked = {
                                shareHoldsVault = false
                                show(Screen.Vault)
                            },
                            onBack = { show(Screen.Setup) },
                        )
                        is Screen.Vault -> VaultScreen(
                            vaultLocation = RemoteVault.label() ?: vaultRoot,
                            onLock = { lockNow() },
                            onOpenPeer = { show(Screen.Peer) },
                            onOpenSync = { show(Screen.Sync) },
                            onOpenPlugins = { show(Screen.Plugins) },
                            onPlayPlugin = { show(Screen.PluginMusic) },
                            shareUris = pendingShare,
                            onShareHandled = { pendingShare = emptyList() },
                            onOpenMusic = {
                                val songs = MusicHub.tracks
                                if (songs.isNotEmpty()) {
                                    val id = MusicHub.anchorId.takeIf { anchor -> songs.any { it.id == anchor } }
                                        ?: songs.first().id
                                    val title = songs.firstOrNull { it.id == id }?.name ?: songs.first().name
                                    show(Screen.Music(id, title, songs, startFresh = false))
                                }
                            },
                            onPlay = { id, name, playlist ->
                                // A tap opens what was tapped, now. This used to
                                // read the header of every file in the folder
                                // (one vault read each, competing with the
                                // gallery's probes) before the player appeared,
                                // which is what made the first video take a
                                // minute on a fresh vault. The name answers
                                // without touching the vault; only a name that
                                // says nothing at all falls back to a read.
                                val known = FileKinds.peek(id) ?: FileKinds.fromNameOnly(name)
                                when {
                                    FileKinds.isImage(known) -> show(Screen.Image(id, name))
                                    known.route == 4 -> show(Screen.Text(id, name))
                                    known.route == 3 -> show(Screen.Pdf(id, name))
                                    // playlist holds library rows; the screens
                                    // take PlayItem, so the filter maps too.
                                    known.route == 2 -> show(Screen.Music(
                                        id, name,
                                        playlist.filter { FileKinds.looksLikeAudio(it.name) }
                                            .map { PlayItem(it.id, it.name) }))
                                    known.route == 1 -> show(Screen.Player(
                                        id, name,
                                        playlist.filter { FileKinds.looksLikeVideo(it.name) }
                                            .map { PlayItem(it.id, it.name) }))
                                    else -> lifecycleScope.launch {
                                        // No extension and nothing cached: one
                                        // read is unavoidable, and only for the
                                        // item actually tapped.
                                        val sniffed = withContext(Dispatchers.IO) { FileKinds.resolve(id, name) }
                                        when {
                                            FileKinds.isImage(sniffed) -> show(Screen.Image(id, name))
                                            sniffed.route == 4 -> show(Screen.Text(id, name))
                                            sniffed.route == 3 -> show(Screen.Pdf(id, name))
                                            sniffed.route == 2 -> show(Screen.Music(
                                                id, name, listOf(PlayItem(id, name))))
                                            sniffed.route == 1 -> show(Screen.Player(
                                                id, name, listOf(PlayItem(id, name))))
                                            else -> notice = "this format currently not supported"
                                        }
                                    }
                                }
                            },
                        )
                        is Screen.Plugins -> PluginScreen(
                            vaultRoot = VaultLocation.saved(this@MainActivity) ?: vaultRoot,
                            onClose = { show(Screen.Vault) },
                        )
                        is Screen.PluginMusic -> {
                            val session = PluginLibrary.session
                            if (session == null) {
                                LaunchedEffect(Unit) { show(Screen.Vault) }
                            } else MusicScreen(
                                videoId = 0L,
                                name = session.selected?.title ?: session.manifest.name,
                                playlist = emptyList(),
                                startFresh = false,
                                onBack = { show(Screen.Vault) },
                                pluginSession = session,
                            )
                        }
                        is Screen.Sync -> SyncScreen(
                            vaultRoot = VaultLocation.saved(this@MainActivity) ?: vaultRoot,
                            onClose = { show(Screen.Vault) },
                        )
                        is Screen.Peer -> PeerScreen(onClose = { show(Screen.Vault) })
                        is Screen.Player -> PlayerScreen(
                            videoId = current.videoId,
                            name = current.name,
                            playlist = current.playlist,
                            onBack = { show(Screen.Vault) },
                        )
                        is Screen.Image -> ImageViewerScreen(
                            videoId = current.videoId,
                            name = current.name,
                            onBack = { show(Screen.Vault) },
                        )
                        is Screen.Pdf -> PdfScreen(
                            videoId = current.videoId,
                            name = current.name,
                            onBack = { show(Screen.Vault) },
                        )
                        is Screen.Text -> TextScreen(
                            videoId = current.videoId,
                            name = current.name,
                            onBack = { show(Screen.Vault) },
                        )
                        is Screen.Music -> MusicScreen(
                            videoId = current.videoId,
                            name = current.name,
                            playlist = current.playlist,
                            startFresh = current.startFresh,
                            onBack = { show(Screen.Vault) },
                        )
                    }
                    }
                    AnimatedVisibility(
                        visible = notice != null,
                        enter = fadeIn(tween(180)),
                        exit = fadeOut(tween(420)),
                        modifier = Modifier.align(Alignment.BottomCenter),
                    ) {
                        Text(
                            notice.orEmpty(),
                            color = Color.White,
                            fontSize = 13.sp,
                            modifier = Modifier
                                .padding(bottom = 72.dp)
                                .background(Mvp.destructive, RoundedCornerShape(8.dp))
                                .padding(horizontal = 14.dp, vertical = 8.dp),
                        )
                    }
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        acceptShare(intent)
        openMusicFromNotification(intent)
    }

    /** Pulls the files out of a Share / Send intent. Ignored for a normal launch. */
    private fun acceptShare(intent: Intent?) {
        val uris = intent?.sharedStreams().orEmpty()
        if (uris.isEmpty()) return
        pendingShare = uris
        val inside = screen is Screen.Vault || screen is Screen.Player
            || screen is Screen.Image || screen is Screen.Pdf || screen is Screen.Text
            || screen is Screen.Music || screen is Screen.PluginMusic || screen is Screen.Peer || screen is Screen.Sync || screen is Screen.Plugins
        if (inside && unlocked()) show(Screen.Vault)
    }

    override fun onStart() {
        super.onStart()
        val inside = screen is Screen.Vault || screen is Screen.Player
            || screen is Screen.Image || screen is Screen.Pdf || screen is Screen.Text
            || screen is Screen.Music || screen is Screen.PluginMusic || screen is Screen.Peer || screen is Screen.Sync || screen is Screen.Plugins
        if (inside && awayTooLong()) lockNow()
        AppSession.leftAt = 0L
    }

    override fun onStop() {
        AppSession.leftAt = SystemClock.elapsedRealtime()
        super.onStop()
    }
}

/** Library sits under a side-sliding peer browser and under viewers that rise from the bottom. */
private fun Screen.depth(): Int = when (this) {
    Screen.Setup, Screen.Login -> 0
    Screen.Vault -> 1
    Screen.Peer, Screen.Sync, Screen.Plugins -> 2
    else -> 3
}

/** Content URIs attached to ACTION_SEND or ACTION_SEND_MULTIPLE. */
private fun Intent.sharedStreams(): List<Uri> {
    if (action != Intent.ACTION_SEND && action != Intent.ACTION_SEND_MULTIPLE) return emptyList()
    val found = LinkedHashSet<Uri>()
    fun add(uri: Uri?) {
        if (uri != null) found.add(uri)
    }
    if (Build.VERSION.SDK_INT >= 33) {
        add(getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java))
        getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)?.forEach { add(it) }
    } else {
        @Suppress("DEPRECATION")
        add(getParcelableExtra(Intent.EXTRA_STREAM))
        @Suppress("DEPRECATION")
        getParcelableArrayListExtra<Uri>(Intent.EXTRA_STREAM)?.forEach { add(it) }
    }
    val clip = clipData
    if (clip != null) {
        for (index in 0 until clip.itemCount) add(clip.getItemAt(index).uri)
    }
    return found.toList()
}
