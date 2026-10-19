package org.megavideoprotect.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import org.json.JSONArray
import org.json.JSONObject
import java.net.URI
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/** A newer GitHub release than the copy that is installed. */
data class AppRelease(val version: String, val apkUrl: String, val size: Long)

private const val RELEASES = "https://api.github.com/repos/XCisCoX/Mega-Video-Protector/releases/latest"
private const val RELEASES_ALL = "https://api.github.com/repos/XCisCoX/Mega-Video-Protector/releases?per_page=30"
private const val CHECK_INTERVAL_MS = 6L * 60L * 60L * 1000L

fun installedVersion(context: Context): String {
    return runCatching {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName
    }.getOrNull().orEmpty().ifBlank { "0.0.0" }
}

private fun fetchText(target: String): Pair<String, Int>? {
    val connection = (URL(target).openConnection() as HttpURLConnection).apply {
        connectTimeout = 8000
        readTimeout = 8000
        setRequestProperty("Accept", "application/vnd.github+json")
        setRequestProperty("User-Agent", "MegaVaultProtect")
    }
    return runCatching {
        val status = connection.responseCode
        val body = if (status in 200..299) {
            connection.inputStream.bufferedReader().use { it.readText() }
        } else {
            connection.errorStream?.bufferedReader()?.use { it.readText() }.orEmpty()
        }
        body to status
    }.getOrNull()
}

/** The installable APK inside one release, or null when it carries none. */
private fun apkAsset(json: JSONObject): AppRelease? {
    val tag = json.optString("tag_name").removePrefix("v")
    val assets = json.optJSONArray("assets") ?: return null
    var url = ""
    var size = 0L
    for (i in 0 until assets.length()) {
        val asset = assets.getJSONObject(i)
        val name = asset.optString("name")
        if (name == "MegaVaultProtect-android-arm64.apk" || (url.isEmpty() && name.endsWith(".apk"))) {
            url = asset.optString("browser_download_url")
            size = asset.optLong("size")
            if (name == "MegaVaultProtect-android-arm64.apk") break
        }
    }
    if (url.isEmpty()) return null
    return AppRelease(tag, url, size)
}

/**
 * Picks the newest usable release out of either a single release object or a
 * list. Drafts never count; a beta needs `includeBeta`, and a stable release
 * always outranks a beta of the same numbers.
 */
fun selectRelease(body: String, current: String, includeBeta: Boolean): AppRelease? {
    val trimmed = body.trimStart()
    val releases = runCatching {
        if (trimmed.startsWith("[")) {
            val array = JSONArray(body)
            (0 until array.length()).mapNotNull { array.optJSONObject(it) }
        } else {
            listOf(JSONObject(body))
        }
    }.getOrNull() ?: return null
    var best: AppRelease? = null
    for (json in releases) {
        if (json.optBoolean("draft")) continue
        val tag = json.optString("tag_name").removePrefix("v")
        if (tag.isBlank()) continue
        val beta = json.optBoolean("prerelease") || tag.contains("-beta.", ignoreCase = true)
        if (beta && !includeBeta) continue
        if (!isNewerVersion(tag, current)) continue
        val candidate = apkAsset(json) ?: continue
        // A package from anywhere but GitHub must never be offered.
        val safe = runCatching {
            val uri = URI(candidate.apkUrl)
            uri.scheme == "https" &&
                (uri.host == "github.com" || uri.host == "objects.githubusercontent.com")
        }.getOrDefault(false)
        if (!safe) continue
        if (best == null || isNewerVersion(candidate.version, best.version)) best = candidate
    }
    return best
}

/** Null when this install is already the latest release, or GitHub cannot be reached. */
fun newerRelease(current: String, includeBeta: Boolean = false): AppRelease? =
    fetchRelease(current, includeBeta).second

/**
 * first = "GitHub answered", second = the release it named. A failed fetch and a
 * genuine "you are up to date" both return null, and they must not be treated
 * the same: only a real answer may clear what we remembered.
 */
private fun fetchRelease(current: String, includeBeta: Boolean): Pair<Boolean, AppRelease?> {
    val answer = fetchText(if (includeBeta) RELEASES_ALL else RELEASES) ?: return false to null
    val (body, status) = answer
    if (status !in 200..299) return false to null
    return true to selectRelease(body, current, includeBeta)
}

/**
 * The library's background check. Honours "check automatically" and only asks
 * GitHub again after six hours on the same channel, unless [force] is set by
 * the Settings screen's Check now.
 */
fun checkForUpdate(context: Context, force: Boolean = false): AppRelease? {
    val beta = ThemePrefs.includeBeta(context)
    val installed = installedVersion(context)
    if (!force) {
        if (!ThemePrefs.checkAutomatically(context)) return cachedRelease(context, installed)
        val last = ThemePrefs.lastCheckMs(context)
        val elapsed = System.currentTimeMillis() - last
        if (last > 0L && elapsed in 0 until CHECK_INTERVAL_MS &&
            ThemePrefs.lastCheckBeta(context) == beta
        ) {
            // Within the interval: answer from what the last real check found,
            // otherwise the update button disappears for six hours.
            return cachedRelease(context, installed)
        }
    }
    ThemePrefs.recordCheck(context, beta, System.currentTimeMillis())
    val (answered, found) = fetchRelease(installed, beta)
    if (!answered) return cachedRelease(context, installed)
    ThemePrefs.rememberRelease(context, found)
    return found
}

/** The last release a check found, while it is still newer than this install. */
fun cachedRelease(context: Context, installed: String = installedVersion(context)): AppRelease? =
    ThemePrefs.rememberedRelease(context)?.takeIf { isNewerVersion(it.version, installed) }

fun isNewerVersion(latest: String, current: String): Boolean {
    fun parts(text: String): List<Int> {
        val head = text.substringBefore('-')
        val numbers = head.split('.').map { it.toIntOrNull() ?: 0 }
        val beta = Regex("""beta\.(\d+)""").find(text)?.groupValues?.get(1)?.toIntOrNull()
        return listOf(
            numbers.getOrElse(0) { 0 },
            numbers.getOrElse(1) { 0 },
            numbers.getOrElse(2) { 0 },
            beta ?: 99,
        )
    }
    val left = parts(latest)
    val right = parts(current)
    for (i in 0..3) {
        if (left[i] != right[i]) return left[i] > right[i]
    }
    return false
}

fun downloadRelease(release: AppRelease, dest: File, onProgress: (Float) -> Unit) {
    val connection = (URL(release.apkUrl).openConnection() as HttpURLConnection).apply {
        instanceFollowRedirects = true
        connectTimeout = 15000
        readTimeout = 30000
        setRequestProperty("User-Agent", "MegaVaultProtect")
        setRequestProperty("Accept", "application/octet-stream")
    }
    val length = connection.contentLengthLong.takeIf { it > 0L } ?: release.size
    connection.inputStream.use { input ->
        dest.outputStream().use { output ->
            val buffer = ByteArray(64 * 1024)
            var sent = 0L
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                output.write(buffer, 0, n)
                sent += n
                if (length > 0L) onProgress((sent.toFloat() / length.toFloat()).coerceIn(0f, 1f))
            }
        }
    }
    if (connection.responseCode !in 200..299) error("The update could not be downloaded.")
}

/** Hands the downloaded APK to the system installer, which replaces this install. */
fun installRelease(context: Context, apk: File) {
    val installer = context.packageManager.packageInstaller
    val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
    val sessionId = installer.createSession(params)
    val session = installer.openSession(sessionId)
    session.openWrite("mvp", 0, apk.length()).use { output ->
        apk.inputStream().use { input -> input.copyTo(output) }
        session.fsync(output)
    }
    val intent = Intent(context, UpdateReceiver::class.java)
    val flags = if (android.os.Build.VERSION.SDK_INT >= 31) {
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
    } else {
        PendingIntent.FLAG_UPDATE_CURRENT
    }
    val pending = PendingIntent.getBroadcast(context, sessionId, intent, flags)
    session.commit(pending.intentSender)
    session.close()
}

class UpdateReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            val confirm = if (android.os.Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(Intent.EXTRA_INTENT)
            }
            confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            if (confirm != null) context.startActivity(confirm)
        }
    }
}
