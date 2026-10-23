package org.megavideoprotect.app

/** Process-session identity, published only after a successful open/create or QR login.
 * Picker preferences are launch hints, never the identity of an unlocked vault.
 */
data class VaultIdentity(val localRoot: String? = null, val remote: Boolean = false) {
    val activeLocalRoot: String? get() = localRoot.takeUnless { remote }

    fun openedLocal(root: String): VaultIdentity = VaultIdentity(root, remote = false)
    // A PC label is not a filesystem namespace; keep the last local root untouched.
    fun connectedRemote(): VaultIdentity = copy(remote = true)
}
