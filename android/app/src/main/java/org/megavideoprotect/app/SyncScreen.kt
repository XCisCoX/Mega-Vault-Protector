package org.megavideoprotect.app

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray

/** One vault the account owns, as the relay lists it. */
private data class RemoteVaultRow(
    val id: String,
    val name: String,
    val records: Int,
    val bytes: Long,
) {
    val detail: String
        get() = "$records item${if (records == 1) "" else "s"} · ${humanBytes(bytes)}"
}

private fun humanBytes(value: Long): String = when {
    value < 1024 -> "$value B"
    value < 1024 * 1024 -> "${value / 1024} KiB"
    else -> String.format(Locale.US, "%.1f MiB", value / (1024.0 * 1024.0))
}

/**
 * Protocol 4 sync, laid out like the desktop's Sync page: sign in with the
 * server URL, email and password; pick one of the account's vaults; then Sync
 * now. The vault password you type seals the packages, so a phone reads what
 * another device sealed with the same password.
 */
@Composable
fun SyncScreen(vaultRoot: String, onClose: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val store = remember(vaultRoot) { SyncV4StateStore(context, vaultRoot) }
    val saved = remember(vaultRoot) { store.load() }

    var link by remember(vaultRoot) { mutableStateOf(saved) }
    var server by remember(vaultRoot) { mutableStateOf(saved?.server ?: "https://ciscox.ir") }
    var email by remember(vaultRoot) { mutableStateOf(saved?.email.orEmpty()) }
    var password by remember { mutableStateOf("") }
    var creating by remember { mutableStateOf(false) }
    var registrationOpen by remember { mutableStateOf(false) }
    var signedIn by remember { mutableStateOf(false) }
    var vaults by remember { mutableStateOf<List<RemoteVaultRow>>(emptyList()) }
    var chosen by remember { mutableStateOf<String?>(null) }
    var packageSecret by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }
    var summary by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var items by remember { mutableStateOf<List<SyncV4Engine.Item>>(emptyList()) }
    var picked by remember { mutableStateOf<Set<String>>(emptySet()) }
    var askingPurge by remember { mutableStateOf(false) }

    suspend fun reloadItems(current: SyncV4Link) {
        items = runCatching {
            withContext(Dispatchers.IO) { SyncV4Engine(context, store, current).items() }
        }.getOrDefault(emptyList())
    }

    // Everything in the list acts on the ticked rows in one pass, like the
    // desktop page: one engine, one save, then the list is rebuilt.
    fun act(block: (SyncV4Engine) -> Unit) {
        val current = link ?: return
        if (busy || SyncV4Run.running) return
        busy = true
        error = null
        scope.launch {
            try {
                val updated = withContext(Dispatchers.IO) {
                    val engine = SyncV4Engine(context, store, current)
                    block(engine)
                    engine.link
                }
                link = updated
                reloadItems(updated)
                picked = picked.filter { id -> items.any { it.recordId == id } }.toSet()
                GalleryCache.clear()
            } catch (failure: Throwable) {
                fail(failure)
            } finally {
                busy = false
            }
        }
    }

    // Only ask the relay whether it takes signups once the URL is well formed.
    LaunchedEffect(server) {
        if (!SyncPolicy.validServer(server)) {
            registrationOpen = false
            return@LaunchedEffect
        }
        registrationOpen = withContext(Dispatchers.IO) {
            runCatching { SyncV4Client(server).registrationOpen() }.getOrDefault(false)
        }
    }

    fun fetchVaults(client: SyncV4Client): List<RemoteVaultRow> {
        val body = client.vaults()
        val array = body.optJSONArray("vaults") ?: JSONArray()
        val listed = ArrayList<RemoteVaultRow>()
        for (index in 0 until array.length()) {
            val entry = array.optJSONObject(index) ?: continue
            listed.add(
                RemoteVaultRow(
                    id = entry.optString("id"),
                    name = entry.optString("name"),
                    records = entry.optInt("records"),
                    bytes = entry.optDouble("bytes", 0.0).toLong(),
                ),
            )
        }
        return listed
    }

    fun fail(failure: Throwable) {
        error = failure.message ?: "Sync failed"
        status = ""
    }

    fun signIn() {
        if (busy) return
        busy = true
        error = null
        status = "Contacting the server…"
        scope.launch {
            try {
                val found = withContext(Dispatchers.IO) {
                    val client = SyncV4Client(server)
                    client.protocol()
                    if (creating) client.register(email, password) else client.login(email, password)
                    fetchVaults(client)
                }
                vaults = found
                chosen = found.firstOrNull()?.id
                signedIn = true
                status = ""
            } catch (failure: Throwable) {
                fail(failure)
            } finally {
                busy = false
            }
        }
    }

    fun connect() {
        val vault = vaults.firstOrNull { it.id == chosen } ?: return
        if (packageSecret.isBlank()) {
            error = "Type this vault's password: it seals the packages."
            return
        }
        if (busy) return
        busy = true
        error = null
        status = "Issuing a device token…"
        scope.launch {
            try {
                val fresh = withContext(Dispatchers.IO) {
                    val client = SyncV4Client(server)
                    client.login(email, password)
                    val token = client.issueToken("phone")
                    val created = SyncV4Link(
                        server = server,
                        email = email,
                        token = token,
                        vaultId = vault.id,
                        vaultName = vault.name,
                        packageSecret = packageSecret,
                        cursor = 0,
                        mappings = emptyList(),
                    )
                    store.save(created)
                    created
                }
                link = fresh
                status = ""
            } catch (failure: Throwable) {
                fail(failure)
            } finally {
                busy = false
            }
        }
    }

    fun syncNow() {
        val current = link ?: return
        if (busy || SyncV4Run.running) return
        error = null
        // The pass runs in the process, not in this screen, so opening a video
        // or leaving the page does not interrupt it.
        SyncV4Run.start(context, store, current) { updated, text ->
            link = updated
            summary = text
            status = ""
            scope.launch { reloadItems(updated) }
            GalleryCache.clear()
        }
    }

    fun disconnect() {
        scope.launch {
            withContext(Dispatchers.IO) { store.clear() }
            link = null
            signedIn = false
            vaults = emptyList()
            chosen = null
            packageSecret = ""
            password = ""
            summary = ""
            error = null
            items = emptyList()
            picked = emptySet()
        }
    }

    LaunchedEffect(link?.token) {
        link?.let { reloadItems(it) }
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(Mvp.window)
            .safeDrawingPadding()
            .imePadding()
            .navigationBarsPadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "‹",
                color = Mvp.accent,
                fontSize = 26.sp,
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .clickable(onClick = onClose)
                    .padding(horizontal = 8.dp, vertical = 2.dp),
            )
            Spacer(Modifier.width(4.dp))
            Text("Sync", color = Mvp.title, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
        }

        val current = link
        when {
            current != null -> ConnectedCard(
                link = current,
                summary = summary,
                status = status,
                busy = busy,
                items = items,
                picked = picked,
                running = SyncV4Run.running,
                progressLabel = SyncV4Run.progress?.label.orEmpty(),
                askingPurge = askingPurge,
                onToggle = { id ->
                    picked = if (picked.contains(id)) picked - id else picked + id
                },
                onSelectAll = {
                    picked = if (picked.size == items.size) emptySet()
                    else items.map { it.recordId }.toSet()
                },
                onKeep = {
                    val ids = items.filter { it.recordId in picked && it.state != "synced" }
                        .map { it.recordId }
                    act { engine -> engine.downloadItems(ids) }
                },
                onRemove = {
                    val ids = items.filter { it.recordId in picked && it.state == "synced" }
                        .map { it.recordId }
                    act { engine -> engine.removeFromDevice(ids) }
                },
                onRecycle = {
                    val ids = items.filter { it.recordId in picked }.map { it.recordId }
                    act { engine -> engine.tombstoneItems(ids) }
                },
                onAskPurge = { askingPurge = true },
                onCancelPurge = { askingPurge = false },
                onConfirmPurge = {
                    askingPurge = false
                    val ids = items.filter { it.recordId in picked }.map { it.recordId }
                    act { engine -> engine.purgeItems(ids) }
                },
                onSync = { syncNow() },
                onDisconnect = { disconnect() },
            )

            !signedIn -> SignInCard(
                server = server,
                email = email,
                password = password,
                creating = creating,
                registrationOpen = registrationOpen,
                busy = busy,
                status = status,
                onServer = { server = it },
                onEmail = { email = it },
                onPassword = { password = it },
                onToggleMode = {
                    creating = !creating
                    error = null
                },
                onSubmit = { signIn() },
            )

            else -> VaultCard(
                vaults = vaults,
                chosen = chosen,
                packageSecret = packageSecret,
                busy = busy,
                status = status,
                onChoose = { chosen = it },
                onSecret = { packageSecret = it },
                onConnect = { connect() },
            )
        }

        MvpError(error)
    }
}

@Composable
private fun SignInCard(
    server: String,
    email: String,
    password: String,
    creating: Boolean,
    registrationOpen: Boolean,
    busy: Boolean,
    status: String,
    onServer: (String) -> Unit,
    onEmail: (String) -> Unit,
    onPassword: (String) -> Unit,
    onToggleMode: () -> Unit,
    onSubmit: () -> Unit,
) {
    MvpCard {
        Text(
            if (creating) "Create an account" else "Sign in to sync",
            color = Mvp.title,
            fontSize = 17.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "Your items are sealed on this phone; the server only stores opaque packages.",
            color = Mvp.description,
            fontSize = 13.sp,
            modifier = Modifier.padding(top = 4.dp, bottom = 14.dp),
        )
        SectionLabel("Server")
        MvpInput(server, onServer, "https://ciscox.ir", enabled = !busy)
        Spacer(Modifier.height(10.dp))
        SectionLabel("Email")
        MvpInput(email, onEmail, "you@example.com", enabled = !busy)
        Spacer(Modifier.height(10.dp))
        SectionLabel("Password")
        MvpInput(password, onPassword, "Account password", isPassword = true, enabled = !busy)
        Spacer(Modifier.height(16.dp))
        MvpButton(
            text = if (creating) "Create account" else "Sign in",
            onClick = onSubmit,
            modifier = Modifier.fillMaxWidth(),
            primary = true,
            enabled = !busy && server.isNotBlank() && email.isNotBlank() && password.isNotEmpty(),
        )
        if (registrationOpen) {
            Text(
                if (creating) "I already have an account" else "No account yet? Create one",
                color = Mvp.accent,
                fontSize = 13.sp,
                modifier = Modifier
                    .padding(top = 12.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .clickable(enabled = !busy, onClick = onToggleMode)
                    .padding(horizontal = 4.dp, vertical = 4.dp),
            )
        }
        if (status.isNotBlank()) {
            Text(status, color = Mvp.description, fontSize = 13.sp, modifier = Modifier.padding(top = 10.dp))
        }
    }
}

@Composable
private fun VaultCard(
    vaults: List<RemoteVaultRow>,
    chosen: String?,
    packageSecret: String,
    busy: Boolean,
    status: String,
    onChoose: (String) -> Unit,
    onSecret: (String) -> Unit,
    onConnect: () -> Unit,
) {
    MvpCard {
        Text("Choose a vault", color = Mvp.title, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
        Text(
            "The vault password seals every package; use the same one as your other device.",
            color = Mvp.description,
            fontSize = 13.sp,
            modifier = Modifier.padding(top = 4.dp, bottom = 10.dp),
        )
        if (vaults.isEmpty()) {
            Text("This account has no vaults yet.", color = Mvp.description, fontSize = 13.sp)
        }
        vaults.forEach { vault ->
            val selected = vault.id == chosen
            Row(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .background(if (selected) Mvp.selection else Mvp.glass)
                    .clickable(enabled = !busy) { onChoose(vault.id) }
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(vault.name, color = Mvp.title, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                    Text(vault.detail, color = Mvp.description, fontSize = 12.sp)
                }
                if (selected) Text("✓", color = Mvp.accent, fontSize = 15.sp)
            }
            Spacer(Modifier.height(6.dp))
        }
        Spacer(Modifier.height(8.dp))
        SectionLabel("Vault password")
        MvpInput(
            packageSecret,
            onSecret,
            "The password this vault was created with",
            isPassword = true,
            enabled = !busy,
        )
        Spacer(Modifier.height(16.dp))
        MvpButton(
            text = "Connect",
            onClick = onConnect,
            modifier = Modifier.fillMaxWidth(),
            primary = true,
            enabled = !busy && chosen != null && packageSecret.isNotBlank(),
        )
        if (status.isNotBlank()) {
            Text(status, color = Mvp.description, fontSize = 13.sp, modifier = Modifier.padding(top = 10.dp))
        }
    }
}

@Composable
private fun ConnectedCard(
    link: SyncV4Link,
    summary: String,
    status: String,
    busy: Boolean,
    items: List<SyncV4Engine.Item>,
    picked: Set<String>,
    running: Boolean,
    progressLabel: String,
    askingPurge: Boolean,
    onToggle: (String) -> Unit,
    onSelectAll: () -> Unit,
    onKeep: () -> Unit,
    onRemove: () -> Unit,
    onRecycle: () -> Unit,
    onAskPurge: () -> Unit,
    onCancelPurge: () -> Unit,
    onConfirmPurge: () -> Unit,
    onSync: () -> Unit,
    onDisconnect: () -> Unit,
) {
    MvpCard {
        Text(
            link.vaultName.ifBlank { "Vault" },
            color = Mvp.title,
            fontSize = 17.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            "${link.server} · ${link.email}",
            color = Mvp.description,
            fontSize = 12.sp,
            modifier = Modifier.padding(top = 4.dp),
        )
        Row(
            Modifier.fillMaxWidth().padding(top = 2.dp, bottom = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                "${items.size} item${if (items.size == 1) "" else "s"} in this vault",
                color = Mvp.description,
                fontSize = 12.sp,
                modifier = Modifier.weight(1f),
            )
            Text(
                if (picked.size == items.size && items.isNotEmpty()) "Clear" else "Select all",
                color = Mvp.accent,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .clickable(enabled = items.isNotEmpty()) { onSelectAll() }
                    .padding(horizontal = 6.dp, vertical = 4.dp),
            )
        }

        if (items.isEmpty()) {
            Text(
                "Nothing here yet. Run a sync to list what the vault holds.",
                color = Mvp.description,
                fontSize = 13.sp,
                modifier = Modifier.padding(bottom = 10.dp),
            )
        }
        items.take(200).forEach { item ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(bottom = 4.dp)
                    .clip(RoundedCornerShape(10.dp))
                    .background(if (item.recordId in picked) Mvp.selection else Mvp.glass)
                    .clickable { onToggle(item.recordId) }
                    .padding(horizontal = 6.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Checkbox(
                    checked = item.recordId in picked,
                    onCheckedChange = { onToggle(item.recordId) },
                    colors = CheckboxDefaults.colors(checkedColor = Mvp.accent, uncheckedColor = Mvp.description),
                )
                Column(Modifier.weight(1f)) {
                    Text(
                        item.name,
                        color = Mvp.title,
                        fontSize = 14.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        itemStateLabel(item) + if (item.path.contains('/')) " · ${item.path.substringBeforeLast('/')}" else "",
                        color = Mvp.description,
                        fontSize = 11.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }

        Spacer(Modifier.height(8.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            MvpButton(
                text = "Keep here",
                onClick = onKeep,
                modifier = Modifier.weight(1f),
                enabled = !busy && !running && items.any { it.recordId in picked && it.state != "synced" },
            )
            MvpButton(
                text = "Remove here",
                onClick = onRemove,
                modifier = Modifier.weight(1f),
                enabled = !busy && !running && items.any { it.recordId in picked && it.state == "synced" },
            )
        }
        Spacer(Modifier.height(8.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            MvpButton(
                text = "Recycle",
                onClick = onRecycle,
                modifier = Modifier.weight(1f),
                enabled = !busy && !running && picked.isNotEmpty(),
            )
            MvpButton(
                text = "Delete forever",
                onClick = onAskPurge,
                modifier = Modifier.weight(1f),
                enabled = !busy && !running && picked.isNotEmpty(),
            )
        }
        if (askingPurge) {
            Text(
                if (picked.size == 1) "Delete 1 item permanently? It cannot be restored."
                else "Delete ${picked.size} items permanently? They cannot be restored.",
                color = Color(0xFFFF6B6B),
                fontSize = 13.sp,
                modifier = Modifier.padding(top = 10.dp),
            )
            Row(Modifier.fillMaxWidth().padding(top = 6.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MvpButton(text = "Cancel", onClick = onCancelPurge, modifier = Modifier.weight(1f))
                MvpButton(text = "Delete", onClick = onConfirmPurge, modifier = Modifier.weight(1f), primary = true)
            }
        }

        Spacer(Modifier.height(14.dp))
        MvpButton(
            text = if (running) "Syncing…" else "Sync now",
            onClick = onSync,
            modifier = Modifier.fillMaxWidth(),
            primary = true,
            enabled = !busy && !running,
        )
        if (running && progressLabel.isNotBlank()) {
            Text(
                progressLabel,
                color = Mvp.accent,
                fontSize = 12.sp,
                modifier = Modifier.padding(top = 8.dp),
            )
        }
        Text(
            "Sync keeps running when you leave this page or play something.",
            color = Mvp.description,
            fontSize = 12.sp,
            modifier = Modifier.padding(top = 6.dp),
        )
        Spacer(Modifier.height(8.dp))
        MvpButton(text = "Disconnect", onClick = onDisconnect, modifier = Modifier.fillMaxWidth(), enabled = !busy)
        if (status.isNotBlank()) {
            Text(status, color = Mvp.accent, fontSize = 13.sp, modifier = Modifier.padding(top = 12.dp))
        }
        if (summary.isNotBlank()) {
            Text(summary, color = Mvp.description, fontSize = 13.sp, modifier = Modifier.padding(top = 6.dp))
        }
    }
}

/** Where one row stands, in the words the screen uses. */
private fun itemStateLabel(item: SyncV4Engine.Item): String = when (item.state) {
    "synced" -> "On this phone"
    "remote" -> "Only in the cloud"
    else -> "Waiting to download"
}
