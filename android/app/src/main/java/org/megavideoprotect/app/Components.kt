package org.megavideoprotect.app

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.graphics.drawable.ColorDrawable
import android.os.Build
import android.view.WindowManager
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.material3.TextButton
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider

/** Frosted panel: dark glass, hairline, large radius. */
@Composable
fun MvpCard(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(22.dp))
            .background(Mvp.card)
            .border(1.dp, Mvp.glassStroke, RoundedCornerShape(22.dp))
            .padding(horizontal = 18.dp, vertical = 18.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content
    )
}

/** A settings group: one glass sheet, rows stacked with hairlines. */
@Composable
fun GlassGroup(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(Mvp.glass)
            .border(1.dp, Mvp.glassStroke, RoundedCornerShape(16.dp)),
        content = content,
    )
}

@Composable
fun SectionLabel(text: String) {
    Text(
        text,
        color = Mvp.description,
        fontSize = 13.sp,
        fontWeight = FontWeight.Medium,
        modifier = Modifier.padding(start = 4.dp),
    )
}

/** Pill used for tags. Selected pills are the blue action color. */
@Composable
fun GlassChip(
    text: String,
    selected: Boolean,
    onClick: () -> Unit,
    enabled: Boolean = true,
) {
    Text(
        text,
        color = if (selected) Color.White else Mvp.text,
        fontSize = 14.sp,
        modifier = Modifier
            .clip(RoundedCornerShape(20.dp))
            .background(if (selected) Mvp.accent else Mvp.glass)
            .border(1.dp, if (selected) Mvp.accent else Mvp.glassStroke, RoundedCornerShape(20.dp))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 8.dp),
    )
}

/** Page title: #f3f6fa, 25sp, weight 600 (QLabel#pageTitle). */
@Composable
fun MvpTitle(text: String) {
    Text(text, color = Mvp.title, fontSize = 25.sp, fontWeight = FontWeight.SemiBold)
}

/** Description: #9eabba, 13sp (QLabel#description). */
@Composable
fun MvpDescription(text: String) {
    Text(text, color = Mvp.description, fontSize = 13.sp)
}

/** Input replicating QLineEdit (bg #10151d, border #364154, radius 8, focus #6f8cff). */
@Composable
fun MvpInput(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    isPassword: Boolean = false,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    keyboardType: KeyboardType = KeyboardType.Text,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier.fillMaxWidth(),
        placeholder = { Text(placeholder, color = Mvp.placeholder, fontSize = 14.sp) },
        visualTransformation = if (isPassword) PasswordVisualTransformation() else VisualTransformation.None,
        singleLine = true,
        enabled = enabled,
        textStyle = TextStyle(color = Mvp.title, fontSize = 14.sp),
        // Masking only changes rendering; the IME must also know this is a password.
        keyboardOptions = if (isPassword) KeyboardOptions(
            keyboardType = KeyboardType.Password,
            autoCorrectEnabled = false,
        ) else KeyboardOptions(keyboardType = keyboardType, autoCorrectEnabled = keyboardType == KeyboardType.Text),
        shape = RoundedCornerShape(14.dp),
        colors = OutlinedTextFieldDefaults.colors(
            focusedTextColor = Mvp.title,
            unfocusedTextColor = Mvp.title,
            disabledTextColor = Mvp.title.copy(alpha = 0.6f),
            cursorColor = Mvp.accent,
            focusedBorderColor = Mvp.inputFocusBorder,
            unfocusedBorderColor = Mvp.inputBorder,
            disabledBorderColor = Mvp.inputBorder.copy(alpha = 0.6f),
            focusedContainerColor = Mvp.inputBg,
            unfocusedContainerColor = Mvp.inputBg,
            disabledContainerColor = Mvp.inputBg,
            focusedPlaceholderColor = Mvp.placeholder,
            unfocusedPlaceholderColor = Mvp.placeholder,
            disabledPlaceholderColor = Mvp.placeholder,
        ),
    )
}

/** Capsule button. Primary is Telegram blue; the rest are glass. */
@Composable
fun MvpButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    primary: Boolean = false,
    enabled: Boolean = true,
) {
    val bg = when {
        !enabled -> Mvp.buttonDisabled
        primary -> Mvp.primary
        else -> Mvp.button
    }
    val border = when {
        !enabled -> Mvp.buttonDisabled
        primary -> Mvp.primaryBorder
        else -> Mvp.buttonBorder
    }
    val fg = when {
        !enabled -> Mvp.buttonDisabledText
        primary -> Color.White
        else -> Mvp.title
    }
    Button(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier.height(44.dp),
        shape = RoundedCornerShape(22.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = bg,
            contentColor = fg,
            disabledContainerColor = Mvp.buttonDisabled,
            disabledContentColor = Mvp.buttonDisabledText,
        ),
        border = BorderStroke(1.dp, border),
        contentPadding = PaddingValues(horizontal = 16.dp),
    ) {
        Text(text, fontSize = 14.sp, fontWeight = if (primary) FontWeight.SemiBold else FontWeight.Normal)
    }
}

/** Error banner replicating QLabel#errorLabel (#ff9a9a on #321d24, 1px #6a303a, radius 7). */
@Composable
fun MvpError(text: String?, modifier: Modifier = Modifier) {
    if (text.isNullOrBlank()) return
    Text(
        text,
        color = Mvp.errorText,
        fontSize = 13.sp,
        modifier = modifier
            .fillMaxWidth()
            .background(Mvp.errorBg, RoundedCornerShape(7.dp))
            .border(1.dp, Mvp.errorBorder, RoundedCornerShape(7.dp))
            .padding(9.dp),
    )
}

/** Dropdown replicating QComboBox (bg #10151d, border #364154, radius 8). */
@Composable
fun MvpCombo(
    selected: String,
    items: List<String>,
    onSelect: (String) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        MvpButton(text = selected, onClick = { open = true }, enabled = enabled)
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            items.forEach { item ->
                DropdownMenuItem(
                    text = { Text(item, color = Mvp.text, fontSize = 13.sp) },
                    onClick = { open = false; onSelect(item) },
                )
            }
        }
    }
}

internal const val ChooseFolder = "Choose a folder…"

/** App-private and shared storage, with a folder picker as the other mode. */
@Composable
fun VaultPlaceField(
    location: String,
    options: List<String>,
    enabled: Boolean,
    onLocation: (String) -> Unit,
    onChooseFolder: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val standard = options.distinct()
    MvpCombo(
        selected = VaultLocation.shortName(context, location),
        items = standard.map { VaultLocation.shortName(context, it) } + ChooseFolder,
        onSelect = { label ->
            if (label == ChooseFolder) {
                onChooseFolder()
            } else {
                val index = standard.indexOfFirst { VaultLocation.shortName(context, it) == label }
                if (index >= 0) onLocation(standard[index])
            }
        },
        enabled = enabled,
        modifier = modifier.fillMaxWidth(),
    )
}

/** The scan mark, pinned to the top-right of the screen. */
@Composable
fun BoxScope.ScanCorner(enabled: Boolean, onClick: () -> Unit) {
    IconButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier
            .align(Alignment.TopEnd)
            .safeDrawingPadding()
            .padding(end = 4.dp),
    ) {
        Icon(
            painter = painterResource(R.drawable.profile_qr_scan_24),
            contentDescription = "Scan",
            tint = Mvp.title,
            modifier = Modifier.size(26.dp),
        )
    }
}

/** One toolbar action row (like the Qt gallery toolbar). */
@Composable
fun RowScope.ToolbarSpacer(weight: Float = 1f) {
    Spacer(Modifier.weight(weight))
}

@Composable
fun RowScope.ToolbarButton(
    text: String,
    onClick: () -> Unit,
    primary: Boolean = false,
) {
    Spacer(Modifier.width(4.dp))
    MvpButton(text = text, onClick = onClick, primary = primary, modifier = Modifier.weight(1f))
}

/**
 * Window-level look for every dialog: dimmed, and blurred behind on Android 12+
 * (the platform drops the blur silently on devices that do not support it).
 */
@Composable
fun BlurredDialogWindow(dim: Float = 0.55f) {
    val view = LocalView.current
    SideEffect {
        val window = (view.parent as? DialogWindowProvider)?.window ?: return@SideEffect
        window.setDimAmount(dim)
        window.setBackgroundDrawable(ColorDrawable(android.graphics.Color.TRANSPARENT))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            window.addFlags(WindowManager.LayoutParams.FLAG_BLUR_BEHIND)
            window.setBackgroundBlurRadius(70)
        }
    }
}

/**
 * The one dialog surface of the app: a bottom sheet over the blurred library,
 * matching the desktop's in-window sheets. Content scrolls, buttons stay put.
 */
@Composable
fun MvpSheet(
    title: String,
    onDismiss: () -> Unit,
    confirmLabel: String,
    onConfirm: (() -> Unit)? = null,
    confirmEnabled: Boolean = true,
    dismissLabel: String = "Cancel",
    content: @Composable ColumnScope.() -> Unit,
) {
    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false),
    ) {
        BlurredDialogWindow()
        val slide = remember { Animatable(0f) }
        LaunchedEffect(Unit) { slide.animateTo(1f, tween(220)) }
        Box(Modifier.fillMaxSize()) {
            Box(
                Modifier.fillMaxSize().clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                    onClick = onDismiss,
                ),
            )
            Column(
                Modifier
                    .align(Alignment.BottomCenter)
                    .graphicsLayer {
                        alpha = slide.value
                        translationY = (1f - slide.value) * 48.dp.toPx()
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
                Text(
                    title,
                    color = Mvp.text,
                    fontSize = 17.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp),
                )
                Column(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(max = 420.dp)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 20.dp, vertical = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                    content = content,
                )
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TextButton(onClick = onDismiss) {
                        Text(dismissLabel, color = Mvp.description, fontSize = 14.sp)
                    }
                    if (onConfirm != null) {
                        Spacer(Modifier.width(8.dp))
                        Button(
                            onClick = onConfirm,
                            enabled = confirmEnabled,
                            shape = RoundedCornerShape(10.dp),
                            colors = ButtonDefaults.buttonColors(
                                containerColor = Mvp.accent,
                                contentColor = Color.White,
                            ),
                        ) { Text(confirmLabel, fontSize = 14.sp, fontWeight = FontWeight.SemiBold) }
                    }
                }
            }
        }
    }
}
