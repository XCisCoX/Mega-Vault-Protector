package org.megavideoprotect.app

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.WindowManager
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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
    // Retained with the native/remote session across activity recreation, not process death.
    var vaultIdentity by mutableStateOf(VaultIdentity())
}

class MainActivity : ComponentActivity() {
    private var screen by mutableStateOf<Screen>(Screen.Login)
    /** Files handed over from another app's Share / Send menu. Held until the vault is open. */
    private var pendingShare by mutableStateOf<List<Uri>>(emptyList())
    /** The lock screen is up, but the vault stays open so a swap can keep going. */
    private var shareHoldsVault = false

    private fun openedLocalVault(root: String) {
        AppSession.vaultIdentity = AppSession.vaultIdentity.openedLocal(root)
        VaultLocation.save(this, root)
        shareHoldsVault = false
        GalleryCache.clear()
        show(Screen.Vault)
        SyncV4Auto.attach(this, root)
    }

    private fun openedRemoteVault() {
        SyncV4Auto.detach()
        AppSession.vaultIdentity = AppSession.vaultIdentity.connectedRemote()
        shareHoldsVault = false
        show(Screen.Vault)
    }

    private fun show(next: Screen) {
        // The library and its provider player share the same RAM-only session.
        if (next !is Screen.Vault && next !is Screen.PluginMusic) PluginLibrary.close()
        if (next !is Screen.Plugins && next !is Screen.Vault && next !is Screen.PluginMusic) PluginRuntime.close()
        screen = next
        AppSession.screen = next
    }

    override fun onResume() {
        super.onResume()
        // Coming back to a vault that is already open is the best moment to
        // catch up: no pass was running while the phone was in someone's pocket.
        if (unlocked() && !RemoteVault.connected()) SyncV4Auto.syncNow()
    }

    private fun unlocked(): Boolean =
        RemoteVault.connected() || runCatching { CoreBridge.nativeIsUnlocked() }.getOrDefault(false)

    private fun awayTooLong(): Boolean {
        val limit = VaultLocation.idleLockMs(this)
        if (limit <= 0L || AppSession.leftAt == 0L) return false
        return SystemClock.elapsedRealtime() - AppSession.leftAt >= limit
    }

    /** Set when the user must choose between a hard lock and a screen-only lock. */
    private var lockChoice by mutableStateOf(false)

    /**
     * The Lock button. When a PC is swapped in, the user has to say whether the
     * vault really locks (sharing ends) or only the screen hides (the keys stay
     * in memory and the PC keeps reading). The choice is never made silently.
     */
    private fun requestLock() {
        if (PeerLink.linked() && unlocked()) {
            lockChoice = true
            return
        }
        lockNow(keepSharing = false)
    }

    private fun lockNow(keepSharing: Boolean = false) {
        SyncV4Auto.detach()
        PluginLibrary.close()
        PluginRuntime.close()
        if (MusicHub.active) {
            startService(
                Intent(this, MusicPlaybackService::class.java)
                    .setAction(MusicPlaybackService.ACTION_STOP),
            )
        }
        if (keepSharing && PeerLink.linked()) {
            // Deliberate, user-chosen exception: the swap keeps its vault so the
            // PC can still read it. The login screen says so.
            shareHoldsVault = true
        } else {
            shareHoldsVault = false
            // Dropping the PC link is part of the lock: its session handle was a
            // second way into this vault.
            runCatching { PeerLink.unlink() }
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
        // Decrypted filenames, thumbnails and video frames must not reach the
        // screenshot pipeline or the Recents thumbnail. This is the platform
        // mechanism for that; it protects the preview, not process memory.
        window.setFlags(
            WindowManager.LayoutParams.FLAG_SECURE,
            WindowManager.LayoutParams.FLAG_SECURE,
        )
        // The saved appearance is read once, before the first frame.
        MvpThemeState.loadOnce(this)

        // Only a locked-screen picker hint. Never use preferences to identify an open vault.
        val initialLocation = VaultLocation.saved(this) ?: filesDir.absolutePath
        val locationOptions = runCatching { VaultLocation.options(this) }
            .getOrDefault(listOf(filesDir.absolutePath))
        val vaultExists = runCatching { CoreBridge.nativeVaultExists(initialLocation) }.getOrDefault(false)
        val open = unlocked()
        if (open && (awayTooLong() || (!RemoteVault.connected() && AppSession.vaultIdentity.activeLocalRoot == null))) {
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
        // A recreated activity reattaches to the actual retained local session.
        if (unlocked() && !RemoteVault.connected() && screen !is Screen.Login && screen !is Screen.Setup) {
            AppSession.vaultIdentity.activeLocalRoot?.let { root -> SyncV4Auto.attach(this, root) }
        }

        setContent {
            MvpTheme {
                // A player, picture, PDF, or song returns to the library. The library
                // walks up one folder, then leaves only after a second press.
                // Login leaves on one press. Setup has nowhere to go back to.
                if (lockChoice) {
                    AlertDialog(
                        onDismissRequest = { lockChoice = false },
                        title = { Text("Lock the vault?") },
                        text = {
                            Text(
                                "Choosing “keep sharing” only hides the screen: the vault stays " +
                                    "unlocked in memory and the PC can still read and copy it.",
                            )
                        },
                        confirmButton = {
                            TextButton(onClick = {
                                lockChoice = false
                                lockNow(keepSharing = false)
                            }) { Text("Lock and stop sharing") }
                        },
                        dismissButton = {
                            TextButton(onClick = {
                                lockChoice = false
                                lockNow(keepSharing = true)
                            }) { Text("Hide screen, keep sharing") }
                        },
                    )
                }
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
                            vaultLocation = initialLocation,
                            locationOptions = locationOptions,
                            onCreated = { root -> openedLocalVault(root) },
                            onRemoteConnected = { openedRemoteVault() },
                            onOpenExisting = { show(Screen.Login) },
                        )
                        is Screen.Login -> LoginScreen(
                            vaultLocation = initialLocation,
                            locationOptions = locationOptions,
                            onUnlocked = { root -> openedLocalVault(root) },
                            onRemoteConnected = { openedRemoteVault() },
                            onBack = { show(Screen.Setup) },
                            privacyScreen = shareHoldsVault && unlocked(),
                            onLockFully = { lockNow(keepSharing = false) },
                        )
                        is Screen.Vault -> {
                            // Outgoing animated content may recompose after a remote disconnect.
                            val location = if (RemoteVault.connected()) RemoteVault.label() ?: "PC vault"
                                else AppSession.vaultIdentity.activeLocalRoot
                            if (location != null) VaultScreen(
                            vaultLocation = location,
                            onLock = { requestLock() },
                            onOpenPeer = { show(Screen.Peer) },
                            onOpenSync = { if (!RemoteVault.connected()) show(Screen.Sync) },
                            onOpenPlugins = { if (!RemoteVault.connected()) show(Screen.Plugins) },
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
                        }
                        is Screen.Plugins -> {
                            val root = AppSession.vaultIdentity.activeLocalRoot
                            if (!RemoteVault.connected() && root != null) PluginScreen(
                                vaultRoot = root,
                                onClose = { show(Screen.Vault) },
                            ) else LaunchedEffect(Unit) { show(Screen.Vault) }
                        }
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
                        is Screen.Sync -> {
                            val root = AppSession.vaultIdentity.activeLocalRoot
                            if (!RemoteVault.connected() && root != null) SyncScreen(
                                vaultRoot = root,
                                onClose = { show(Screen.Vault) },
                            ) else LaunchedEffect(Unit) { show(Screen.Vault) }
                        }
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
