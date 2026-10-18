package org.megavideoprotect.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import java.util.Locale
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * One small sync section: server URL, library key, Sync. The relay is an
 * encrypted hash bucket; this screen only exports what is checked and imports
 * what it has never seen.
 */
@Composable
fun SyncScreen(vaultRoot: String, onClose: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val mutex = remember { Mutex() }
    val vault = remember { NativeSyncVault() }

    var engine by remember { mutableStateOf<SyncEngine?>(null) }
    var server by remember { mutableStateOf("") }
    var key by remember { mutableStateOf("") }
    var showKey by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var status by remember { mutableStateOf("Loading private sync state…") }
    var files by remember { mutableStateOf<List<SyncLocalFile>>(emptyList()) }
    var packages by remember { mutableStateOf<List<JSONObject>>(emptyList()) }
    var selection by remember { mutableStateOf<Set<Long>>(emptySet()) }
    var filter by remember { mutableStateOf("") }
    var chosen by remember { mutableStateOf<String?>(null) }
    var confirmDelete by remember { mutableStateOf(false) }

    suspend fun readLocalFiles(): List<SyncLocalFile>? = withContext(Dispatchers.IO) {
        runCatching { if (vault.isUnlocked()) vault.files() else null }.getOrNull()
    }

    fun action(block: suspend (SyncEngine) -> Unit) {
        if (busy) return
        busy = true
        error = null
        scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    mutex.withLock { block(engine ?: error("Sync state unavailable")) }
                }
            } catch (cancel: CancellationException) {
                throw cancel
            } catch (failure: Exception) {
                error = failure.message ?: "Sync failed"
            } finally {
                val refreshed = readLocalFiles()
                busy = false
                engine?.let { current ->
                    status = current.status
                    current.failure?.let { error = it }
                    packages = current.packages()
                    selection = current.selectedIds()
                    if (current.libraryChanged) GalleryCache.clear()
                }
                if (refreshed != null) files = refreshed
            }
        }
    }

    LaunchedEffect(vaultRoot) {
        try {
            val created = withContext(Dispatchers.IO) {
                mutex.withLock { SyncEngine(AndroidSyncStore(context.applicationContext, vaultRoot), vault) }
            }
            engine = created
            server = created.savedServer()
            key = created.savedKey()
            status = created.status
            packages = created.packages()
            selection = created.selectedIds()
            readLocalFiles()?.let { files = it }
        } catch (cancel: CancellationException) {
            throw cancel
        } catch (failure: Exception) {
            error = "Private sync state unavailable: ${failure.message}"
        }
    }

    DisposableEffect(Unit) { onDispose { GalleryCache.clear() } }

    Column(
        Modifier.fillMaxSize().safeDrawingPadding().imePadding().verticalScroll(rememberScrollState()).padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        MvpTitle("Encrypted sync")
        MvpButton("Back to local gallery", onClick = onClose)
        MvpDescription(
            "Your local vault stays the library. Each shared item is exported as one encrypted .mvpk " +
                "package under the library key, named by the SHA-256 of its own bytes, so the relay " +
                "stores opaque packages and never sees a name or a byte of content.",
        )
        MvpInput(server, { server = it }, "Server URL, for example https://vault.example:8766", enabled = !busy)
        MvpInput(key, { key = it }, "Library key: 64 lowercase hex characters", isPassword = !showKey, enabled = !busy)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            MvpButton("Sync", onClick = {
                val url = server
                val secret = key
                action { current ->
                    if (!current.configure(url, secret)) return@action
                    current.select(selection)
                    current.syncNow()
                }
            }, primary = true, enabled = !busy && engine != null)
            MvpButton(if (showKey) "Hide key" else "Show key", onClick = { showKey = !showKey })
        }
        if (error == null) Text(status, color = Mvp.description)
        MvpError(error)
        if (engine?.configured() == true) {
            MvpDescription("Shared items on this device: ${engine!!.sharedCount()} · packages known here: ${engine!!.knownCount()}")
        }
        if (showKey && key.isNotEmpty()) {
            SelectionContainer { Text(key, color = Mvp.description) }
        }

        Text("Share these items", color = Mvp.title)
        MvpInput(filter, { filter = it }, "Filter local file/folder path", enabled = !busy)
        val matching = files.filter { it.path.contains(filter, ignoreCase = true) }
        Text(
            "${matching.size} matching file(s), first 200 shown · ${selection.size} selected",
            color = Mvp.description,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            MvpButton("Select shown", onClick = { selection = selection + matching.take(200).map { it.id } }, enabled = !busy)
            MvpButton("Clear", onClick = {
                selection = emptySet()
                scope.launch { withContext(Dispatchers.IO) { mutex.withLock { engine?.select(emptySet()) } } }
            }, enabled = !busy)
        }
        matching.take(200).forEach { file ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(
                    checked = file.id in selection,
                    onCheckedChange = { checked ->
                        val next = if (checked) selection + file.id else selection - file.id
                        selection = next
                        scope.launch { withContext(Dispatchers.IO) { mutex.withLock { engine?.select(next) } } }
                    },
                    enabled = !busy,
                )
                Text(file.path, color = Mvp.description, modifier = Modifier.weight(1f))
            }
        }

        Text("Packages on the server", color = Mvp.title)
        if (packages.isEmpty()) {
            MvpDescription("Press Sync to list the bucket. Names stay opaque; contents appear in the local gallery after import.")
        } else {
            MvpDescription("${packages.size} package(s); the first 200 are shown. Select one to delete it for every device.")
        }
        packages.take(200).forEach { entry ->
            val name = entry.optString("name")
            Row(verticalAlignment = Alignment.CenterVertically) {
                RadioButton(selected = chosen == name, onClick = { chosen = name }, enabled = !busy)
                Column(Modifier.weight(1f)) {
                    Text(
                        "${name.take(12)}… .mvpk · ${sizeText(entry.optLong("size", 0L))}",
                        color = Mvp.description,
                    )
                    Text(
                        if (engine?.isKnown(name) == true) "on this device" else "not imported here",
                        color = if (engine?.isKnown(name) == true) Mvp.description else Mvp.accent,
                    )
                }
            }
        }
        MvpButton(
            "Delete the selected package from the server…",
            onClick = { confirmDelete = true },
            enabled = !busy && chosen != null,
        )
        MvpDescription(
            "Deletes only what you select, one package at a time, for every device; local items are never " +
                "removed. Unchanged items are not uploaded twice. Sync does not run in the background: " +
                "press Sync after changing what you share.",
        )
        MvpButton("Back to local gallery", onClick = onClose)
    }

    if (confirmDelete && chosen != null) {
        val name = chosen!!
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text("Delete a package from the server") },
            text = {
                Text(
                    "This deletes the encrypted package ${name.take(12)}… from the server for every device. " +
                        "Local items are not deleted. Continue?",
                )
            },
            confirmButton = {
                MvpButton("Delete", onClick = {
                    confirmDelete = false
                    chosen = null
                    action { current -> current.remove(name) }
                })
            },
            dismissButton = { MvpButton("Cancel", onClick = { confirmDelete = false }) },
        )
    }
}

private fun sizeText(bytes: Long): String = when {
    bytes < 1024 -> "$bytes B"
    bytes < 1024 * 1024 -> "${bytes / 1024} KiB"
    else -> String.format(Locale.US, "%.1f MiB", bytes / (1024.0 * 1024.0))
}
