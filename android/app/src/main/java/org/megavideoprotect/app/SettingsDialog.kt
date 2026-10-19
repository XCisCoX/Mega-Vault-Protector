package org.megavideoprotect.app

import android.content.Intent
import android.graphics.drawable.ColorDrawable
import android.net.Uri
import android.os.Build
import android.view.WindowManager
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

private const val TAB_UPDATES = 0
private const val TAB_APPEARANCE = 1
private const val TAB_PLUGINS = 2
private const val TAB_SECURITY = 3
private const val TAB_LIBRARY = 4

/**
 * Phone settings. Same shape as the desktop build: grouped cards, one section
 * per tab, and switches that save the moment they move.
 */
@Composable
fun SettingsDialog(
    currentTags: List<TagEntry>,
    remote: Boolean = false,
    onDismiss: () -> Unit,
    onChanged: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var tab by remember { mutableStateOf(TAB_UPDATES) }
    var idleLabel by remember { mutableStateOf(VaultLocation.idleLockLabel(context)) }
    var current by remember { mutableStateOf("") }
    var newPass by remember { mutableStateOf("") }
    var confirm by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var ok by remember { mutableStateOf<String?>(null) }
    var newTag by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var tagError by remember { mutableStateOf<String?>(null) }

    var checkAutomatically by remember { mutableStateOf(ThemePrefs.checkAutomatically(context)) }
    var downloadAutomatically by remember { mutableStateOf(ThemePrefs.downloadAutomatically(context)) }
    var includeBeta by remember { mutableStateOf(ThemePrefs.includeBeta(context)) }
    var updateStatus by remember { mutableStateOf<String?>(null) }
    var checking by remember { mutableStateOf(false) }
    var themeMode by remember { mutableStateOf(ThemePrefs.theme(context)) }
    var pluginPath by remember { mutableStateOf(File(context.filesDir, "plugins")) }
    var pluginNote by remember { mutableStateOf<String?>(null) }

    fun changePassword() {
        if (newPass.length < 8) { error = "New password must be at least 8 characters."; return }
        if (newPass != confirm) { error = "New passwords do not match."; return }
        error = null; ok = null; busy = true
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeChangePassword(current, newPass, 256 * 1024, 3, 1))
            }
            busy = false
            if (res.ok) {
                ok = "Password changed."
                current = ""; newPass = ""; confirm = ""
            } else error = res.error.ifBlank { res.detail }
        }
    }

    fun addTag() {
        if (newTag.isBlank()) return
        tagError = null
        scope.launch {
            val res = withContext(Dispatchers.Default) {
                CoreBridge.Result.parse(CoreBridge.nativeCreateTag(newTag.trim()))
            }
            if (res.ok) { newTag = ""; onChanged() } else tagError = res.error.ifBlank { res.detail }
        }
    }

    fun deleteTag(id: Long) {
        scope.launch {
            withContext(Dispatchers.Default) { CoreBridge.Result.parse(CoreBridge.nativeDeleteTag(id)) }
            onChanged()
        }
    }

    fun checkNow() {
        if (checking) return
        checking = true
        updateStatus = "Checking for updates…"
        scope.launch {
            val release = withContext(Dispatchers.IO) {
                runCatching { checkForUpdate(context, force = true) }.getOrNull()
            }
            checking = false
            updateStatus = when {
                release == null -> "You’re up to date · Version ${installedVersion(context)}"
                else -> "Version ${release.version} is available. Use the update button in the library."
            }
        }
    }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false),
    ) {
        val view = LocalView.current
        SideEffect {
            val window = (view.parent as? DialogWindowProvider)?.window ?: return@SideEffect
            window.setDimAmount(0.55f)
            window.setBackgroundDrawable(ColorDrawable(android.graphics.Color.TRANSPARENT))
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                window.addFlags(WindowManager.LayoutParams.FLAG_BLUR_BEHIND)
                window.setBackgroundBlurRadius(70)
            }
        }
        val sheet = remember { Animatable(0f) }
        LaunchedEffect(Unit) { sheet.animateTo(1f, tween(240)) }
        Box(Modifier.fillMaxSize()) {
            Box(
                Modifier
                    .fillMaxSize()
                    .clickable(
                        interactionSource = remember { MutableInteractionSource() },
                        indication = null,
                        onClick = onDismiss,
                    ),
            )
            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .graphicsLayer {
                        alpha = sheet.value
                        translationY = (1f - sheet.value) * 56.dp.toPx()
                    }
                    .fillMaxWidth()
                    .imePadding()
                    .clip(RoundedCornerShape(topStart = 22.dp, topEnd = 22.dp))
                    .background(Mvp.window)
                    .clickable(
                        interactionSource = remember { MutableInteractionSource() },
                        indication = null,
                    ) {}
                    .navigationBarsPadding(),
            ) {
                Box(
                    Modifier
                        .align(Alignment.CenterHorizontally)
                        .padding(top = 8.dp)
                        .size(width = 36.dp, height = 4.dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(Mvp.description.copy(alpha = 0.35f)),
                )
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(start = 20.dp, end = 12.dp, top = 6.dp, bottom = 2.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Settings", color = Mvp.title, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
                    Spacer(Modifier.weight(1f))
                    Text(
                        "Done",
                        color = Mvp.accent,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier
                            .clickable(onClick = onDismiss)
                            .padding(horizontal = 8.dp, vertical = 8.dp),
                    )
                }
                SettingsTabs(selected = tab, onSelect = { tab = it })
                Column(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(max = 560.dp)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 16.dp)
                        .padding(top = 14.dp)
                        .padding(bottom = 20.dp),
                    verticalArrangement = Arrangement.spacedBy(18.dp),
                ) {
                    when (tab) {
                        TAB_UPDATES -> {
                            Text(
                                "Keep Mega Vault up to date, on your terms.",
                                color = Mvp.description,
                                fontSize = 13.sp,
                            )
                            SettingsBlock(title = "Automatic") {
                                SettingsSwitchRow(
                                    title = "Check for updates automatically",
                                    detail = null,
                                    checked = checkAutomatically,
                                    onCheckedChange = {
                                        checkAutomatically = it
                                        ThemePrefs.setCheckAutomatically(context, it)
                                    },
                                )
                                Hairline()
                                SettingsSwitchRow(
                                    title = "Download updates automatically",
                                    detail = "Never installs without your confirmation.",
                                    checked = downloadAutomatically,
                                    onCheckedChange = {
                                        downloadAutomatically = it
                                        ThemePrefs.setDownloadAutomatically(context, it)
                                    },
                                )
                                Hairline()
                                SettingsSwitchRow(
                                    title = "Include beta releases",
                                    detail = "Try previews; they may be less stable.",
                                    checked = includeBeta,
                                    onCheckedChange = {
                                        includeBeta = it
                                        ThemePrefs.setIncludeBeta(context, it)
                                    },
                                )
                            }
                            SettingsBlock(title = "Check now") {
                                Row(
                                    Modifier.fillMaxWidth(),
                                    verticalAlignment = Alignment.CenterVertically,
                                ) {
                                    Column(Modifier.weight(1f).padding(horizontal = 16.dp, vertical = 12.dp)) {
                                        Text(
                                            "Installed version ${installedVersion(context)}",
                                            color = Mvp.text,
                                            fontSize = 14.sp,
                                        )
                                        updateStatus?.let {
                                            Text(it, color = Mvp.description, fontSize = 12.sp)
                                        }
                                    }
                                    Text(
                                        if (checking) "Checking…" else "Check",
                                        color = if (checking) Mvp.placeholder else Mvp.accent,
                                        fontSize = 15.sp,
                                        fontWeight = FontWeight.SemiBold,
                                        modifier = Modifier
                                            .clickable(enabled = !checking) { checkNow() }
                                            .padding(horizontal = 16.dp, vertical = 12.dp),
                                    )
                                }
                            }
                        }

                        TAB_APPEARANCE -> {
                            Text(
                                "Choose a look for the whole app.",
                                color = Mvp.description,
                                fontSize = 13.sp,
                            )
                            SettingsBlock(title = "Theme") {
                                ThemeSegments(
                                    selected = themeMode,
                                    onSelect = {
                                        themeMode = it
                                        ThemePrefs.setTheme(context, it)
                                    },
                                )
                            }
                            Text(
                                if (themeMode == ThemeMode.SYSTEM) {
                                    "System follows your phone’s light or dark setting."
                                } else {
                                    "Your choice is saved for the next launch."
                                },
                                color = Mvp.description,
                                fontSize = 12.sp,
                            )
                        }

                        TAB_PLUGINS -> {
                            Text(
                                "A future home for external extensions.",
                                color = Mvp.description,
                                fontSize = 13.sp,
                            )
                            SettingsBlock(title = "Plugins") {
                                Column(Modifier.fillMaxWidth().padding(16.dp)) {
                                    Text(
                                        "External plugin loading is not supported yet",
                                        color = Mvp.text,
                                        fontSize = 15.sp,
                                        fontWeight = FontWeight.SemiBold,
                                    )
                                    Spacer(Modifier.height(4.dp))
                                    Text(
                                        "This version has no plugin loader and no plugin API. Files in the folder below are not loaded or executed.",
                                        color = Mvp.description,
                                        fontSize = 13.sp,
                                    )
                                    Spacer(Modifier.height(10.dp))
                                    Text(
                                        pluginPath.absolutePath,
                                        color = Mvp.description,
                                        fontSize = 12.sp,
                                    )
                                    Spacer(Modifier.height(8.dp))
                                    Text(
                                        "Create folder",
                                        color = Mvp.accent,
                                        fontSize = 15.sp,
                                        fontWeight = FontWeight.SemiBold,
                                        modifier = Modifier
                                            .clip(RoundedCornerShape(8.dp))
                                            .clickable {
                                                val created = runCatching {
                                                    val dir = File(context.filesDir, "plugins")
                                                    if (!dir.isDirectory) dir.mkdirs()
                                                    dir.isDirectory
                                                }.getOrDefault(false)
                                                pluginPath = File(context.filesDir, "plugins")
                                                pluginNote = if (created) {
                                                    "Folder is ready. Nothing in it is loaded by this version."
                                                } else {
                                                    "The folder could not be created."
                                                }
                                            }
                                            .padding(vertical = 6.dp),
                                    )
                                    pluginNote?.let {
                                        Text(it, color = Mvp.description, fontSize = 12.sp)
                                    }
                                }
                            }
                        }

                        TAB_SECURITY -> {
                            Text(
                                "Protect your vault and control when it locks.",
                                color = Mvp.description,
                                fontSize = 13.sp,
                            )
                            SettingsBlock(title = "Auto-lock") {
                                VaultLocation.idleChoices.forEachIndexed { index, (ms, label) ->
                                    if (index > 0) Hairline()
                                    Row(
                                        Modifier
                                            .fillMaxWidth()
                                            .clickable {
                                                VaultLocation.saveIdleLock(context, ms)
                                                idleLabel = label
                                            }
                                            .padding(horizontal = 16.dp, vertical = 14.dp),
                                        verticalAlignment = Alignment.CenterVertically,
                                    ) {
                                        Text(label, color = Mvp.text, fontSize = 16.sp, modifier = Modifier.weight(1f))
                                        if (idleLabel == label) {
                                            Icon(
                                                painterResource(R.drawable.msg_check_s),
                                                contentDescription = null,
                                                tint = Mvp.accent,
                                                modifier = Modifier.size(18.dp),
                                            )
                                        }
                                    }
                                }
                            }
                            if (remote) {
                                Text(
                                    "This phone is using the PC vault. Change the password on the PC.",
                                    color = Mvp.description,
                                    fontSize = 13.sp,
                                )
                            } else {
                                SettingsBlock(title = "Password") {
                                    SettingsField(current, { current = it }, "Current password", password = true)
                                    Hairline()
                                    SettingsField(newPass, { newPass = it }, "New password", password = true)
                                    Hairline()
                                    SettingsField(confirm, { confirm = it }, "Confirm new password", password = true)
                                    if (!error.isNullOrBlank() || !ok.isNullOrBlank()) {
                                        Hairline()
                                        Text(
                                            error ?: ok.orEmpty(),
                                            color = if (error != null) Mvp.errorText else Mvp.accent,
                                            fontSize = 13.sp,
                                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                                        )
                                    }
                                    Hairline()
                                    Box(
                                        Modifier
                                            .fillMaxWidth()
                                            .clickable(enabled = !busy) { changePassword() }
                                            .padding(vertical = 14.dp),
                                        contentAlignment = Alignment.Center,
                                    ) {
                                        Text(
                                            if (busy) "Changing…" else "Change password",
                                            color = if (busy) Mvp.placeholder else Mvp.accent,
                                            fontSize = 16.sp,
                                            fontWeight = FontWeight.SemiBold,
                                        )
                                    }
                                }
                            }
                        }

                        else -> {
                            Text(
                                "Organize tags used by the library filter.",
                                color = Mvp.description,
                                fontSize = 13.sp,
                            )
                            if (remote) {
                                Text(
                                    "This phone is using the PC vault. Manage tags on the PC.",
                                    color = Mvp.description,
                                    fontSize = 13.sp,
                                )
                            } else {
                                SettingsBlock(title = "Tags") {
                                    Column(
                                        Modifier
                                            .fillMaxWidth()
                                            .heightIn(max = 220.dp)
                                            .verticalScroll(rememberScrollState()),
                                    ) {
                                        if (currentTags.isEmpty()) {
                                            Text(
                                                "No tags yet",
                                                color = Mvp.description,
                                                fontSize = 15.sp,
                                                modifier = Modifier.padding(horizontal = 16.dp, vertical = 14.dp),
                                            )
                                        }
                                        currentTags.forEachIndexed { index, tag ->
                                            if (index > 0) Hairline()
                                            Row(
                                                Modifier
                                                    .fillMaxWidth()
                                                    .padding(start = 16.dp, end = 8.dp),
                                                verticalAlignment = Alignment.CenterVertically,
                                            ) {
                                                Text(
                                                    tag.name,
                                                    color = Mvp.text,
                                                    fontSize = 16.sp,
                                                    modifier = Modifier.weight(1f),
                                                )
                                                Text("${tag.videoCount}", color = Mvp.description, fontSize = 14.sp)
                                                Spacer(Modifier.width(4.dp))
                                                Text(
                                                    "Remove",
                                                    color = Mvp.destructive,
                                                    fontSize = 14.sp,
                                                    modifier = Modifier
                                                        .clickable(enabled = !busy) { deleteTag(tag.id) }
                                                        .padding(horizontal = 8.dp, vertical = 14.dp),
                                                )
                                            }
                                        }
                                    }
                                    if (!tagError.isNullOrBlank()) {
                                        Hairline()
                                        Text(
                                            tagError.orEmpty(),
                                            color = Mvp.errorText,
                                            fontSize = 13.sp,
                                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                                        )
                                    }
                                    Hairline()
                                    Row(
                                        Modifier.fillMaxWidth(),
                                        verticalAlignment = Alignment.CenterVertically,
                                    ) {
                                        Box(Modifier.weight(1f)) {
                                            SettingsField(newTag, { newTag = it }, "New tag")
                                        }
                                        Text(
                                            "Add",
                                            color = if (newTag.isBlank() || busy) Mvp.placeholder else Mvp.accent,
                                            fontSize = 16.sp,
                                            fontWeight = FontWeight.SemiBold,
                                            modifier = Modifier
                                                .clickable(enabled = newTag.isNotBlank() && !busy) { addTag() }
                                                .padding(horizontal = 16.dp, vertical = 14.dp),
                                        )
                                    }
                                }
                            }
                        }
                    }

                    Text(
                        "github.com/XCisCoX/Mega-Vault-Protector",
                        color = Mvp.description,
                        fontSize = 12.sp,
                        textAlign = TextAlign.Center,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable {
                                context.startActivity(
                                    Intent(
                                        Intent.ACTION_VIEW,
                                        Uri.parse("https://github.com/XCisCoX/Mega-Vault-Protector"),
                                    )
                                )
                            }
                            .padding(top = 4.dp, bottom = 8.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingsTabs(selected: Int, onSelect: (Int) -> Unit) {
    val labels = listOf("Updates", "Appearance", "Plugins", "Security", "Library")
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp)
            .heightIn(min = 42.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        labels.forEachIndexed { index, label ->
            val active = selected == index
            val color by animateColorAsState(
                if (active) Mvp.accent else Mvp.description,
                tween(160),
                label = "settingsTab",
            )
            Column(
                Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .clickable { onSelect(index) }
                    .padding(horizontal = 8.dp, vertical = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    label,
                    color = color,
                    fontSize = 13.sp,
                    fontWeight = if (active) FontWeight.SemiBold else FontWeight.Normal,
                )
                Spacer(Modifier.height(4.dp))
                Box(
                    Modifier
                        .height(2.dp)
                        .width(if (active) 18.dp else 0.dp)
                        .clip(RoundedCornerShape(1.dp))
                        .background(Mvp.accent),
                )
            }
        }
    }
}

@Composable
private fun ThemeSegments(selected: ThemeMode, onSelect: (ThemeMode) -> Unit) {
    val modes = listOf(ThemeMode.SYSTEM to "System", ThemeMode.LIGHT to "Light", ThemeMode.DARK to "Dark")
    Row(
        Modifier
            .fillMaxWidth()
            .padding(10.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(Mvp.itemHover)
            .padding(3.dp),
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        modes.forEach { (mode, label) ->
            val active = mode == selected
            Box(
                Modifier
                    .weight(1f)
                    .height(44.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(if (active) Mvp.primary else Color.Transparent)
                    .clickable { onSelect(mode) },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    label,
                    color = if (active) Color.White else Mvp.text,
                    fontSize = 15.sp,
                    fontWeight = if (active) FontWeight.SemiBold else FontWeight.Normal,
                )
            }
        }
    }
}

@Composable
private fun SettingsSwitchRow(
    title: String,
    detail: String?,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable { onCheckedChange(!checked) }
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, color = Mvp.text, fontSize = 16.sp)
            if (detail != null) {
                Spacer(Modifier.height(2.dp))
                Text(detail, color = Mvp.description, fontSize = 12.sp)
            }
        }
        Spacer(Modifier.width(12.dp))
        MvpSwitch(checked = checked)
    }
}

/** iOS-style pill switch: a track and a sliding thumb, not a square tick box. */
@Composable
private fun MvpSwitch(checked: Boolean) {
    val thumb by animateDpAsState(if (checked) 20.dp else 2.dp, tween(160), label = "switchThumb")
    val track by animateColorAsState(
        if (checked) Mvp.primary else Mvp.description.copy(alpha = 0.35f),
        tween(160),
        label = "switchTrack",
    )
    Box(
        Modifier
            .width(46.dp)
            .height(28.dp)
            .clip(CircleShape)
            .background(track),
    ) {
        Box(
            Modifier
                .offset(x = thumb, y = 2.dp)
                .size(24.dp)
                .clip(CircleShape)
                .background(Color.White),
        )
    }
}

@Composable
private fun Hairline() {
    Box(
        Modifier
            .fillMaxWidth()
            .padding(start = 16.dp)
            .height(1.dp)
            .background(Mvp.description.copy(alpha = 0.18f)),
    )
}

@Composable
private fun SettingsBlock(title: String, content: @Composable () -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            title,
            color = Mvp.description,
            fontSize = 13.sp,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.padding(start = 4.dp),
        )
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(14.dp))
                .background(Mvp.itemHover),
        ) {
            content()
        }
    }
}

@Composable
private fun SettingsField(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    password: Boolean = false,
) {
    BasicTextField(
        value = value,
        onValueChange = onValueChange,
        singleLine = true,
        textStyle = TextStyle(color = Mvp.text, fontSize = 16.sp),
        visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
        keyboardOptions = if (password) KeyboardOptions(
            keyboardType = KeyboardType.Password,
            autoCorrectEnabled = false,
        ) else KeyboardOptions.Default,
        cursorBrush = SolidColor(Mvp.accent),
        modifier = Modifier.fillMaxWidth(),
        decorationBox = { inner ->
            Box(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 14.dp),
                contentAlignment = Alignment.CenterStart,
            ) {
                if (value.isEmpty()) {
                    Text(placeholder, color = Mvp.placeholder, fontSize = 16.sp)
                }
                inner()
            }
        },
    )
}
