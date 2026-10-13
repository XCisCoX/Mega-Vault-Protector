package org.megavideoprotect.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/** A newer GitHub release than the copy that is installed. */
data class AppRelease(val version: String, val apkUrl: String, val size: Long)

private const val RELEASES = "https://api.github.com/repos/XCisCoX/Mega-Video-Protector/releases/latest"

fun installedVersion(context: Context): String {
    return runCatching {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName
    }.getOrNull().orEmpty().ifBlank { "0.0.0" }
}

/** Null when this install is already the latest release, or GitHub cannot be reached. */
fun newerRelease(current: String): AppRelease? {
    val connection = (URL(RELEASES).openConnection() as HttpURLConnection).apply {
        connectTimeout = 8000
        readTimeout = 8000
        setRequestProperty("Accept", "application/vnd.github+json")
        setRequestProperty("User-Agent", "MegaVaultProtect")
    }
    val body = connection.inputStream.bufferedReader().use { it.readText() }
    if (connection.responseCode != 200) return null
    val json = JSONObject(body)
    val tag = json.optString("tag_name").removePrefix("v")
    if (!isNewerVersion(tag, current)) return null
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
