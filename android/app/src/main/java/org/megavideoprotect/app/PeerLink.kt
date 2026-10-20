package org.megavideoprotect.app

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.URL
import java.net.URLEncoder
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLSocketFactory

/**
 * Swap with PC. The phone scans the PC's share code (`mvpvault://host:port?fp=…&k=…`),
 * the person at the PC accepts, and then each side sees the other's library as a
 * read-only folder it can copy from.
 *
 * The phone cannot accept connections, so it keeps a couple of requests waiting
 * on the PC for work (the PC browsing or copying this phone's library) and posts
 * each answer back. This vault stays open on the phone the whole time. Locking
 * the phone only hides the library; the swap lasts until someone disconnects
 * or the PC stops sharing.
 */
object PeerLink {
    private class Session(
        val base: String,
        val host: String,
        val token: String,
        val factory: SSLSocketFactory,
        val verifier: HostnameVerifier,
    )

    private class Answer(val code: Int, val body: ByteArray?)

    private const val POLLERS = 2
    private const val MAX_CHUNK = 1 shl 20

    @Volatile private var session: Session? = null
    private var scope: CoroutineScope? = null
    private val main = Handler(Looper.getMainLooper())

    /** Names of this phone's items, from the last listing the PC asked for. */
    @Volatile private var names: Map<Long, String> = emptyMap()

    /** The PC's name while swapped. The library shows a PC folder while it is set. */
    var pcName by mutableStateOf<String?>(null)
        private set

    /** Why the last swap ended on its own, shown once in the library. */
    var endedReason by mutableStateOf<String?>(null)

    fun linked(): Boolean = session != null

    fun deviceName(context: Context): String {
        val named = runCatching {
            Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
        }.getOrNull()
        if (!named.isNullOrBlank()) return named.trim()
        val model = Build.MODEL.orEmpty()
        val maker = Build.MANUFACTURER.orEmpty().replaceFirstChar { it.uppercase() }
        return if (model.startsWith(maker, ignoreCase = true)) model else "$maker $model".trim()
    }

    /**
     * Null when the PC accepted. Otherwise a sentence for the library. Blocks
     * until the person at the PC answers (up to about a minute).
     */
    fun link(raw: String, deviceName: String): String? {
        val uri = runCatching { Uri.parse(raw.trim()) }.getOrNull()
            ?: return "That code is not a PC share."
        if (uri.scheme != "mvpvault") return "That code is not a PC share."
        val host = uri.host ?: return "That code has no address."
        val port = uri.port
        if (port <= 0) return "That code has no port."
        val fingerprint = uri.getQueryParameter("fp")?.trim()?.lowercase().orEmpty()
        if (!isHex64(fingerprint)) {
            return "That code has no certificate. Open Share on the PC and scan the new one."
        }
        val key = uri.getQueryParameter("k")?.trim()?.lowercase().orEmpty()
        if (!isHex64(key)) {
            return "This PC needs the latest Mega Vault Protect to swap libraries."
        }
        val base = "https://$host:$port"
        val factory = RemoteVault.pinnedFactory(fingerprint)
        val verifier = HostnameVerifier { hostname, _ -> hostname == host }
        val name = URLEncoder.encode(deviceName.take(48), "UTF-8")
        val conn = (AppProxy.open(URL("$base/v1/link?k=$key&name=$name")) as HttpsURLConnection).apply {
            sslSocketFactory = factory
            hostnameVerifier = verifier
            requestMethod = "POST"
            doOutput = true
            setFixedLengthStreamingMode(0)
            connectTimeout = 8000
            readTimeout = 90000
            instanceFollowRedirects = false
            useCaches = false
        }
        return try {
            conn.outputStream.use { }
            when (conn.responseCode) {
                200 -> {
                    val json = JSONObject(conn.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8))
                    val token = json.optString("token")
                    if (token.isBlank()) {
                        "The PC did not accept the swap."
                    } else {
                        val pc = json.optString("name").ifBlank { host }
                        begin(Session(base, host, token, factory, verifier), pc)
                        null
                    }
                }
                401 -> "That code is out of date. Open Share on the PC and scan it again."
                403 -> "The PC declined the swap."
                409 -> "Unlock Mega Vault Protect on the PC first."
                else -> "The PC refused the swap. Open Share on the PC and try again."
            }
        } catch (error: Exception) {
            if (error.message.orEmpty().contains("certificate", ignoreCase = true)) {
                "The PC certificate does not match the code. Scan it again."
            } else {
                "Could not reach the PC. Same Wi-Fi, and allow the app through the firewall."
            }
        } finally {
            conn.disconnect()
        }
    }

    /** Ends the swap from this side. The PC drops its phone folder too. */
    fun unlink() {
        val ending = synchronized(this) {
            val current = session
            session = null
            scope?.cancel()
            scope = null
            current
        } ?: return
        names = emptyMap()
        main.post { pcName = null }
        Thread { runCatching { send(ending, "POST", "/v1/link/end", ByteArray(0), 8000) } }.start()
    }

    // ---- Reading the PC's library ------------------------------------------

    fun getText(path: String): String? = getBytes(path)?.let { String(it, Charsets.UTF_8) }

    fun getBytes(path: String): ByteArray? {
        val current = session ?: return null
        val answer = send(current, "GET", path, null, 60000)
        if (answer.code == 401 || answer.code == 403) end(current, "The PC ended the swap.")
        return if (answer.code == 200) answer.body else null
    }

    fun readRange(id: Long, offset: Long, size: Int): ByteArray? {
        val ask = size.coerceIn(1, MAX_CHUNK)
        return getBytes("/v1/read?id=$id&offset=$offset&size=$ask")
    }

    // ---- Answering the PC ---------------------------------------------------

    private fun begin(next: Session, pc: String) {
        val previous = synchronized(this) {
            val old = session
            scope?.cancel()
            session = next
            val work = CoroutineScope(SupervisorJob() + Dispatchers.IO)
            scope = work
            repeat(POLLERS) { work.launch { serve(next) } }
            old
        }
        if (previous != null) {
            Thread { runCatching { send(previous, "POST", "/v1/link/end", ByteArray(0), 8000) } }.start()
        }
        main.post {
            endedReason = null
            pcName = pc
        }
    }

    private fun end(ending: Session, reason: String) {
        synchronized(this) {
            if (session !== ending) return
            session = null
            scope?.cancel()
            scope = null
        }
        names = emptyMap()
        main.post {
            pcName = null
            endedReason = reason
        }
    }

    private suspend fun serve(current: Session) {
        var failures = 0
        while (kotlin.coroutines.coroutineContext.isActive && session === current) {
            val answer = send(current, "GET", "/v1/link/next", null, 40000)
            when (answer.code) {
                200 -> {
                    failures = 0
                    answer.body?.let { handle(current, it) }
                }
                204 -> failures = 0
                401, 403 -> {
                    end(current, "The PC ended the swap.")
                    return
                }
                else -> {
                    failures++
                    if (failures >= 6) {
                        end(current, "Lost the connection to the PC. Swap again to reconnect.")
                        return
                    }
                    delay(2000)
                }
            }
        }
    }

    private fun handle(current: Session, raw: ByteArray) {
        val request = runCatching { JSONObject(String(raw, Charsets.UTF_8)) }.getOrNull() ?: return
        val id = request.optLong("req", -1L)
        if (id < 0L) return
        val (status, body) = try {
            answer(request)
        } catch (error: Throwable) {
            500 to ByteArray(0)
        }
        send(current, "POST", "/v1/link/reply?req=$id&status=$status", body, 30000)
    }

    /** The PC only ever reads. Nothing here changes this phone's vault. */
    private fun answer(request: JSONObject): Pair<Int, ByteArray> {
        if (!runCatching { CoreBridge.nativeIsUnlocked() }.getOrDefault(false)) {
            return 403 to "locked".toByteArray()
        }
        val id = request.optLong("id", -1L)
        return when (request.optString("op")) {
            "folders" -> 200 to CoreBridge.nativeListFolders().toByteArray(Charsets.UTF_8)
            "videos" -> {
                val raw = CoreBridge.nativeListVideos()
                names = runCatching { Json.videos(raw).associate { it.id to it.name } }.getOrDefault(emptyMap())
                200 to raw.toByteArray(Charsets.UTF_8)
            }
            "thumb" -> {
                val max = request.optInt("max", 320).coerceIn(64, 720)
                thumbnail(id, max)?.let { 200 to it } ?: (404 to ByteArray(0))
            }
            "size" -> 200 to CoreBridge.nativeVideoSize(id).toString().toByteArray()
            "read" -> {
                val offset = request.optLong("offset", -1L)
                val size = request.optInt("size", 0)
                if (id <= 0L || offset < 0L || size <= 0) {
                    400 to ByteArray(0)
                } else {
                    CoreBridge.nativeReadRange(id, offset, size.coerceAtMost(MAX_CHUNK))
                        ?.let { 200 to it } ?: (404 to ByteArray(0))
                }
            }
            else -> 404 to ByteArray(0)
        }
    }

    private fun thumbnail(id: Long, max: Int): ByteArray? {
        if (id <= 0L) return null
        // The gallery decodes photos itself; the core probe is for movies and songs.
        if (names[id]?.let { isImageName(it) } == true) {
            val bitmap = decodeVaultImage(id, max) ?: return null
            val out = ByteArrayOutputStream()
            bitmap.compress(Bitmap.CompressFormat.JPEG, 85, out)
            return out.toByteArray()
        }
        return CoreBridge.nativeThumbnail(id, max)?.takeIf { it.isNotEmpty() }
    }

    private fun send(current: Session, method: String, path: String, body: ByteArray?, readTimeoutMs: Int): Answer {
        val conn = try {
            (AppProxy.open(URL(current.base + path)) as HttpsURLConnection).apply {
                sslSocketFactory = current.factory
                hostnameVerifier = current.verifier
                requestMethod = method
                setRequestProperty("Authorization", "Bearer ${current.token}")
                connectTimeout = 15000
                readTimeout = readTimeoutMs
                instanceFollowRedirects = false
                useCaches = false
                if (body != null) {
                    doOutput = true
                    setRequestProperty("Content-Type", "application/octet-stream")
                    setFixedLengthStreamingMode(body.size)
                }
            }
        } catch (error: Exception) {
            return Answer(-1, null)
        }
        return try {
            if (body != null) conn.outputStream.use { it.write(body) }
            val code = conn.responseCode
            Answer(code, if (code == 200) conn.inputStream.use { it.readBytes() } else null)
        } catch (error: Exception) {
            Answer(-1, null)
        } finally {
            conn.disconnect()
        }
    }

    private fun isHex64(text: String): Boolean =
        text.length == 64 && text.all { it in '0'..'9' || it in 'a'..'f' }
}
