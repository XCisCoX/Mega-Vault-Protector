package org.megavideoprotect.app

import androidx.compose.runtime.mutableStateOf
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File

/** Local copies of cloud media. The green tick is this set. */
object CloudFiles {
    val synced = mutableStateOf(emptySet<Long>())

    /**
     * True while a video, image or song is open. The background copy then waits:
     * the link belongs to whatever is on screen, not to the sync.
     */
    @Volatile var viewerOpen = false

    /** The file the user just opened. It downloads ahead of everything else. */
    @Volatile private var wanted: Long = -1

    fun request(id: Long) {
        wanted = id
    }

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
        val queue = ArrayDeque(videos.filter { it.id !in done && it.size > 0L })
        val failed = HashSet<Long>()
        while (true) {
            // The file the user just opened jumps the queue, even mid-playback.
            val asked = wanted
            wanted = -1
            val opened = if (asked > 0L) queue.firstOrNull { it.id == asked } else null
            if (opened != null) {
                queue.remove(opened)
            } else if (viewerOpen) {
                // Something is open: hold the background copy back so browsing
                // and playback keep the link to themselves.
                delay(400)
                continue
            }
            val video = opened ?: queue.removeFirstOrNull() ?: break
            if (video.id in synced.value || video.size <= 0L || video.id in failed) continue
            if (opened == null) delay(150)
            val part = File(dir, "${video.id}.part")
            val ok = withContext(Dispatchers.IO) { download(video, part) }
            if (!ok) {
                // One bad file must not stall the rest or spin forever.
                failed.add(video.id)
                continue
            }
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
