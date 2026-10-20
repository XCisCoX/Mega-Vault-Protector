package org.megavideoprotect.app

import kotlinx.coroutines.sync.Semaphore

/**
 * Priority for the one thing that has to feel instant: playback.
 *
 * Every thumbnail and metadata probe is a blocking JNI call that holds the
 * vault. A freshly opened folder starts a decode per visible row, all at once,
 * so a tap that follows had to queue behind dozens of them and the first frame
 * took a minute. Two rules fix that:
 *
 *   - at most [thumbs] decodes run at the same time, so the vault is never
 *     saturated by background work;
 *   - while [playback] is true nothing new starts, so the player's own reads
 *     have the vault to themselves and the picture appears at once.
 */
object VaultWork {
    val thumbs = Semaphore(2)

    @Volatile
    var playback: Boolean = false
}
