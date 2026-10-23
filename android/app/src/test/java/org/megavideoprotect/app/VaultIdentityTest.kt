package org.megavideoprotect.app

import org.junit.Assert.*
import org.junit.Test

class VaultIdentityTest {
    @Test fun successfulOpenOrCreateReplacesThePreviousLocalNamespace() {
        val a = VaultIdentity().openedLocal("vault-A")
        val b = a.openedLocal("vault-B")
        assertEquals("vault-B", b.activeLocalRoot)
        assertEquals("vault-A", a.activeLocalRoot)
        assertEquals(b, b.copy()) // Retaining the process session needs no preference reload.
    }

    @Test fun remoteConnectionPreservesLastLocalRootButExposesNoLocalNamespace() {
        val remote = VaultIdentity().openedLocal("vault-B").connectedRemote()
        assertEquals("vault-B", remote.localRoot)
        assertNull(remote.activeLocalRoot)
        assertNull(remote.copy().activeLocalRoot)
        assertEquals("vault-A", remote.openedLocal("vault-A").activeLocalRoot)
    }

    @Test fun coldProcessOrRemoteOnlySessionCannotInventALocalNamespace() {
        assertNull(VaultIdentity().activeLocalRoot)
        assertNull(VaultIdentity().connectedRemote().activeLocalRoot)
    }
}
