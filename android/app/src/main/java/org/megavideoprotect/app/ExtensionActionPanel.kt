package org.megavideoprotect.app

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.*
import org.json.JSONObject

/** A fresh bounded native JS runtime per invocation. No host APIs or vault objects enter it. */
class ExtensionActionSession(val manifest: PluginManifest) {
    private val scope=CoroutineScope(SupervisorJob()+Dispatchers.Main.immediate)
    private var generation=0L
    private var job: Job?=null
    private var closed=false
    var busy by mutableStateOf(false); private set
    var title by mutableStateOf(""); private set
    var text by mutableStateOf(""); private set
    init {
        require(manifest.protocol=="megavault-actions-v1")
        PluginRuntime.close();PluginRuntime.actions=this
    }
    fun run(command: PluginCommand, input: String, settings: Map<String,Boolean>) {
        require(manifest.commands.any {it.id==command.id})
        if(closed) return
        job?.cancel();val token=++generation;val source=manifest.script
        val effective=manifest.settings.associate {it.id to (settings[it.id] ?: it.defaultValue)}
        val payload=JSONObject().put("text",input.take(16000)).put("settings",JSONObject(effective)).toString()
        job=scope.launch {
            busy=true;title="";text=""
            try {
                val raw=withContext(Dispatchers.Default) {CoreBridge.nativeInvokeExtension(source,command.id,payload)}
                ensureActive()
                if(closed || token!=generation)return@launch
                val result=JSONObject(raw)
                if(!result.optBoolean("ok")) {
                    title="Extension failed";text=result.optString("error","The sandbox rejected this command.").take(2000)
                } else {
                    val output=result.getJSONObject("result")
                    title=output.getString("title").take(300)
                    text=output.getString("text").take(65536)
                }
            } catch (_: CancellationException) { }
            catch (_: Exception) { if(!closed && token==generation) { title="Extension failed";text="The command did not return a valid {title, text} result." } }
            finally {if(token==generation)busy=false}
        }
    }
    fun close() {closed=true;generation++;scope.cancel();title="";text="";busy=false}
}

@Composable
fun ExtensionActionPanel(manifest: PluginManifest, settings: Map<String,Boolean>) {
    val session=remember(manifest.id,manifest.version) {ExtensionActionSession(manifest)}
    var command by remember(manifest.id) {mutableStateOf(manifest.commands.first())}
    var input by remember(manifest.id) {mutableStateOf("")}
    var menu by remember {mutableStateOf(false)}
    DisposableEffect(session) {onDispose {input="";session.close();if(PluginRuntime.actions===session)PluginRuntime.actions=null}}
    Column(verticalArrangement=Arrangement.spacedBy(12.dp)) {
        Text(manifest.name,fontWeight=FontWeight.Bold)
        Text("Runs locally in a fresh bounded JavaScript sandbox. No network, files, vault keys or host objects are exposed.",style=MaterialTheme.typography.bodySmall)
        Box {
            OutlinedButton(onClick={menu=true},enabled=!session.busy) {Text(command.title + " ▾")}
            DropdownMenu(expanded=menu,onDismissRequest={menu=false}) {
                manifest.commands.forEach {c -> DropdownMenuItem(text={Text(c.title)},onClick={command=c;menu=false})}
            }
        }
        OutlinedTextField(input,onValueChange={input=it.take(16000)},label={Text(command.inputLabel)},modifier=Modifier.fillMaxWidth().heightIn(min=160.dp,max=260.dp),enabled=!session.busy)
        Button(onClick={session.run(command,input,settings)},enabled=!session.busy) {Text(if(session.busy)"Running…" else "Run command")}
        if(session.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        if(session.title.isNotBlank()) Text(session.title,fontWeight=FontWeight.Bold)
        if(session.text.isNotBlank()) androidx.compose.foundation.text.selection.SelectionContainer {Text(session.text)}
    }
}
