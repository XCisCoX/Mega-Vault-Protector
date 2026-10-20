package org.megavideoprotect.app

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Semaphore

/**
 * Process-only state survives visiting the existing player, but is wiped on lock.
 * Root shows exactly two folders: Songs and Albums.
 */
object PluginLibrary {
    var session by mutableStateOf<PluginSession?>(null); private set
    var folderOpen by mutableStateOf(false); private set
    var revision by mutableIntStateOf(0); private set
    private val trail = mutableStateListOf<PluginLibraryTarget>()
    val target: PluginLibraryTarget? get() = trail.lastOrNull()
    val breadcrumb: String get() = trail.joinToString(" / ") { it.title }

    fun open(context: Context, manifest: PluginManifest, root: String, options: PluginOptions, query: String) {
        require(options.enabled && query.isNotBlank())
        close()
        session = PluginSession(context.applicationContext, manifest, root, options)
        trail.add(PluginLibraryTarget(query.trim().take(240)))
        folderOpen = true
        reload()
    }
    /** An album opens as a folder of its tracks, exactly like a vault folder. */
    fun openAlbum(item: PluginItem) {
        require(item.kind == "album")
        val current = target ?: return
        trail.add(PluginLibraryTarget(current.query, item = item))
        reload()
    }
    fun back() {
        if (trail.size <= 1) { folderOpen = false; return }
        val leaving = trail.removeAt(trail.lastIndex)
        val current = target ?: return
        if (leaving.item == current.item && leaving.query == current.query) revision++
        else reload()
    }
    fun leave() { folderOpen = false }
    fun reload() {
        val current = target ?: return
        revision++
        session?.browse(query = current.query, item = current.item)
    }
    fun close() {
        session?.close()
        session = null
        folderOpen = false
        trail.clear()
        revision++
    }
}

@Composable
internal fun PluginSearchEntry(manifest: PluginManifest, query: String, viewMode: Int, onOpen: () -> Unit) {
    val title = "Search ${manifest.name} for “${query.trim().take(240)}”"
    when (viewMode) {
        0 -> FolderDetailRow(title, "Online provider", onOpen, null, { false })
        1 -> FolderTile(title, onOpen, null, { false }, place = "Online provider")
        else -> FolderListRow(title, "Online provider", onOpen, null, { false })
    }
}

/** Only the gallery body changes: VaultScreen keeps its header, footer and search. */

/** The folder's back control: a real chevron asset, not a text glyph. */
@Composable
private fun PluginBackRow(label: String, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            painter = painterResource(R.drawable.ic_lucide_arrow_left),
            contentDescription = null,
            tint = Mvp.accent,
            modifier = Modifier.size(18.dp),
        )
        Text(label, color = Mvp.accent, fontSize = 14.sp, modifier = Modifier.padding(start = 6.dp))
    }
}

@Composable
internal fun PluginLibraryView(viewMode: Int, padding: PaddingValues, onPlay: (PluginSession) -> Unit) {
    val session = PluginLibrary.session ?: return
    val target = PluginLibrary.target ?: return
    val revision = PluginLibrary.revision
    val images = remember(session) { Semaphore(2) }
    var deadline by remember(session, revision) { mutableStateOf(false) }
    // An independent surface deadline prevents an unstarted request leaving Waiting forever.
    LaunchedEffect(session, revision) {
        delay(15_000)
        if (session.catalogueBusy) deadline = true
    }
    // One mixed list: songs are rows, albums are folders holding their songs.
    val rows = target.visibleRows(session.rows)
    val inAlbum = target.item != null
    val status = when {
        deadline -> "Provider did not answer within 15 seconds. Tap Retry."
        session.catalogueBusy -> "Searching ${session.manifest.name}…"
        session.message.isNotBlank() && rows.isEmpty() -> session.message
        rows.isEmpty() -> "Nothing here"
        inAlbum -> "${rows.size} songs in this album"
        else -> {
            val albumCount = rows.count { it.kind == "album" }
            val songCount = rows.count { it.kind == "track" }
            "$songCount songs" + if (albumCount > 0) " · $albumCount albums" else ""
        }
    }
    val openAlbum: (PluginItem) -> Unit = { PluginLibrary.openAlbum(it) }
    val playTrack: (PluginItem) -> Unit = { item ->
        session.play(item, rows.filter { it.kind == "track" && it.playable })
        onPlay(session)
    }

    if (viewMode == 1) {
        LazyVerticalGrid(
            columns = GridCells.Fixed(3), contentPadding = padding,
            modifier = Modifier.fillMaxSize().background(Mvp.window),
        ) {
            item(key = "provider-back") { PluginBackRow("Back", { PluginLibrary.back() }) }
            item(key = "provider-state", span = { GridItemSpan(3) }) {
                ProviderStatus(status, session.catalogueBusy && !deadline, onRetry = { PluginLibrary.reload() }, showRetry = !session.catalogueBusy || deadline)
            }
            items(rows, key = { "${it.kind}:${it.id}" }) { item ->
                if (item.kind == "album") {
                    FolderTile(item.title, { openAlbum(item) }, null, { false }, place = item.subtitle.ifBlank { "Album" })
                } else {
                    ProviderTrackTile(session, item, images, onPlay = { playTrack(item) }, onDownload = { session.downloadTrack(item) })
                }
            }
        }
    } else {
        LazyColumn(contentPadding = padding, modifier = Modifier.fillMaxSize().background(Mvp.window)) {
            item(key = "provider-back") { PluginBackRow("Back", { PluginLibrary.back() }) }
            item(key = "provider-state") {
                ProviderStatus(status, session.catalogueBusy && !deadline, onRetry = { PluginLibrary.reload() }, showRetry = !session.catalogueBusy || deadline)
            }
            items(rows, key = { "${it.kind}:${it.id}" }) { item ->
                if (item.kind == "album") {
                    if (viewMode == 0) FolderDetailRow(item.title, item.subtitle.ifBlank { "Album" }, { openAlbum(item) }, null, { false })
                    else FolderListRow(item.title, item.subtitle.ifBlank { "Album" }, { openAlbum(item) }, null, { false })
                } else {
                    ProviderTrackRow(session, item, images, viewMode, onPlay = { playTrack(item) }, onDownload = { session.downloadTrack(item) })
                }
            }
        }
    }
}

@Composable
private fun ProviderStatus(message: String, loading: Boolean, onRetry: () -> Unit, showRetry: Boolean) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp)) {
        if (loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(message, color = Mvp.description, fontSize = 12.sp, modifier = Modifier.weight(1f))
            if (showRetry) TextButton(onClick = onRetry) { Text("Retry") }
        }
    }
}

/** The download affordance on a song row: a button that becomes a live state. */
@Composable
private fun DownloadControl(state: String?, onDownload: () -> Unit) {
    when (state) {
        "downloading", "saving" -> Row(verticalAlignment = Alignment.CenterVertically) {
            CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp, color = Mvp.accent)
            Text(
                if (state == "saving") " Saving…" else " Downloading…",
                color = Mvp.description, fontSize = 12.sp, maxLines = 1,
            )
        }
        "saved" -> Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.padding(horizontal = 10.dp),
        ) {
            Icon(
                painter = painterResource(R.drawable.ic_lucide_check),
                contentDescription = null, tint = Mvp.description, modifier = Modifier.size(14.dp),
            )
            Text("Saved", color = Mvp.description, fontSize = 12.sp, modifier = Modifier.padding(start = 5.dp))
        }
        null, "" -> Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.clickable(onClick = onDownload)
                .padding(horizontal = 10.dp, vertical = 6.dp),
        ) {
            Icon(
                painter = painterResource(R.drawable.ic_lucide_download),
                contentDescription = null, tint = Mvp.accent, modifier = Modifier.size(15.dp),
            )
            Text(
                "Download",
                color = Mvp.accent,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.padding(start = 5.dp),
            )
        }
        else -> Row(verticalAlignment = Alignment.CenterVertically) {
            Text(state, color = Mvp.errorText, fontSize = 11.sp, maxLines = 1, modifier = Modifier.weight(1f, fill = false))
            Text(
                "Retry",
                color = Mvp.accent,
                fontSize = 12.sp,
                modifier = Modifier.clickable(onClick = onDownload).padding(horizontal = 8.dp),
            )
        }
    }
}

@Composable
private fun songMeta(item: PluginItem): String {
    val kind = if (item.playable) "" else " · unavailable"
    return (item.subtitle.ifBlank { "Song" } + kind)
}

@Composable
private fun ProviderTrackRow(
    session: PluginSession,
    item: PluginItem,
    images: Semaphore,
    viewMode: Int,
    onPlay: () -> Unit,
    onDownload: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().clickable(enabled = item.playable, onClick = onPlay)
            .padding(start = 12.dp, end = 6.dp, top = if (viewMode == 0) 10.dp else 6.dp,
                bottom = if (viewMode == 0) 10.dp else 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PluginArtwork(session, item.artwork, images)
        Column(Modifier.weight(1f).padding(start = 10.dp)) {
            Text(item.title, color = Mvp.text, fontSize = if (viewMode == 0) 15.sp else 14.sp,
                fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(songMeta(item), color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        DownloadControl(session.downloadStates[item.id], onDownload)
    }
}

@Composable
private fun ProviderTrackTile(
    session: PluginSession,
    item: PluginItem,
    images: Semaphore,
    onPlay: () -> Unit,
    onDownload: () -> Unit,
) {
    Column(
        Modifier.fillMaxWidth().aspectRatio(1f).padding(4.dp)
            .background(Mvp.card).clickable(enabled = item.playable, onClick = onPlay).padding(6.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        PluginArtwork(session, item.artwork, images)
        Text(item.title, color = Mvp.text, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
        DownloadControl(session.downloadStates[item.id], onDownload)
    }
}

@Composable
internal fun PluginLibraryBar() {
    val session = PluginLibrary.session ?: return
    Row(Modifier.fillMaxWidth().background(Mvp.inputBg).padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        TextButton(onClick = { PluginLibrary.leave() }) {
            Icon(
                painter = painterResource(R.drawable.ic_lucide_arrow_left),
                contentDescription = null, tint = Mvp.accent, modifier = Modifier.size(16.dp),
            )
            Text("Back to library", color = Mvp.accent, modifier = Modifier.padding(start = 5.dp))
        }
        Text("${session.manifest.name} / ${PluginLibrary.breadcrumb}", color = Mvp.text,
            fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
    }
}
