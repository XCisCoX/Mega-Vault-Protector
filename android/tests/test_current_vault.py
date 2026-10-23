"""Current-vault source wiring; JVM behavior is exercised by current_vault_harness.py."""
from pathlib import Path

import unittest

APP = Path(__file__).resolve().parents[1] / 'app/src/main/java/org/megavideoprotect/app'


class CurrentVaultWiring(unittest.TestCase):
    def source(self, name):
        return (APP / name).read_text(encoding='utf-8')

    def test_only_successful_local_operations_report_the_exact_opened_root(self):
        for filename, native, callback in [('LoginScreen.kt', 'nativeOpenVault', 'onUnlocked'),
                                            ('SetupScreen.kt', 'nativeCreateVault', 'onCreated')]:
            with self.subTest(filename=filename):
                source = self.source(filename)
                self.assertIn(f'{callback}: (String) -> Unit', source)
                self.assertIn('val openedRoot = location', source)
                self.assertIn(f'{native}(openedRoot,', source)
                self.assertRegex(source, rf'if \(res.ok\) {callback}\(openedRoot\) else')
                self.assertIn('onRemoteConnected: () -> Unit', source)
                remote = source.split('if (failure == null)', 1)[1].split('else', 1)[0]
                self.assertIn('onRemoteConnected()', remote)
                self.assertNotIn(f'{callback}(', remote)

    def test_activity_uses_session_identity_not_preferences_for_unlocked_consumers(self):
        source = self.source('MainActivity.kt')
        self.assertIn('var vaultIdentity by mutableStateOf(VaultIdentity())', source)
        self.assertIn('AppSession.vaultIdentity.openedLocal(root)', source)
        self.assertIn('AppSession.vaultIdentity.connectedRemote()', source)
        self.assertEqual(2, source.count('onRemoteConnected = { openedRemoteVault() }'))
        self.assertIn('onCreated = { root -> openedLocalVault(root) }', source)
        self.assertIn('onUnlocked = { root -> openedLocalVault(root) }', source)
        self.assertNotIn('val vaultRoot = VaultLocation.saved', source)
        self.assertNotIn('vaultRoot = VaultLocation.saved', source)
        self.assertIn('else AppSession.vaultIdentity.activeLocalRoot', source)
        self.assertIn('vaultLocation = location', source)

        for screen in ['Plugins', 'Sync']:
            branch = source.split(f'is Screen.{screen} -> {{', 1)[1].split('\n                        }', 1)[0]
            self.assertIn('AppSession.vaultIdentity.activeLocalRoot', branch)
            self.assertIn('!RemoteVault.connected()', branch)
            self.assertIn('vaultRoot = root', branch)
        self.assertIn('SyncV4Auto.attach(this, root)', source)
        self.assertNotIn('SyncV4Auto.attach(this, initialLocation)', source)
        self.assertIn('AppSession.vaultIdentity.activeLocalRoot?.let { root -> SyncV4Auto.attach(this, root) }', source)
        remote = source.split('private fun openedRemoteVault()', 1)[1].split('private fun show', 1)[0]
        self.assertNotIn('VaultLocation.save', remote)
        self.assertIn('SyncV4Auto.detach()', remote)

    def test_remote_lock_transition_does_not_render_a_local_namespace(self):
        source = self.source('MainActivity.kt')
        branch = source.split('is Screen.Vault -> {', 1)[1].split('is Screen.Plugins ->', 1)[0]
        self.assertTrue('if (location != null) VaultScreen(' in branch,
                        'Outgoing remote Vault content must tolerate disconnect before its exit animation finishes')
        self.assertIn('if (RemoteVault.connected()) RemoteVault.label()', branch)
        self.assertIn('else AppSession.vaultIdentity.activeLocalRoot', branch)

    def test_plugin_namespace_owns_all_pending_ui_state_and_effects(self):
        source = self.source('PluginScreen.kt').split('@Composable\nprivate fun ExtensionSettingsDialog', 1)[0]
        for name in ['manifests', 'revision', 'error', 'consent', 'configure', 'browser', 'actionBrowser', 'installing']:
            self.assertRegex(source, rf'var {name} by remember\(vaultRoot\)')
        self.assertIn('LaunchedEffect(vaultRoot, revision)', source)
        self.assertIn('remember(vaultRoot, manifest.id, manifest.version, revision)', source)
        self.assertIn('key(vaultRoot)', source)
        self.assertIn('key(vaultRoot) { rememberCoroutineScope() }', source)


if __name__ == '__main__':
    unittest.main()
