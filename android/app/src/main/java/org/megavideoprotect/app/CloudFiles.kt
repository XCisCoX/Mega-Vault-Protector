package org.megavideoprotect.app

import androidx.compose.runtime.mutableStateOf
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File

/** Local copies of cloud media. The green tick is this set. */
object CloudFiles {
    val synced = mutableStateOf(emptySet<Long>())

    fun sessionLine(raw: String?): String {
        if (raw.isNullOrBlank()) return ""
        val people = ArrayList<String>()
        val arr = runCatching { JSONArray(raw) }.getOrNull() ?: return ""
        for (i in 0 until arr.length()) {
            val person = arr.optJSONObject(i) ?: continue
            val model = person.optString("model")
            val kind = person.optString("kind")
            if (model.isBlank() && kind.isBlank()) continue
            people.add(listOf(model, kind).filter { it.isNotBlank() }.joinToString(" · "))
        }
        return people.joinToString("    ")
    }

    suspend fun pull(cacheDir: File, videos: List<VideoEntry>) {
        val dir = File(cacheDir, "cloud").apply { mkdirs() }
        val done = videos.mapNotNull { video ->
            val file = File(dir, video.id.toString())
            if (video.size == 0L || (file.isFile && file.length() == video.size)) video.id else null
        }.toSet()
        synced.value = done
        for (video in videos) {
            if (video.id in synced.value || video.size <= 0L) continue
            val part = File(dir, "${video.id}.part")
            val ok = withContext(Dispatchers.IO) { download(video, part) }
            if (!ok) continue
            val dest = File(dir, video.id.toString())
            if (dest.exists()) dest.delete()
            if (part.renameTo(dest) || (dest.isFile && dest.length() == video.size)) {
                synced.value = synced.value + video.id
            }
        }
    }

    private fun download(video: VideoEntry, part: File): Boolean {
        part.outputStream().use { out ->
            var at = 0L
            while (at < video.size) {
                val ask = minOf(256 * 1024, (video.size - at).toInt())
                val chunk = RemoteVault.readRange(video.id, at, ask) ?: return false
                if (chunk.isEmpty()) return false
                out.write(chunk)
                at += chunk.size
            }
        }
        return part.length() == video.size
    }
}
