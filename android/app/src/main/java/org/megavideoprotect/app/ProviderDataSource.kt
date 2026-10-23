@file:OptIn(androidx.media3.common.util.UnstableApi::class)
package org.megavideoprotect.app

import android.net.Uri
import androidx.media3.common.C
import androidx.media3.datasource.BaseDataSource
import androidx.media3.datasource.DataSpec
import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.PushbackInputStream
import java.net.URI

internal data class ProviderPart(val url: String, val size: Long = -1)
internal data class ProviderPlan(val stream: ProviderStream, val parts: List<ProviderPart>) {
    val size: Long get() = if (parts.all { it.size > 0 } && parts.sumOf { it.size } > 0) parts.sumOf { it.size } else -1
}

/** Host-owned, bounded VOD playlist resolution; encrypted/DRM/live/range playlists fail closed. */
internal object ProviderHls {
    fun plan(http: PluginHttp, stream: ProviderStream, measureParts: Boolean = false): ProviderPlan {
        if (!stream.hls) return ProviderPlan(stream, listOf(ProviderPart(stream.url)))
        var url = stream.url
        repeat(3) {
            val text = http.text(url)
            require(text.startsWith("#EXTM3U")) { "Invalid HLS playlist" }
            val lines = text.lines().map { it.trim() }.filter { it.isNotEmpty() }
            val variants = lines.indices.filter { lines[it].startsWith("#EXT-X-STREAM-INF:") }
            if (variants.isNotEmpty()) {
                url = URI(url).resolve(lines.getOrNull(variants.first() + 1) ?: error("Missing HLS variant")).toASCIIString()
            } else {
                val urls = parseMedia(url, text)
                val parts = urls.map { part -> ProviderPart(part, if (measureParts) runCatching { http.contentSize(part) }.getOrDefault(-1) else -1) }
                return ProviderPlan(stream.copy(codec = if (lines.any { it.startsWith("#EXT-X-MAP:") }) "m4a" else "ts", mime = ""), parts)
            }
        }
        error("HLS variant nesting limit exceeded")
    }
    internal fun parseMedia(url: String, text: String): List<String> {
        val lines = text.lines().map { it.trim() }.filter { it.isNotEmpty() }
        require(lines.firstOrNull() == "#EXTM3U" && "#EXT-X-ENDLIST" in lines) { "Only finite HLS tracks are supported" }
        require(lines.none { it.startsWith("#EXT-X-KEY:") && it != "#EXT-X-KEY:METHOD=NONE" } && lines.none { it.startsWith("#EXT-X-SESSION-KEY:") }) { "Encrypted or DRM HLS is not supported" }
        require(lines.none { it.startsWith("#EXT-X-BYTERANGE:") || it.startsWith("#EXT-X-DISCONTINUITY") || it.startsWith("#EXT-X-PART:") }) { "Unsupported HLS range/discontinuity playlist" }
        val maps = lines.filter { it.startsWith("#EXT-X-MAP:") }
        require(maps.size <= 1 && maps.none { "BYTERANGE=" in it }) { "Unsupported HLS initialization ranges" }
        val init = maps.map { Regex("URI=\"([^\"]+)\"").find(it)?.groupValues?.get(1) ?: error("Invalid HLS initialization") }
        val segments = lines.filter { !it.startsWith('#') }
        require(segments.size in 1..1000 && lines.count { it.startsWith("#EXTINF:") } == segments.size) { "Invalid or excessive HLS segments" }
        return (init + segments).map { URI(url).resolve(it).toASCIIString() }
    }
}

/** One bounded JNI chunk per append, no plaintext file, offset gaps abort instead of publishing. */
internal class ProviderImport(
    private val manifest: PluginManifest, private val item: PluginItem, private val root: String,
    private val folder: Long, private val stream: ProviderStream, private val report: (Boolean, String) -> Unit,
) : Closeable {
    private var handle = 0L
    private var next = 0L
    private var expected = -1L
    private var finished = false
    @Synchronized fun begin(position: Long, size: Long) {
        if (finished) return
        if (position != 0L || next != 0L) { fail("Seek interrupted the offline copy; retry Download from the start."); return }
        if (size <= 0) { fail("Unknown stream length: playback only; no offline copy was created."); return }
        expected = size
        val codec = stream.codec.takeIf { it in setOf("flac", "mp3", "m4a", "aac", "ogg", "opus", "wav", "ts") } ?: "mp3"
        val name = PluginAudio.fileName(item.title, item.subtitle).removeSuffix(".flac") + ".$codec"
        val result = CoreBridge.Result.parse(CoreBridge.nativeBeginStreamImport(name, size, folder, root,
            manifest.id, item.id, stream.sourceUrl, item.title, item.subtitle, codec))
        if (!result.ok) fail(result.error) else handle = result.id
    }
    @Synchronized fun append(position: Long, bytes: ByteArray, offset: Int, length: Int) {
        if (finished || handle == 0L) return
        if (position != next) { fail("Non-sequential stream: offline copy cancelled."); return }
        val result = CoreBridge.Result.parse(CoreBridge.nativeAppendStreamImport(handle, bytes, offset, length))
        if (!result.ok) { fail(result.error); return }
        next += length
        if (next == expected) {
            val committed = CoreBridge.Result.parse(CoreBridge.nativeCommitStreamImport(handle))
            handle = 0; finished = true
            report(committed.ok, if (committed.ok) "Saved to the encrypted vault." else committed.error)
        }
    }
    @Synchronized private fun fail(message: String) {
        if (finished) return
        if (handle != 0L) CoreBridge.nativeAbortStreamImport(handle)
        handle = 0; finished = true; report(false, message)
    }
    @Synchronized override fun close() { fail("Offline copy cancelled before the complete track arrived.") }
}

/** ExoPlayer's existing progressive decoder reads the network directly, never a whole-track buffer. */
internal class ProviderDataSource(
    private val manifest: PluginManifest, private val plan: ProviderPlan,
    private val sink: ProviderImport?, private val active: () -> Boolean,
    private val own: (PluginHttp, Boolean) -> Unit,
) : BaseDataSource(true) {
    private var http: PluginHttp? = null
    private var input: InputStream? = null
    private var part = 0
    private var partRead = 0L
    private var partLength = -1L
    private var position = 0L
    private var remaining = -1L
    private var uri: Uri? = null
    private var opened = false
    private fun openPart(offset: Long) {
        http?.let { it.close(); own(it, false) }
        input?.close(); input = null
        val client = PluginHttp(manifest); http = client; own(client, true)
        if (!active()) throw IOException("Playback session cancelled")
        val c = client.stream(plan.parts[part].url, offset)
        partLength = c.contentLengthLong.let { if (it >= 0) it + offset else -1 }
        val declared = plan.parts[part].size
        require(declared < 0 || declared == partLength) { "Provider segment length changed" }
        partRead = offset
        val body = PushbackInputStream(c.inputStream, 12)
        input = body
        if (part == 0 && offset == 0L) {
            val signature = ByteArray(12)
            try {
                var n = 0
                while (n < signature.size) { val got = body.read(signature, n, signature.size - n); if (got < 0) break; n += got }
                val mime = c.contentType.orEmpty().substringBefore(';').trim().lowercase()
                require(mime.startsWith("audio/") || mime in setOf("", "application/octet-stream", "binary/octet-stream", "video/mp2t", "video/mp4", "application/mp4")) { "Provider returned a non-audio response" }
                val magic = if (n >= 4) String(signature, 0, 4, Charsets.US_ASCII) else ""
                val audio = magic in setOf("fLaC", "OggS", "RIFF") ||
                    (n >= 3 && String(signature, 0, 3, Charsets.US_ASCII) == "ID3") ||
                    (n >= 2 && (signature[0].toInt() and 255) == 255 && (signature[1].toInt() and 224) == 224) ||
                    (n >= 8 && String(signature, 4, 4, Charsets.US_ASCII) in setOf("ftyp", "styp")) ||
                    (n >= 1 && signature[0] == 0x47.toByte() && plan.stream.hls)
                require(audio) { "Provider response has no supported audio signature" }
                if (plan.stream.codec == "flac") require(PluginAudio.isFlac(c.contentType.orEmpty(), signature)) { "Provider response is not verified FLAC audio" }
                body.unread(signature, 0, n)
            } finally { signature.fill(0) }
        }
    }
    override fun open(dataSpec: DataSpec): Long {
        transferInitializing(dataSpec)
        require(active()) { "Playback session cancelled" }
        position = dataSpec.position; part = 0
        var within = position
        while (part < plan.parts.lastIndex && plan.parts[part].size > 0 && within >= plan.parts[part].size) {
            within -= plan.parts[part++].size
        }
        require(position == 0L || !plan.stream.hls || plan.size > 0) { "This HLS stream cannot seek without segment lengths" }
        openPart(within)
        val total = if (plan.stream.hls) plan.size else partLength
        remaining = if (dataSpec.length >= 0) dataSpec.length else if (total >= 0) total - position else -1
        require(remaining >= -1) { "Invalid provider seek" }
        sink?.begin(position, total)
        uri = dataSpec.uri; opened = true; transferStarted(dataSpec)
        return remaining
    }
    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        if (length == 0) return 0
        if (!active()) { sink?.close(); throw IOException("Playback session cancelled or vault locked") }
        if (remaining == 0L) return C.RESULT_END_OF_INPUT
        try {
            val want = minOf(length, 65536, if (remaining >= 0) remaining.coerceAtMost(65536).toInt() else 65536)
            val n = checkNotNull(input).read(buffer, offset, want)
            if (n < 0) {
                require(partLength < 0 || partRead == partLength) { "Truncated provider response" }
                if (part < plan.parts.lastIndex) { part++; openPart(0); return read(buffer, offset, length) }
                require(remaining <= 0) { "Truncated provider track" }
                return C.RESULT_END_OF_INPUT
            }
            if (!active()) { sink?.close(); throw IOException("Playback session cancelled or vault locked") }
            sink?.append(position, buffer, offset, n)
            position += n; partRead += n
            if (remaining >= 0) remaining -= n
            bytesTransferred(n); return n
        } catch (e: Exception) { sink?.close(); throw IOException("Provider stream failed", e) }
    }
    override fun getUri(): Uri? = uri
    override fun close() {
        // Cancel the transport before closing a potentially blocked input.
        http?.let { it.close(); own(it, false) }; http = null
        runCatching { input?.close() }; input = null; sink?.close()
        if (opened) { opened = false; transferEnded() }
    }
}
