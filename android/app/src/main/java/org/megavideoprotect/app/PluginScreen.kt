package org.megavideoprotect.app

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit

/** Vault > Add > Plugins. Both registered native adapters and sandbox actions need version-scoped consent. */
@Composable
fun PluginScreen(vaultRoot: String, onClose: () -> Unit) {
    val context = LocalContext.current
    val manager = remember(vaultRoot) { PluginManager(context.applicationContext, vaultRoot) }
    val scope = rememberCoroutineScope()
    var manifests by remember { mutableStateOf<List<PluginManifest>>(emptyList()) }
    var revision by remember { mutableIntStateOf(0) }
    var error by remember { mutableStateOf("") }
    var consent by remember { mutableStateOf<PluginManifest?>(null) }
    var configure by remember { mutableStateOf<PluginManifest?>(null) }
    var browser by remember { mutableStateOf<PluginSession?>(null) }
    var actionBrowser by remember { mutableStateOf<PluginManifest?>(null) }
    var installing by remember { mutableStateOf(false) }
    LaunchedEffect(revision) {
        try { manifests = withContext(Dispatchers.IO) { manager.manifests() } }
        catch (_: Exception) { error = "Plugin descriptors could not be loaded." }
    }
    DisposableEffect(vaultRoot) { onDispose { PluginRuntime.close() } }
    fun back() { if (browser != null || actionBrowser != null) { PluginRuntime.close(); browser=null;actionBrowser=null } else onClose() }
    BackHandler { back() }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) scope.launch {
            installing = true; error = ""
            try { withContext(Dispatchers.IO) { manager.install(uri) }; revision++ }
            catch (e: Exception) { error = e.message ?: "Manifest installation failed" }
            finally { installing = false }
        }
    }
    Surface(color = Mvp.window, contentColor = Mvp.text, modifier = Modifier.fillMaxSize()) {
        Column(Modifier.safeDrawingPadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = { back() }) { Text("Back") }
                Text(browser?.manifest?.name ?: actionBrowser?.name ?: "Portable extensions", fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            }
            if (browser != null) {
                PluginBrowser(checkNotNull(browser))
            } else if (actionBrowser != null) {
                val active=checkNotNull(actionBrowser)
                ExtensionActionPanel(active,manager.booleanSettings(active))
            } else {
                Text("Install portable JSON extensions: approved native provider adapters or bounded sandbox commands. Disabled by default; permissions are approved per extension version and vault. No external native code is loaded.")
                OutlinedButton(onClick = { picker.launch(arrayOf("application/json")) }, enabled = !installing) { Text(if (installing) "Installing…" else "Install JSON manifest") }
                if (error.isNotEmpty()) Text(error, color = MaterialTheme.colorScheme.error)
                LazyColumn(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    items(manifests, key = { it.id }) { manifest ->
                        val settings = remember(manifest.id, revision) { manager.options(manifest) }
                        Card(Modifier.fillMaxWidth()) {
                            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text(manifest.name + " · " + manifest.version, fontWeight = FontWeight.Bold)
                                Text(manifest.description)
                                Text(manifest.sourceNotice, style = MaterialTheme.typography.bodySmall)
                                Text(if (settings.enabled) "Enabled for this vault" else "Disabled", color = if (settings.enabled) Mvp.accent else Mvp.description)
                                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                    TextButton(onClick = {
                                        if (settings.enabled) {
                                            PluginRuntime.close(); browser = null
                                            manager.save(manifest,settings.copy(enabled=false)); revision++
                                        } else consent = manifest
                                    }) { Text(if (settings.enabled) "Disable" else "Enable…") }
                                    TextButton(onClick = { configure = manifest }) { Text("Settings") }
                                    if (settings.enabled) TextButton(onClick = {
                                        if(PluginProviderRegistry.protocol(manifest.protocol).music) onClose()
                                        else actionBrowser = manifest
                                    }) { Text(if(PluginProviderRegistry.protocol(manifest.protocol).music) "Search in Library" else "Open extension") }
                                }
                                if (!manager.isBundled(manifest)) TextButton(onClick = {
                                    runCatching { manager.remove(manifest) }.onFailure { error = it.message.orEmpty() }
                                    revision++
                                }) { Text("Remove") }
                            }
                        }
                    }
                }
            }
        }
    }
    consent?.let { manifest ->
        AlertDialog(onDismissRequest = { consent = null }, title = { Text("Enable ${manifest.name}?") }, text = {
            Text(if(PluginProviderRegistry.protocol(manifest.protocol).music)
                "Network: ${manifest.allowedHosts.joinToString()} over HTTPS. Your searches are sent to this service.\n\nPermissions: audio playback and encrypted vault import. Download is manual unless you separately enable auto-save after actual playback begins.\n\n${manifest.sourceNotice}"
                else "This extension contains JavaScript commands that run locally in a bounded sandbox. No network, filesystem, vault keys or host APIs are exposed. Only the text you enter is passed to a fresh runtime.\n\n${manifest.sourceNotice}")
        }, confirmButton = { TextButton(onClick = {
            manager.save(manifest,manager.options(manifest).copy(enabled=true)); consent=null; revision++
        }) { Text("Approve and enable") } }, dismissButton = { TextButton(onClick = { consent=null }) { Text("Cancel") } })
    }
    configure?.let { manifest ->
        if(PluginProviderRegistry.protocol(manifest.protocol).music) {
            PluginSettingsDialog(manifest.name, manager.options(manifest), onDismiss = { configure = null }, onSave = {
                manager.save(manifest,it); configure=null; revision++
            })
        } else ExtensionSettingsDialog(manifest,manager.booleanSettings(manifest),onDismiss={configure=null},onSave={
            manager.saveBooleanSettings(manifest,it);configure=null;revision++
        })
    }
}

@Composable
private fun ExtensionSettingsDialog(manifest: PluginManifest, initial: Map<String,Boolean>, onDismiss: () -> Unit, onSave: (Map<String,Boolean>) -> Unit) {
    var values by remember(manifest.id,manifest.version) {mutableStateOf(initial)}
    AlertDialog(onDismissRequest=onDismiss,title={Text("${manifest.name} settings")},text={
        Column(verticalArrangement=Arrangement.spacedBy(10.dp)) {
            Text("${manifest.id} · ${manifest.version}",style=MaterialTheme.typography.bodySmall)
            Text("Boolean preferences are isolated to this extension version and vault. Only these declared values accompany submitted text; no secrets or host APIs enter the sandbox.",style=MaterialTheme.typography.bodySmall)
            manifest.settings.forEach {setting -> Row(verticalAlignment=Alignment.CenterVertically) {
                Text(setting.title,modifier=Modifier.weight(1f))
                Switch(checked=values[setting.id] ?: setting.defaultValue,onCheckedChange={values=values+(setting.id to it)})
            } }
            if(manifest.settings.isEmpty())Text("This extension declares no settings.")
        }
    },confirmButton={TextButton(onClick={onSave(values)}){Text("Save")}},dismissButton={TextButton(onClick=onDismiss){Text("Cancel")}})
}

@Composable
private fun PluginSettingsDialog(name: String, initial: PluginOptions, onDismiss: () -> Unit, onSave: (PluginOptions) -> Unit) {
    var settings by remember { mutableStateOf(initial) }
    var folders by remember { mutableStateOf<List<FolderEntry>>(emptyList()) }
    var chooseFolder by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    LaunchedEffect(Unit) {
        try { folders = withContext(Dispatchers.IO) { Json.folders(CoreBridge.nativeListFolders()) } }
        catch (_: Exception) { error = "Vault folders could not be loaded" }
    }
    fun folderPath(folder: FolderEntry): String {
        val parts = mutableListOf(folder.name); val seen=mutableSetOf(folder.id); var parent=folder.parentId
        while (parent != 0L && seen.add(parent)) {
            val p=folders.firstOrNull { it.id==parent } ?: break
            parts.add(0,p.name); parent=p.parentId
        }
        return "Vault / " + parts.joinToString(" / ")
    }
    AlertDialog(onDismissRequest=onDismiss,title={ Text("$name settings") },text={
        Column(verticalArrangement=Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment=Alignment.CenterVertically) {
                Checkbox(settings.autoSave,onCheckedChange={ settings=settings.copy(autoSave=it) })
                Text("Auto-save only after playback starts")
            }
            Text("Destination is scoped to this vault. No search history, audio files or artwork caches are persisted.", style=MaterialTheme.typography.bodySmall)
            TextButton(onClick={ chooseFolder=true }) {
                Text(if (settings.folderId==0L) "Destination: Vault root" else folders.firstOrNull { it.id==settings.folderId }?.let(::folderPath) ?: "Destination missing — choose another folder")
            }
            Text("Audio RAM limit (default 32 MiB)")
            Row { listOf(32,64,128).forEach { limit ->
                FilterChip(selected=settings.memoryMiB==limit,onClick={settings=settings.copy(memoryMiB=limit)},label={Text("$limit MiB")},modifier=Modifier.padding(end=4.dp))
            } }
            Text("One track at a time. Decoder/JVM/native overhead is additional; higher limits can exhaust low-memory devices.",style=MaterialTheme.typography.bodySmall)
            if (error.isNotEmpty()) Text(error,color=MaterialTheme.colorScheme.error)
        }
    },confirmButton={ TextButton(onClick={onSave(settings)},enabled=error.isEmpty() && (settings.folderId==0L || folders.any{it.id==settings.folderId})) {Text("Save")} },dismissButton={TextButton(onClick=onDismiss){Text("Cancel")}})
    if (chooseFolder) AlertDialog(onDismissRequest={chooseFolder=false},title={Text("Choose encrypted vault folder")},text={
        LazyColumn(Modifier.heightIn(max=360.dp)) {
            item { TextButton(onClick={settings=settings.copy(folderId=0);chooseFolder=false}){Text("Vault root")} }
            items(folders,key={it.id}) { folder -> TextButton(onClick={settings=settings.copy(folderId=folder.id);chooseFolder=false}) {Text(folderPath(folder))} }
        }
    },confirmButton={TextButton(onClick={chooseFolder=false}){Text("Close")}})
}

@Composable
private fun ColumnScope.PluginBrowser(session: PluginSession) {
    var query by remember { mutableStateOf("") }
    var filter by remember { mutableStateOf("All") }
    val images = remember(session) { Semaphore(2) }
    OutlinedTextField(query,onValueChange={query=it.take(240)},label={Text("Songs, artists or albums")},singleLine=true,modifier=Modifier.fillMaxWidth())
    Button(onClick={ if(query.isNotBlank()) session.browse(query=query) },enabled=query.isNotBlank()) { Text("Search ${session.manifest.name}") }
    Row { listOf("All","Songs","Artists","Albums").forEach { mode ->
        FilterChip(selected=filter==mode,onClick={filter=mode},label={Text(mode)},modifier=Modifier.padding(end=4.dp))
    } }
    Text(session.heading,fontWeight=FontWeight.Bold)
    if(session.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
    if(session.message.isNotEmpty()) Text(session.message,style=MaterialTheme.typography.bodySmall)
    session.selected?.let { track ->
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp)) {
                Text(track.title,fontWeight=FontWeight.Bold); Text(track.subtitle)
                var position by remember(track.id) { mutableFloatStateOf(0f) }
                var duration by remember(track.id) { mutableFloatStateOf(1f) }
                LaunchedEffect(track.id) { while(isActive) {
                    position=session.player.currentPosition.toFloat()
                    duration=session.player.duration.takeIf {it>0}?.toFloat() ?: 1f
                    delay(400)
                } }
                Slider(value=position.coerceIn(0f,duration),onValueChange={ position=it;session.player.seekTo(it.toLong()) },valueRange=0f..duration)
                Row {
                    TextButton(onClick={session.toggle()}) { Text(if(session.playing) "Pause" else "Play") }
                    TextButton(onClick={session.download()},enabled=!session.saved && !session.saveBusy) { Text(if(session.saveBusy) "Encrypting…" else if(session.saved) "Saved" else "Download to vault") }
                }
            }
        }
    }
    LazyColumn(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(4.dp)) {
        items(session.rows.filter { filter=="All" || (filter=="Songs" && it.kind=="track") || (filter=="Artists" && it.kind=="artist") || (filter=="Albums" && it.kind=="album") },key={it.kind+it.id}) { item ->
            Row(Modifier.fillMaxWidth().clickable(enabled=item.playable) {
                if(item.kind=="track") session.play(item) else session.browse(item=item)
            }.padding(vertical=7.dp),verticalAlignment=Alignment.CenterVertically) {
                PluginArtwork(session,item.artwork,images)
                Column(Modifier.weight(1f).padding(start=12.dp)) {
                    Text(item.title,fontWeight=FontWeight.Medium)
                    Text("${item.kind} · ${item.subtitle}" + if(item.playable) "" else " · unavailable",style=MaterialTheme.typography.bodySmall)
                }
                Text(if(item.kind=="track") "Play" else "Open",color=Mvp.accent)
            }
        }
    }
}

@Composable
internal fun PluginArtwork(session: PluginSession, url: String, semaphore: Semaphore) {
    var bitmap by remember(url) { mutableStateOf<Bitmap?>(null) }
    LaunchedEffect(url,session) {
        if(url.isNotBlank()) semaphore.withPermit {
            var bytes: ByteArray? = null
            try {
                withContext(Dispatchers.IO) {
                    bytes=session.artwork(url)
                    val b=checkNotNull(bytes)
                    val info=BitmapFactory.Options().apply {inJustDecodeBounds=true}
                    BitmapFactory.decodeByteArray(b,0,b.size,info)
                    if(info.outWidth in 1..4096 && info.outHeight in 1..4096) {
                        val options=BitmapFactory.Options().apply {inMutable=true;inSampleSize=maxOf(1,maxOf(info.outWidth,info.outHeight)/160)}
                        currentCoroutineContext().ensureActive()
                        bitmap=BitmapFactory.decodeByteArray(b,0,b.size,options)?.also { session.ownArtwork(it) }
                    }
                }
            } catch (_: Exception) { /* Artwork failure never broadens the host allowlist. */ }
            finally {bytes?.fill(0)}
        }
    }
    DisposableEffect(bitmap) {
        val owned = bitmap
        onDispose { owned?.let { session.releaseArtwork(it) } }
    }
    // Erase but do not recycle a bitmap that the renderer may still reference.
    Box(Modifier.size(48.dp).background(Mvp.card),contentAlignment=Alignment.Center) {
        bitmap?.let { Image(it.asImageBitmap(),contentDescription=null,modifier=Modifier.fillMaxSize(),contentScale=ContentScale.Crop) } ?: Text("♪",color=Mvp.description)
    }
}
