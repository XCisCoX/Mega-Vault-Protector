"""Settings navigation/source contracts; Compose compilation is checked separately."""
from pathlib import Path
import unittest

APP = Path(__file__).resolve().parents[1] / 'app/src/main/java/org/megavideoprotect/app'


class PluginSettingsIntegration(unittest.TestCase):
    def test_settings_embeds_the_real_manager_for_the_current_vault(self):
        settings = (APP / 'SettingsDialog.kt').read_text(encoding='utf-8')
        plugins = settings.split('TAB_PLUGINS -> {', 1)[1].split('TAB_SECURITY ->', 1)[0]
        self.assertIn('PluginScreen(', plugins)
        self.assertIn('vaultRoot = vaultRoot', plugins)
        self.assertIn('embedded = true', plugins)
        self.assertIn('if (remote)', plugins)
        self.assertNotIn('pluginPath', settings)
        self.assertNotIn('External plugin loading is not supported yet', plugins)
        vault = (APP / 'VaultScreen.kt').read_text(encoding='utf-8')
        call = vault.split('SettingsDialog(', 1)[1].split('onDismiss =', 1)[0]
        self.assertIn('vaultRoot = vaultLocation', call)

    def test_embedded_manager_keeps_one_store_and_scoped_consent(self):
        screen = (APP / 'PluginScreen.kt').read_text(encoding='utf-8')
        self.assertIn('embedded: Boolean = false', screen)
        self.assertIn('PluginManager(context.applicationContext, vaultRoot)', screen)
        self.assertIn('manager.manifests()', screen)
        self.assertIn('manager.options(manifest)', screen)
        self.assertIn('Enabled for this vault', screen)
        self.assertIn('Approve and enable', screen)
        self.assertIn('if (!embedded) BackHandler', screen)
        self.assertIn('if (!embedded) PluginRuntime.close()', screen)
        # Settings already scrolls: its embedded manager must not nest an
        # unbounded LazyColumn inside that scroll container.
        self.assertRegex(screen, r'if \(embedded\)\s*\{\s*Column\(')
        self.assertIn('manifests.forEach', screen)
        manager = (APP / 'PluginManager.kt').read_text(encoding='utf-8')
        self.assertIn('context.noBackupFilesDir', manager)
        self.assertIn('manifest.id + "@" + manifest.version, vaultRoot', manager)


if __name__ == '__main__':
    unittest.main()
