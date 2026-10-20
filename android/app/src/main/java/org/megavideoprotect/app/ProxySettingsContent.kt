package org.megavideoprotect.app

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.UUID

/** Telegram-style list and editor; selection is not advertised as a connectivity test. */
@Composable
fun ProxySettingsContent() {
    val scope = rememberCoroutineScope()
    var state by remember { mutableStateOf(AppProxy.snapshot) }
    var editing by remember { mutableStateOf(false) }
    var id by remember { mutableStateOf("") }
    var kind by remember { mutableStateOf(ProxyKind.SOCKS5) }
    var host by remember { mutableStateOf("") }
    var port by remember { mutableStateOf("1080") }
    var username by remember { mutableStateOf("") }
    // Deliberately not rememberSaveable: never put passwords in Activity saved state.
    var password by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    fun save(next: ProxySnapshot, done: () -> Unit = {}) {
        busy = true; error = null
        scope.launch {
            val ok = withContext(Dispatchers.IO) { runCatching { AppProxy.save(next) }.isSuccess }
            busy = false
            if (ok) { state = AppProxy.snapshot; done() }
            else error = "Could not securely save proxy settings. The previous routing policy is unchanged."
        }
    }
    if (AppProxy.settingsUnavailable) {
        Text("Proxy settings could not be decrypted. Network access is blocked; no direct fallback is used.", color = Mvp.errorText)
        return
    }
    if (editing) {
        Text(if (state.profiles.any { it.id == id }) "Edit Proxy" else "Add Proxy", color = Mvp.title, fontSize = 18.sp)
        Row(verticalAlignment = Alignment.CenterVertically) {
            ProxyKind.entries.forEach { value ->
                RadioButton(selected = kind == value, onClick = { if (!busy) kind = value })
                Text(value.name, color = Mvp.text, modifier = Modifier.clickable(enabled = !busy) { kind = value }.padding(end = 16.dp))
            }
        }
        OutlinedTextField(host, { host = it }, label = { Text("Server") }, singleLine = true, enabled = !busy,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, autoCorrectEnabled = false), modifier = Modifier.fillMaxWidth())
        OutlinedTextField(port, { port = it }, label = { Text("Port") }, singleLine = true, enabled = !busy,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), modifier = Modifier.fillMaxWidth())
        OutlinedTextField(username, { username = it }, label = { Text("Username (optional)") }, singleLine = true, enabled = !busy,
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false), modifier = Modifier.fillMaxWidth())
        OutlinedTextField(password, { password = it }, label = { Text("Password (optional)") }, singleLine = true, enabled = !busy,
            visualTransformation = PasswordVisualTransformation(), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, autoCorrectEnabled = false), modifier = Modifier.fillMaxWidth())
        Row {
            TextButton(enabled = !busy, onClick = { editing = false; password = ""; username = ""; error = null }) { Text("Cancel") }
            TextButton(enabled = !busy, onClick = {
                try {
                    val profile = ProxyProfile(id, kind, host.trim(), port.toIntOrNull() ?: 0, username, password)
                    val entries = state.profiles.filterNot { it.id == id } + profile
                    save(ProxySnapshot(state.enabled, state.selected ?: id, entries)) { editing = false; password = ""; username = "" }
                } catch (e: IllegalArgumentException) { error = e.message ?: "Invalid proxy settings" }
            }) { Text(if (busy) "Saving…" else "Save") }
            if (state.profiles.any { it.id == id }) TextButton(enabled = !busy, onClick = {
                val entries = state.profiles.filterNot { it.id == id }
                val selected = if (state.selected == id) entries.firstOrNull()?.id else state.selected
                // Deleting the last active proxy leaves Use Proxy ON and blocks traffic until explicitly disabled.
                save(ProxySnapshot(state.enabled, selected, entries)) { editing = false; password = ""; username = "" }
            }) { Text("Delete", color = Mvp.destructive) }
        }
    } else {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("Use Proxy", color = Mvp.text, fontSize = 17.sp, modifier = Modifier.weight(1f))
            Switch(checked = state.enabled, enabled = !busy, onCheckedChange = { save(ProxySnapshot(it, state.selected, state.profiles)) })
        }
        Text("Routes app sync, plugins and media, updates, and PC connections. No direct fallback. External browsers use their own network settings.", color = Mvp.description, fontSize = 13.sp)
        if (state.enabled && state.profiles.none { it.id == state.selected }) {
            Text("Traffic is blocked. Add or select a proxy, or turn Use Proxy off.", color = Mvp.errorText, fontSize = 13.sp)
        }
        state.profiles.forEach { profile ->
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                RadioButton(selected = state.selected == profile.id, enabled = !busy,
                    onClick = { save(ProxySnapshot(state.enabled, profile.id, state.profiles)) })
                Column(Modifier.weight(1f).clickable(enabled = !busy) { save(ProxySnapshot(state.enabled, profile.id, state.profiles)) }.padding(vertical = 10.dp)) {
                    Text(if (':' in profile.host) "[${profile.host}]:${profile.port}" else "${profile.host}:${profile.port}", color = Mvp.text)
                    Text("${if (state.selected == profile.id) "Selected" else "Saved"} · ${profile.kind.name}", color = Mvp.description, fontSize = 12.sp)
                }
                TextButton(enabled = !busy, onClick = {
                    id = profile.id; kind = profile.kind; host = profile.host; port = profile.port.toString()
                    username = profile.username; password = profile.password; editing = true; error = null
                }) { Text("Edit") }
            }
        }
        TextButton(enabled = !busy, onClick = {
            id = UUID.randomUUID().toString(); kind = ProxyKind.SOCKS5; host = ""; port = "1080"; username = ""; password = ""; editing = true; error = null
        }) { Text("Add Proxy") }
        Text("Passwords are encrypted with Android Keystore. HTTP proxy authentication and SOCKS5 authentication are not encrypted on the proxy hop; use a proxy you trust. SOCKS5 resolves destination names remotely. Plugin destination safety checks still require local DNS before connecting.", color = Mvp.description, fontSize = 12.sp)
    }
    error?.let { Text(it, color = Mvp.errorText, fontSize = 13.sp) }
}
