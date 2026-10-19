package org.megavideoprotect.app

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

enum class TransferState { Waiting, Copying, Done, Failed, Canceled }

data class TransferItem(
    val id: Long,
    val title: String,
    val detail: String,
    val state: TransferState,
    val progress: Float,
    val label: String,
    val note: String = "",
)

private class TransferWork(
    val id: Long,
    val stop: AtomicBoolean,
    val run: suspend TransferWork.() -> Unit,
)

/**
 * Copies keep running after you leave the PC's library. Another copy waits
 * in line. The vault still writes one file at a time, so nothing collides.
 */
object TransferHub {
    var items by mutableStateOf<List<TransferItem>>(emptyList())
        private set

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val inbox = Channel<TransferWork>(Channel.UNLIMITED)
    private val nextId = AtomicLong(1L)
    private val stops = HashMap<Long, AtomicBoolean>()

    init {
        scope.launch {
            for (work in inbox) work.run(work)
        }
    }

    val headline: String?
        get() {
            val running = items.firstOrNull { it.state == TransferState.Copying }
            val waiting = items.count { it.state == TransferState.Waiting }
            if (running != null) {
                val pct = (running.progress * 100f).toInt().coerceIn(0, 100)
                val more = if (waiting > 0) " · $waiting waiting" else ""
                return "$pct%  ${running.label.ifBlank { running.title }}$more"
            }
            if (waiting > 0) return if (waiting == 1) "1 waiting" else "$waiting waiting"
            return null
        }

    fun enqueue(
        title: String,
        detail: String,
        work: suspend (onProgress: suspend (Float, String) -> Unit, canceled: () -> Boolean) -> String,
    ) {
        val id = nextId.getAndIncrement()
        val stop = AtomicBoolean(false)
        synchronized(stops) { stops[id] = stop }
        items = items + TransferItem(id, title, detail, TransferState.Waiting, 0f, "Waiting")
        scope.launch {
            inbox.send(TransferWork(id, stop) {
                if (stop.get()) {
                    mark(id) { it.copy(state = TransferState.Canceled, label = "Canceled") }
                    synchronized(stops) { stops.remove(id) }
                    return@TransferWork
                }
                mark(id) { it.copy(state = TransferState.Copying, progress = 0f, label = "Starting") }
                val outcome = runCatching {
                    work(
                        { fraction, label ->
                            mark(id) {
                                if (it.state == TransferState.Copying) {
                                    it.copy(progress = fraction.coerceIn(0f, 1f), label = label)
                                } else {
                                    it
                                }
                            }
                        },
                        { stop.get() },
                    )
                }.getOrElse { it.message ?: "The copy stopped." }
                val canceled = stop.get() || outcome.startsWith("Canceled")
                mark(id) {
                    when {
                        canceled -> it.copy(state = TransferState.Canceled, label = "Canceled", note = outcome)
                        outcome.startsWith("Copied") -> {
                            it.copy(state = TransferState.Done, progress = 1f, label = "Done", note = outcome)
                        }
                        else -> it.copy(state = TransferState.Failed, label = "Stopped", note = outcome)
                    }
                }
                synchronized(stops) { stops.remove(id) }
            })
        }
    }

    fun cancel(id: Long) {
        synchronized(stops) { stops[id] }?.set(true)
        items = items.map { item ->
            if (item.id == id && item.state == TransferState.Waiting) {
                item.copy(state = TransferState.Canceled, label = "Canceled")
            } else {
                item
            }
        }
    }

    fun clearFinished() {
        items = items.filter { it.state == TransferState.Waiting || it.state == TransferState.Copying }
    }

    private suspend fun mark(id: Long, change: (TransferItem) -> TransferItem) {
        withContext(Dispatchers.Main) {
            items = items.map { if (it.id == id) change(it) else it }
        }
    }
}

data class ArrivedFile(
    val id: Long,
    val name: String,
    val size: Long,
    val folderId: Long,
    val importedAt: Long,
)

data class ArrivedFolder(val id: Long, val parentId: Long, val name: String)

/**
 * Files and folders that have landed, one at a time. The library inserts each
 * one; it does not rebuild the whole list when a batch finishes.
 */
object LibraryArrivals {
    var stamp by mutableStateOf(0)
        private set
    private val files = ArrayList<ArrivedFile>()
    private val folders = ArrayList<ArrivedFolder>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    fun addFile(id: Long, name: String, size: Long, folderId: Long) {
        if (id <= 0L) return
        val whenSec = System.currentTimeMillis() / 1000L
        scope.launch {
            if (files.none { it.id == id }) {
                files.add(ArrivedFile(id, name, size, folderId, whenSec))
                stamp += 1
            }
        }
    }

    fun addFolder(id: Long, parentId: Long, name: String) {
        if (id <= 0L) return
        scope.launch {
            if (folders.none { it.id == id }) {
                folders.add(ArrivedFolder(id, parentId, name))
                stamp += 1
            }
        }
    }

    fun take(): Pair<List<ArrivedFile>, List<ArrivedFolder>> {
        val outFiles = files.toList()
        val outFolders = folders.toList()
        files.clear()
        folders.clear()
        return outFiles to outFolders
    }
}

/** One quiet line. Tap it for the list. */
@Composable
fun TransferBar(onOpen: () -> Unit) {
    val sync = SyncV4Run.progress
    val line = TransferHub.headline
    if (sync == null && line == null) return
    Row(
        Modifier
            .fillMaxWidth()
            .background(Color(0xFF16324F))
            .clickable(onClick = onOpen)
            .padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // A sync in flight owns the left of the bar, so leaving the Sync page
        // never hides that work is still happening.
        if (sync != null) {
            Text(
                "⇅ Syncing",
                color = Mvp.accent,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
            )
            Spacer(Modifier.width(6.dp))
            Text(
                if (line != null) "$line · ${sync.label}" else sync.label,
                color = Color.White,
                fontSize = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
        } else {
            Text(
                line.orEmpty(),
                color = Color.White,
                fontSize = 13.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
        }
    }
}

/** What is coming in, what is moving, and what you can stop. */
@Composable
fun TransferPage(onClose: () -> Unit) {
    Dialog(
        onDismissRequest = onClose,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false),
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .background(Mvp.window)
                .statusBarsPadding()
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "‹",
                    color = Mvp.accent,
                    fontSize = 22.sp,
                    modifier = Modifier.clickable(onClick = onClose).padding(end = 12.dp),
                )
                Text("Transfers", color = Mvp.title, fontSize = 22.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                val finished = TransferHub.items.any {
                    it.state != TransferState.Waiting && it.state != TransferState.Copying
                }
                if (finished) {
                    Text(
                        "Clear",
                        color = Mvp.accent,
                        fontSize = 15.sp,
                        modifier = Modifier.clickable { TransferHub.clearFinished() }.padding(8.dp),
                    )
                }
            }
            Text(
                "Copies keep going while you look around. Another one waits its turn.",
                color = Mvp.description,
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp, bottom = 14.dp),
            )
            val rows = TransferHub.items
            if (rows.isEmpty()) {
                Text("Nothing moving.", color = Mvp.description, fontSize = 15.sp, modifier = Modifier.padding(top = 24.dp))
            } else {
                Column(
                    Modifier.verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    rows.asReversed().forEach { item ->
                        TransferRow(item)
                    }
                }
            }
        }
    }
}

@Composable
private fun TransferRow(item: TransferItem) {
    val alive = item.state == TransferState.Waiting || item.state == TransferState.Copying
    Column(
        Modifier
            .fillMaxWidth()
            .background(Color(0xFF161618), RoundedCornerShape(12.dp))
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(item.title, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(item.detail, color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            if (alive) {
                Text(
                    "Cancel",
                    color = Color(0xFFFF6B6B),
                    fontSize = 14.sp,
                    modifier = Modifier.clickable { TransferHub.cancel(item.id) }.padding(start = 10.dp),
                )
            } else {
                Text(item.label, color = if (item.state == TransferState.Done) Mvp.accent else Mvp.description, fontSize = 13.sp)
            }
        }
        if (item.state == TransferState.Copying) {
            LinearProgressIndicator(
                progress = { item.progress.coerceIn(0f, 1f) },
                modifier = Modifier.fillMaxWidth(),
                color = Mvp.primary,
                trackColor = Mvp.sliderGroove,
            )
            Text(item.label, color = Mvp.description, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        } else if (item.state == TransferState.Waiting) {
            Text("Waiting", color = Mvp.description, fontSize = 12.sp)
        } else if (item.note.isNotBlank() && item.state != TransferState.Done) {
            Text(item.note, color = Mvp.description, fontSize = 12.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
    }
}
