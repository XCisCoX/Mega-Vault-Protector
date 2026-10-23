package org.megavideoprotect.app

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * The cloud half of the library.
 *
 * Once a vault is linked, this keeps it in step with the relay on its own:
 * no button, no screen, no "press Sync". A local change (import, delete,
 * rename, move, tag) queues a pass a few seconds later so a burst of work
 * coalesces into one; the vault coming to the front queues one; a quiet timer
 * checks every [QUIET_PERIOD_MS] while the vault stays open. Failures back off
 * instead of hammering the server, and never lose the queued work.
 *
 * [phase] is what the library's status pill draws, [remoteRevision] is bumped
 * when a pass actually brought something in so the gallery re-reads the index
 * it is showing, and [lastSyncedAt] feeds the "Synced 2m ago" line.
 */
object SyncV4Auto {
    sealed interface Phase {
        /** No linked vault: nothing to say, no pill. */
        object Off : Phase
        /** Linked and waiting — the normal state. */
        object Idle : Phase
        /** A pass is running; [Busy.label] is its current step. */
        data class Busy(val label: String) : Phase
        /** The relay refused or the network is down; a retry is scheduled. */
        data class Failed(val message: String) : Phase
    }

    var phase by mutableStateOf<Phase>(Phase.Off)
        private set
    var lastSyncedAt by mutableStateOf(0L)
        private set
    var lastSummary by mutableStateOf("")
        private set

    /** Bumped whenever a pull changed what this phone holds. */
    var remoteRevision by mutableStateOf(0)
        private set

    private var context: Context? = null
    private var store: SyncV4StateStore? = null
    private var link: SyncV4Link? = null
    private var scope: CoroutineScope? = null
    private var debounce: Job? = null
    private var inFlight = false
    private var pending = false
    private var failures = 0

    private const val QUIET_PERIOD_MS = 90_000L
    private const val DEBOUNCE_MS = 3_500L
    private const val FIRST_BACKOFF_MS = 6_000L
    private const val MAX_BACKOFF_MS = 120_000L

    // ── lifetime ─────────────────────────────────────────────────────────

    /** Called when the vault opens. Reads the saved link and starts watching. */
    fun attach(appContext: Context, vaultRoot: String) {
        if (scope != null) return
        val saved = SyncV4StateStore(appContext.applicationContext, vaultRoot)
        context = appContext.applicationContext
        store = saved
        val created = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        scope = created
        created.launch {
            val linked = runCatching { saved.load() }.getOrNull()
            link = linked
            if (linked == null) {
                phase = Phase.Off
                return@launch
            }
            phase = Phase.Idle
            pass("opened")
            while (isActive) {
                delay(QUIET_PERIOD_MS)
                if (!inFlight) pass("timer")
            }
        }
    }

    /** Called when the vault locks. Nothing of the old vault survives. */
    fun detach() {
        debounce?.cancel()
        debounce = null
        scope?.cancel()
        scope = null
        store = null
        context = null
        link = null
        inFlight = false
        pending = false
        failures = 0
        phase = Phase.Off
    }

    // ── requests ─────────────────────────────────────────────────────────

    /** Something changed here. Queue a pass just far enough out to coalesce. */
    fun markDirty() {
        val runner = scope ?: return
        if (link == null) return
        pending = true
        debounce?.cancel()
        debounce = runner.launch {
            delay(DEBOUNCE_MS)
            pass("change")
        }
    }

    /** Explicit request: the Sync page's button, a retry, or coming to front. */
    fun syncNow() {
        val runner = scope ?: return
        runner.launch { pass("manual") }
    }

    /** Adopts a link the Sync page just created or signed out of. */
    fun adopt(updated: SyncV4Link?) {
        link = updated
        failures = 0
        phase = if (updated == null) Phase.Off else Phase.Idle
    }

    /** "Synced 2m ago" / "Connecting…" — the pill's quiet half. */
    fun statusText(now: Long): String = when (val state = phase) {
        Phase.Off -> ""
        is Phase.Busy -> state.label
        is Phase.Failed -> "Sync paused"
        Phase.Idle -> when {
            lastSyncedAt <= 0L -> "Connected"
            else -> {
                val age = (now - lastSyncedAt).coerceAtLeast(0L)
                when {
                    age < 60_000L -> "Synced just now"
                    age < 3_600_000L -> "Synced ${age / 60_000L}m ago"
                    age < 86_400_000L -> "Synced ${age / 3_600_000L}h ago"
                    else -> "Synced ${age / 86_400_000L}d ago"
                }
            }
        }
    }

    // ── one pass ─────────────────────────────────────────────────────────

    private suspend fun pass(reason: String) {
        val appContext = context ?: return
        val saved = store ?: return
        if (inFlight) {
            pending = true
            return
        }
        // The Sync page owns the engine while its own pass runs.
        if (SyncV4Run.running) {
            pending = true
            return
        }
        inFlight = true
        pending = false
        phase = Phase.Busy("Checking for changes…")
        var pulledCount = 0
        try {
            // The Sync page writes the link too; always start from the stored one.
            val current = runCatching { saved.load() }.getOrNull() ?: link ?: return
            link = current
            val engine = SyncV4Engine(appContext, saved, current)
            pulledCount = engine.pull { label -> phase = Phase.Busy(label) }
            if (pulledCount > 0) remoteRevision += 1
            val pushedCount = engine.push { label -> phase = Phase.Busy(label) }
            link = engine.link
            lastSyncedAt = System.currentTimeMillis()
            lastSummary = when {
                pulledCount == 0 && pushedCount == 0 -> "Everything is up to date."
                else -> "Pulled $pulledCount, pushed $pushedCount."
            }
            failures = 0
            phase = Phase.Idle
        } catch (failure: Throwable) {
            failures += 1
            phase = Phase.Failed(failure.message ?: "Sync failed")
        } finally {
            inFlight = false
            val retryNow = pending
            pending = false
            if (retryNow) markDirty()
            else if (phase is Phase.Failed) scheduleRetry()
        }
    }

    private fun scheduleRetry() {
        val runner = scope ?: return
        val wait = (FIRST_BACKOFF_MS shl failures.coerceAtMost(5)).coerceAtMost(MAX_BACKOFF_MS)
        runner.launch {
            delay(wait)
            if (!inFlight) pass("retry")
        }
    }
}
