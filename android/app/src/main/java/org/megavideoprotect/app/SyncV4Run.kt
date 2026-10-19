package org.megavideoprotect.app

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * One sync at a time, owned by the process rather than by a screen. Leaving the
 * Sync page — or opening a video from the vault — does not cancel a running
 * pass; the bar at the bottom of the library reads [progress] instead.
 */
object SyncV4Run {
    data class Progress(val label: String, val step: Int)

    var progress by mutableStateOf<Progress?>(null)
        private set
    var running by mutableStateOf(false)
        private set
    var lastResult by mutableStateOf("")
        private set

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null

    fun start(
        context: Context,
        store: SyncV4StateStore,
        link: SyncV4Link,
        onDone: (SyncV4Link, String) -> Unit = { _, _ -> },
    ) {
        if (running) return
        running = true
        progress = Progress("Starting…", 0)
        job = scope.launch {
            var step = 0
            val engine = SyncV4Engine(context.applicationContext, store, link)
            try {
                val summary = engine.syncNow { label ->
                    step += 1
                    progress = Progress(label, step)
                }
                lastResult = summary
                withContext(Dispatchers.Main) { onDone(engine.link, summary) }
            } catch (failure: Throwable) {
                lastResult = failure.message ?: "Sync failed"
            } finally {
                progress = null
                running = false
                job = null
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
        running = false
        progress = null
    }
}
