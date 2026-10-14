package org.megavideoprotect.app

import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.security.SecureRandom

/** Sync is opt-in, screen/lifecycle-bound, and never replaces the local gallery with remote browsing. */
@Composable
fun SyncScreen(vaultRoot: String, onClose: () -> Unit) {
    val context = LocalContext.current
    val lifecycle = (context as ComponentActivity).lifecycle
    val scope = rememberCoroutineScope()
    val mutex = remember { Mutex() }
    val vault = remember { NativeSyncVault() }
    var engine by remember { mutableStateOf<SyncEngine?>(null) }
    var address by remember { mutableStateOf("") }
    var pin by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var name by remember { mutableStateOf("Android device") }
    var recovery by remember { mutableStateOf("") }
    var showRecovery by remember { mutableStateOf(false) }
    var configured by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var status by remember { mutableStateOf("Loading private sync state…") }
    var notice by remember { mutableStateOf("") }
    var files by remember { mutableStateOf<List<SyncLocalFile>>(emptyList()) }
    var records by remember { mutableStateOf<List<JSONObject>>(emptyList()) }
    var localSelection by remember { mutableStateOf<Set<Long>>(emptySet()) }
    var remoteSelection by remember { mutableStateOf<Set<String>>(emptySet()) }
    var receipt by remember { mutableStateOf<JSONObject?>(null) }
    var mappingChoice by remember { mutableStateOf<Long?>(null) }
    var actionJob by remember { mutableStateOf<kotlinx.coroutines.Job?>(null) }
    var localFilter by remember { mutableStateOf("") }
    var remoteFilter by remember { mutableStateOf("") }

    suspend fun snapshot(e: SyncEngine) {
        val list = if (vault.isUnlocked()) vault.files() else emptyList()
        withContext(Dispatchers.Main) {
            configured = e.configured(); status = e.status(); notice = e.notice()
            files = list; records = e.catalog().map { JSONObject(it.toString()) }; remoteSelection = e.selectedDownloads(); receipt = e.receipt()?.let { JSONObject(it.toString()) }
            if (address.isEmpty()) { address = e.endpoint(); pin = e.pin() }
        }
    }
    fun action(block: suspend (SyncEngine) -> Unit) {
        if (busy) return
        busy = true; error = null
        actionJob = scope.launch {
            try {
                withContext(Dispatchers.IO) { mutex.withLock {
                    val e = engine ?: error("Sync state unavailable")
                    try { block(e) } finally { snapshot(e) }
                } }
            } catch (cancel: kotlinx.coroutines.CancellationException) { throw cancel }
            catch (failure: Exception) { error = failure.message ?: "Sync failed" }
            finally { busy = false }
        }
    }
    LaunchedEffect(vaultRoot) {
        try {
            withContext(Dispatchers.IO) { mutex.withLock {
                val e = SyncEngine(AndroidSyncStore(context.applicationContext, vaultRoot), vault)
                withContext(Dispatchers.Main) { engine = e }
                snapshot(e)
            } }
        } catch (cancel: kotlinx.coroutines.CancellationException) { throw cancel }
        catch (failure: Exception) { error = "Private state unavailable: ${failure.message}. Pending files are preserved." }
    }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP) actionJob?.cancel()
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer); actionJob?.cancel(); GalleryCache.clear() }
    }
    LaunchedEffect(engine) {
        val e = engine ?: return@LaunchedEffect
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            var lastSnapshot = 0L
            var lastStatus = ""
            while (true) {
                try {
                    withContext(Dispatchers.IO) { mutex.withLock {
                        if (e.configured() && vault.isUnlocked()) {
                            e.pass()
                            val now = System.currentTimeMillis()
                            val currentStatus = e.status()
                            if (currentStatus != lastStatus || now - lastSnapshot >= 15_000L) {
                                snapshot(e); lastSnapshot = now; lastStatus = currentStatus
                            }
                        }
                    } }
                } catch (cancel: kotlinx.coroutines.CancellationException) { throw cancel }
                catch (failure: Exception) { error = failure.message ?: "Sync paused" }
                delay(1000)
            }
        }
    }
    Column(Modifier.fillMaxSize().safeDrawingPadding().imePadding().verticalScroll(rememberScrollState()).padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)) {
        MvpTitle("Encrypted sync v2")
        MvpButton("Back to local gallery (pause sync)", onClick = { actionJob?.cancel(); onClose() })
        MvpDescription("Local-first encrypted MVPK relay. Unlock this phone's vault first. Recovery key stays on this device; send it to your other device out-of-band, never as a pairing code.")
        Text(status, color = Mvp.description)
        if (notice.isNotEmpty()) Text(notice, color = Mvp.accent)
        MvpError(error)
        if (!configured) {
            MvpInput(address, { address = it }, "HTTPS server URL (no path)")
            MvpInput(pin, { pin = it }, "Optional certificate SHA-256 pin (lowercase hex)")
            MvpInput(name, { name = it }, "Non-sensitive device label")
            MvpInput(code, { code = it }, "Single-use server pairing code", isPassword = true)
            MvpInput(recovery, { recovery = it }, "Shared recovery key: 64 lowercase hex", isPassword = true)
            MvpButton("Generate recovery key for a NEW shared library", onClick = {
                recovery = ByteArray(32).also { SecureRandom().nextBytes(it) }.hex()
                showRecovery = true
            }, enabled = !busy)
            MvpButton(if (showRecovery) "Hide recovery key" else "Show recovery key to copy out-of-band", onClick = { showRecovery = !showRecovery }, enabled = !busy && recovery.isNotEmpty())
            if (showRecovery && recovery.isNotEmpty()) {
                androidx.compose.foundation.text.selection.SelectionContainer {
                    Text("Save/share this key out-of-band before pairing:\n$recovery", color = Mvp.description)
                }
            }
            MvpButton("Pair this local vault", onClick = {
                val url = address; val certificate = pin; val key = recovery; val pairing = code; val label = name
                action { e ->
                    check(vault.isUnlocked()) { "Unlock a local vault first" }
                    require(SyncPolicy.validSecret(key)) { "Recovery key must be exactly 64 lowercase hex" }
                    require(label.isNotBlank() && label.length <= 64 && !label.contains('\n') && !label.contains('\r'))
                    val api = SyncHttp(url, "", certificate)
                    check(api.json("GET", "/healthz").getInt("protocol") == 2) { "Server is not sync v2" }
                    val paired = api.json("POST", "/v2/pair", JSONObject().put("code", pairing).put("name", label))
                    require(SyncPolicy.validId(paired.getString("device_id")))
                    e.configure(url, certificate, key, paired.getString("token"))
                    withContext(Dispatchers.Main) { recovery = ""; code = "" }
                    e.retry(); e.refresh()
                }
            }, primary = true, enabled = !busy && engine != null)
        } else {
            Text("Server: $address", color = Mvp.description)
            MvpInput(code, { code = it }, "Fresh server code if re-pairing is required", isPassword = true)
            MvpButton("Re-pair (preserve key, queue and mappings)", onClick = {
                val pairing = code
                action { e ->
                    val paired = SyncHttp(e.endpoint(), "", e.pin()).json("POST", "/v2/pair", JSONObject().put("code", pairing).put("name", "Android device"))
                    require(SyncPolicy.validId(paired.getString("device_id")))
                    e.rePair(paired.getString("token")); e.retry()
                    withContext(Dispatchers.Main) { code = "" }
                }
            }, enabled = !busy && code.isNotBlank())
            MvpButton("Refresh server catalog", onClick = { action { it.refresh() } }, enabled = !busy)
            MvpButton("Retry pending transfers now", onClick = { action { it.retry(); it.pass() } }, primary = true, enabled = !busy)
            Column {
                MvpButton("Unlimited", onClick = { action { it.setSpeed(0) } }, enabled = !busy)
                MvpButton("1 MiB/s", onClick = { action { it.setSpeed(1024 * 1024) } }, enabled = !busy)
                MvpButton("256 KiB/s", onClick = { action { it.setSpeed(256 * 1024) } }, enabled = !busy)
            }
            if (receipt != null) {
                MvpButton("Retry cached import (preserve local originals)", onClick = { action { it.resumeImport() } }, enabled = !busy)
                if (receipt!!.optString("phase") == "failed") {
                    MvpInput(recovery, { recovery = it }, "Correct initial recovery key locally", isPassword = true)
                    MvpButton("Correct mistyped initial key (not key rotation)", onClick = {
                        val key = recovery
                        action { it.correctInitialRecoveryKey(key); withContext(Dispatchers.Main) { recovery = "" } }
                    }, enabled = !busy && SyncPolicy.validSecret(recovery))
                }
                Text("Confirm the local file imported for object ${receipt!!.getString("id")}. JNI cannot identify deduplicated files. Retry an interrupted/failed import first, then verify the local file before linking; do not link an unrelated file.", color = Mvp.description)
                MvpInput(localFilter, { localFilter = it }, "Filter local file/folder path")
                val matching = files.filter { it.path.contains(localFilter, ignoreCase = true) }
                Text("${matching.size} matching files; displaying first 200. Filter to find others.", color = Mvp.description)
                matching.take(200).forEach { file -> Row {
                    Checkbox(mappingChoice == file.id, onCheckedChange = { mappingChoice = if (it) file.id else null }, enabled = !busy)
                    Text(file.path, color = Mvp.description, modifier = Modifier.weight(1f))
                } }
                MvpButton("Confirm existing local file mapping", onClick = { val id = mappingChoice ?: return@MvpButton; action { it.resolveReceipt(id); GalleryCache.clear() } }, enabled = !busy && mappingChoice != null && receipt!!.optString("phase") == "imported")
            } else {
                Text("Select local files to upload (folder names shown locally only)", color = Mvp.description)
                MvpInput(localFilter, { localFilter = it }, "Filter local file/folder path")
                val matching = files.filter { it.path.contains(localFilter, ignoreCase = true) }
                Text("${matching.size} matching files; displaying first 200. ${localSelection.size} selected.", color = Mvp.description)
                MvpButton("Select matching local files/folders", onClick = { localSelection = matching.map { it.id }.toSet() }, enabled = !busy)
                MvpButton("Clear local selection", onClick = { localSelection = emptySet() }, enabled = !busy)
                matching.take(200).forEach { file -> Row {
                    Checkbox(file.id in localSelection, onCheckedChange = { selected -> localSelection = if (selected) localSelection + file.id else localSelection - file.id }, enabled = !busy)
                    Text(file.path, color = Mvp.description, modifier = Modifier.weight(1f))
                } }
                MvpButton("Stage selected encrypted packages", onClick = { val ids = localSelection; action { it.stage(ids); it.retry() } }, enabled = !busy && localSelection.isNotEmpty())
            }
            Text("Select opaque remote objects to download; names appear only after local import", color = Mvp.description)
            MvpInput(remoteFilter, { remoteFilter = it }, "Filter remote object ID")
            val matchingRemote = records.filter { it.getString("id").contains(remoteFilter) }
            Text("${matchingRemote.size} matching objects; displaying first 200. Filter to find others. ${remoteSelection.size} selected.", color = Mvp.description)
            matchingRemote.take(200).forEach { record ->
                val id = record.getString("id")
                Row {
                    Checkbox(id in remoteSelection, onCheckedChange = { selected ->
                        val selection = if (selected) remoteSelection + id else remoteSelection - id
                        action { it.selectDownloads(selection); it.retry() }
                    }, enabled = !busy && !record.getBoolean("deleted"))
                    Text("$id · rev ${record.getLong("revision")} · ${record.getLong("size")} bytes${if (record.getBoolean("deleted")) " · deleted; local retained" else ""}", color = Mvp.description, modifier = Modifier.weight(1f))
                }
                if (record.getBoolean("deleted")) MvpButton("Acknowledge remote deletion (keep local)", onClick = { action { it.acknowledgeDelete(id) } }, enabled = !busy)
            }
        }
        MvpDescription("Tags are local only; MVPK does not sync tags. Remote updates preserve old local copies. Local removals are never automatically sent as server deletes. Sync pauses when you leave this screen or background the app; reopen and Retry to resume. Native import may use transient plaintext locally; network and sync cache contain encrypted MVPK only.")
        MvpButton("Back to local gallery (pause sync)", onClick = { actionJob?.cancel(); GalleryCache.clear(); onClose() })
    }
}
