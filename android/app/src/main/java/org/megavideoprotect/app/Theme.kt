package org.megavideoprotect.app

import android.app.Activity
import android.content.Context
import android.graphics.drawable.ColorDrawable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

/**
 * One palette's worth of colours. The dark instance is the original Telegram
 * night glass; the light instance keeps the same structure (panels, hairlines,
 * blue actions) on a bright field, like the desktop build's Light theme.
 */
data class MvpPalette(
    val window: Color,
    val card: Color,
    val cardBorder: Color,
    val glass: Color,
    val glassStroke: Color,
    val inputBg: Color,
    val inputBorder: Color,
    val inputFocusBorder: Color,
    val title: Color,
    val description: Color,
    val text: Color,
    val placeholder: Color,
    val button: Color,
    val buttonBorder: Color,
    val buttonHover: Color,
    val buttonDisabled: Color,
    val buttonDisabledText: Color,
    val primary: Color,
    val primaryBorder: Color,
    val primaryHover: Color,
    val accent: Color,
    val selection: Color,
    val itemHover: Color,
    val alternateRow: Color,
    val headerBg: Color,
    val headerText: Color,
    val errorText: Color,
    val errorBg: Color,
    val errorBorder: Color,
    val sliderGroove: Color,
    val sliderFill: Color,
    val sliderHandle: Color,
    val sliderHandleBorder: Color,
    val scrollbarHandle: Color,
    val statusText: Color,
    val destructive: Color,
    /** Backing behind folder tiles, thumbnails and media placeholders. */
    val tileBg: Color,
    /** Labels sitting on a tile that is always painted with tileBg. */
    val tileText: Color,
    /** Full-bleed dimming for sheets and drag previews. */
    val scrim: Color,
    /** Quiet progress groove on cards (transfers, imports). */
    val progressTrack: Color,
    /** Frosted field behind the library's top/bottom chrome (haze bars). */
    val barBg: Color,
    /** Extra tint layered into the haze blur of those bars. */
    val barTint: Color,
    /** Labels painted on a bar filled with barBg — never plain white in light. */
    val barText: Color,
)

val MvpDarkPalette = MvpPalette(
    window = Color(0xFF000000),
    card = Color(0xF21C1C1E),
    cardBorder = Color(0x24FFFFFF),
    glass = Color(0x0EFFFFFF),
    glassStroke = Color(0x1CFFFFFF),
    inputBg = Color(0xFF161618),
    inputBorder = Color(0x1FFFFFFF),
    inputFocusBorder = Color(0xFF3390EC),
    title = Color(0xFFFFFFFF),
    description = Color(0xFF8E8E93),
    text = Color(0xFFF2F2F7),
    placeholder = Color(0xFF6D6D72),
    button = Color(0x1AFFFFFF),
    buttonBorder = Color(0x24FFFFFF),
    buttonHover = Color(0x26FFFFFF),
    buttonDisabled = Color(0xFF1C1C1E),
    buttonDisabledText = Color(0xFF636366),
    primary = Color(0xFF3390EC),
    primaryBorder = Color(0xFF3390EC),
    primaryHover = Color(0xFF4BA0F5),
    accent = Color(0xFF3390EC),
    selection = Color(0x6E3390EC),
    itemHover = Color(0xFF1C1C1E),
    alternateRow = Color(0xFF101012),
    headerBg = Color(0xE6101012),
    headerText = Color(0xFF8E8E93),
    errorText = Color(0xFFFF6B6B),
    errorBg = Color(0xFF2C1518),
    errorBorder = Color(0xFF5C2A30),
    sliderGroove = Color(0x33FFFFFF),
    sliderFill = Color(0xFF3390EC),
    sliderHandle = Color(0xFFFFFFFF),
    sliderHandleBorder = Color(0xFF3390EC),
    scrollbarHandle = Color(0xFF3A3A3C),
    statusText = Color(0xFF8E8E93),
    destructive = Color(0xFFFF453A),
    tileBg = Color(0xFF161618),
    tileText = Color(0xFFDCDCE0),
    scrim = Color(0xC4101014),
    progressTrack = Color(0xFF2A2A2E),
    barBg = Color(0xC4101014),
    barTint = Color(0x47000000),
    barText = Color(0xDBFFFFFF),
)

val MvpLightPalette = MvpPalette(
    window = Color(0xFFF2F2F7),
    card = Color(0xFFFFFFFF),
    cardBorder = Color(0x14000000),
    glass = Color(0xB3FFFFFF),
    glassStroke = Color(0x1F000000),
    inputBg = Color(0xFFFFFFFF),
    inputBorder = Color(0x1F000000),
    inputFocusBorder = Color(0xFF3390EC),
    title = Color(0xFF000000),
    description = Color(0xFF6D6D72),
    text = Color(0xFF1C1C1E),
    placeholder = Color(0xFF8E8E93),
    button = Color(0x0F000000),
    buttonBorder = Color(0x1F000000),
    buttonHover = Color(0x1A000000),
    buttonDisabled = Color(0xFFEBEBF0),
    buttonDisabledText = Color(0xFF8E8E93),
    primary = Color(0xFF3390EC),
    primaryBorder = Color(0xFF3390EC),
    primaryHover = Color(0xFF4BA0F5),
    accent = Color(0xFF2A7CD0),
    selection = Color(0x4D3390EC),
    itemHover = Color(0xFFE9E9EE),
    alternateRow = Color(0xFFF7F7FA),
    headerBg = Color(0xE6F2F2F7),
    headerText = Color(0xFF6D6D72),
    errorText = Color(0xFFC62828),
    errorBg = Color(0xFFFFEBEE),
    errorBorder = Color(0xFFF0BDC3),
    sliderGroove = Color(0x33000000),
    sliderFill = Color(0xFF3390EC),
    sliderHandle = Color(0xFFFFFFFF),
    sliderHandleBorder = Color(0xFF3390EC),
    scrollbarHandle = Color(0xFFC7C7CC),
    statusText = Color(0xFF6D6D72),
    destructive = Color(0xFFD70015),
    tileBg = Color(0xFFEDEDF1),
    tileText = Color(0xFF3C3C43),
    scrim = Color(0x99000000),
    progressTrack = Color(0xFFE5E5EA),
    barBg = Color(0xE8F5F5F8),
    barTint = Color(0x59FFFFFF),
    barText = Color(0xFF1C1C1E),
)

/** System is the default so a phone already in light mode is not forced dark. */
enum class ThemeMode { SYSTEM, LIGHT, DARK }

object ThemePrefs {
    private const val FILE = "mvp_preferences"
    private const val KEY_THEME = "appearance/theme"
    private const val KEY_CHECK = "updates/checkAutomatically"
    private const val KEY_DOWNLOAD = "updates/downloadAutomatically"
    private const val KEY_BETA = "updates/includeBeta"
    private const val KEY_LAST_CHECK = "updates/lastCheckUtcMs"
    private const val KEY_LAST_CHANNEL = "updates/lastCheckBeta"
    private const val KEY_RELEASE_VERSION = "updates/foundVersion"
    private const val KEY_RELEASE_URL = "updates/foundUrl"
    private const val KEY_RELEASE_SIZE = "updates/foundSize"

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    fun theme(context: Context): ThemeMode = when (
        prefs(context).getString(KEY_THEME, "system")
    ) {
        "light" -> ThemeMode.LIGHT
        "dark" -> ThemeMode.DARK
        else -> ThemeMode.SYSTEM
    }

    fun setTheme(context: Context, mode: ThemeMode) {
        prefs(context).edit()
            .putString(
                KEY_THEME,
                when (mode) {
                    ThemeMode.LIGHT -> "light"
                    ThemeMode.DARK -> "dark"
                    ThemeMode.SYSTEM -> "system"
                },
            )
            .apply()
        MvpThemeState.mode = mode
    }

    fun checkAutomatically(context: Context): Boolean =
        prefs(context).getBoolean(KEY_CHECK, true)

    fun downloadAutomatically(context: Context): Boolean =
        prefs(context).getBoolean(KEY_DOWNLOAD, false)

    fun includeBeta(context: Context): Boolean = prefs(context).getBoolean(KEY_BETA, false)

    fun setCheckAutomatically(context: Context, value: Boolean) =
        prefs(context).edit().putBoolean(KEY_CHECK, value).apply()

    fun setDownloadAutomatically(context: Context, value: Boolean) =
        prefs(context).edit().putBoolean(KEY_DOWNLOAD, value).apply()

    fun setIncludeBeta(context: Context, value: Boolean) =
        prefs(context).edit().putBoolean(KEY_BETA, value).apply()

    fun lastCheckMs(context: Context): Long = prefs(context).getLong(KEY_LAST_CHECK, 0L)

    fun lastCheckBeta(context: Context): Boolean = prefs(context).getBoolean(KEY_LAST_CHANNEL, false)

    /**
     * The release the last successful check found. Remembering it is what keeps
     * the update button visible: a throttled or offline check must return the
     * answer we already have instead of pretending there is nothing to install.
     */
    fun rememberedRelease(context: Context): AppRelease? {
        val stored = prefs(context)
        val version = stored.getString(KEY_RELEASE_VERSION, null) ?: return null
        val url = stored.getString(KEY_RELEASE_URL, null) ?: return null
        return AppRelease(version, url, stored.getLong(KEY_RELEASE_SIZE, 0L))
    }

    fun rememberRelease(context: Context, release: AppRelease?) {
        val edit = prefs(context).edit()
        if (release == null) {
            edit.remove(KEY_RELEASE_VERSION).remove(KEY_RELEASE_URL).remove(KEY_RELEASE_SIZE)
        } else {
            edit.putString(KEY_RELEASE_VERSION, release.version)
                .putString(KEY_RELEASE_URL, release.apkUrl)
                .putLong(KEY_RELEASE_SIZE, release.size)
        }
        edit.apply()
    }

    fun recordCheck(context: Context, beta: Boolean, atMs: Long) {
        prefs(context).edit()
            .putLong(KEY_LAST_CHECK, atMs)
            .putBoolean(KEY_LAST_CHANNEL, beta)
            .apply()
    }
}

/**
 * The live mode, read by every palette colour. Changing it here recomposes
 * anything that painted with Mvp.*; no restart and no manual plumbing.
 */
object MvpThemeState {
    var mode by mutableStateOf(ThemeMode.SYSTEM)
    var systemDark by mutableStateOf(true)
    private var loaded = false

    fun loadOnce(context: Context) {
        if (loaded) return
        loaded = true
        mode = ThemePrefs.theme(context)
    }

    val dark: Boolean
        get() = when (mode) {
            ThemeMode.DARK -> true
            ThemeMode.LIGHT -> false
            ThemeMode.SYSTEM -> systemDark
        }

    val palette: MvpPalette
        get() = if (dark) MvpDarkPalette else MvpLightPalette
}

/**
 * Telegram night glass, or its light twin. Every call site keeps reading
 * Mvp.window / Mvp.text / ... — the getters follow the saved theme.
 */
object Mvp {
    private val p: MvpPalette get() = MvpThemeState.palette
    val window: Color get() = p.window
    val card: Color get() = p.card
    val cardBorder: Color get() = p.cardBorder
    val tileBg: Color get() = p.tileBg
    val tileText: Color get() = p.tileText
    val scrim: Color get() = p.scrim
    val progressTrack: Color get() = p.progressTrack
    val barBg: Color get() = p.barBg
    val barTint: Color get() = p.barTint
    val barText: Color get() = p.barText
    val glass: Color get() = p.glass
    val glassStroke: Color get() = p.glassStroke
    val inputBg: Color get() = p.inputBg
    val inputBorder: Color get() = p.inputBorder
    val inputFocusBorder: Color get() = p.inputFocusBorder
    val title: Color get() = p.title
    val description: Color get() = p.description
    val text: Color get() = p.text
    val placeholder: Color get() = p.placeholder
    val button: Color get() = p.button
    val buttonBorder: Color get() = p.buttonBorder
    val buttonHover: Color get() = p.buttonHover
    val buttonDisabled: Color get() = p.buttonDisabled
    val buttonDisabledText: Color get() = p.buttonDisabledText
    val primary: Color get() = p.primary
    val primaryBorder: Color get() = p.primaryBorder
    val primaryHover: Color get() = p.primaryHover
    val accent: Color get() = p.accent
    val selection: Color get() = p.selection
    val itemHover: Color get() = p.itemHover
    val alternateRow: Color get() = p.alternateRow
    val headerBg: Color get() = p.headerBg
    val headerText: Color get() = p.headerText
    val errorText: Color get() = p.errorText
    val errorBg: Color get() = p.errorBg
    val errorBorder: Color get() = p.errorBorder
    val sliderGroove: Color get() = p.sliderGroove
    val sliderFill: Color get() = p.sliderFill
    val sliderHandle: Color get() = p.sliderHandle
    val sliderHandleBorder: Color get() = p.sliderHandleBorder
    val scrollbarHandle: Color get() = p.scrollbarHandle
    val statusText: Color get() = p.statusText
    val destructive: Color get() = p.destructive

    /** True while the rendered palette is the dark one. */
    val isDark: Boolean get() = MvpThemeState.dark
}

@Composable
fun mvpColorScheme() = if (MvpThemeState.dark) {
    darkColorScheme(
        primary = Mvp.primary,
        onPrimary = Color.White,
        background = Mvp.window,
        onBackground = Mvp.title,
        surface = Mvp.card,
        onSurface = Mvp.text,
        surfaceVariant = Mvp.inputBg,
        onSurfaceVariant = Mvp.description,
        outline = Mvp.cardBorder,
        outlineVariant = Mvp.inputBorder,
        error = Mvp.errorText,
        onError = Color.White,
        errorContainer = Mvp.errorBg,
        onErrorContainer = Mvp.errorText,
    )
} else {
    lightColorScheme(
        primary = Mvp.primary,
        onPrimary = Color.White,
        background = Mvp.window,
        onBackground = Mvp.title,
        surface = Mvp.card,
        onSurface = Mvp.text,
        surfaceVariant = Mvp.inputBg,
        onSurfaceVariant = Mvp.description,
        outline = Mvp.cardBorder,
        outlineVariant = Mvp.inputBorder,
        error = Mvp.errorText,
        onError = Color.White,
        errorContainer = Mvp.errorBg,
        onErrorContainer = Mvp.errorText,
    )
}

/**
 * Root theme. Reads the saved choice once, follows the system only in System
 * mode, and republishes it so every Mvp.* read refreshes immediately.
 */
@Composable
fun MvpTheme(content: @Composable () -> Unit) {
    val systemDark = isSystemInDarkTheme()
    // Loaded once by the activity before the first frame; calling it again here
    // only re-checks the flag, never mutates during composition.
    SideEffect { MvpThemeState.systemDark = systemDark }
    // The window and the system bar icons follow the palette too. Without this a
    // light page sat under white status-bar glyphs (invisible) and a dark
    // windowBackground (#0c0f15 from themes.xml) showed through every gap.
    val view = LocalView.current
    SideEffect {
        val window = (view.context as? Activity)?.window ?: return@SideEffect
        window.setBackgroundDrawable(ColorDrawable(Mvp.window.toArgb()))
        WindowCompat.getInsetsController(window, view).apply {
            isAppearanceLightStatusBars = !MvpThemeState.dark
            isAppearanceLightNavigationBars = !MvpThemeState.dark
        }
    }
    MaterialTheme(colorScheme = mvpColorScheme(), content = content)
}
