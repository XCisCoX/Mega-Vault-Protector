"""Source boundary regressions, not a substitute for JNI/device execution."""
import unittest
from pathlib import Path
R=Path(__file__).resolve().parents[2]; A=R/'android/app/src/main'
class PluginBoundaries(unittest.TestCase):
 def test_shared_manifest_assets(self):
  self.assertIn('assets.srcDir("../../plugins")',(R/'android/app/build.gradle.kts').read_text())
 def test_memory_jni_import(self):
  s=(A/'cpp/jni_bridge.cpp').read_text(); self.assertIn('JNI_METHOD(nativeImportBytes)',s)
  self.assertIn('nativeImportBytesForVault',s)
  self.assertIn('g_vault_root',s)
  s=s.split('static jstring import_bytes_for_vault')[1].split('JNI_METHOD(nativeImportBytes)')[0]
  self.assertIn('import_bytes',s); self.assertIn('GetByteArrayElements',s); self.assertIn('JNI_ABORT',s)
  self.assertNotIn('import_file',s)
 def test_provider_player_boundaries(self):
  root=A/'java/org/megavideoprotect/app'
  self.assertTrue((root/'PluginManager.kt').exists(), 'Data-only plugin manager is missing')
  self.assertTrue((root/'PluginSession.kt').exists(), 'Memory player session is missing')
  s=(root/'PluginSession.kt').read_text()
  self.assertIn('ProviderDataSource',s)
  self.assertNotIn('ByteArrayDataSource',s)
  self.assertIn('onIsPlayingChanged',s)
  self.assertIn('eraseColor',s)
  self.assertIn('importSink?.close()',s)
  self.assertIn('setTargetBufferBytes',s)
  self.assertNotIn('cacheDir',s)
 def test_registry_separates_provider_from_manager(self):
  root=A/'java/org/megavideoprotect/app'
  self.assertNotIn('org.megavault.monochrome',(root/'PluginManager.kt').read_text())
  self.assertTrue((root/'PluginProviders.kt').exists(), 'Provider adapter registry missing')
 def test_action_runtime_is_linked_and_dispatched(self):
  self.assertIn('MegaVaultExtensions',(A/'cpp/CMakeLists.txt').read_text())
  self.assertIn('nativeInvokeExtension',(A/'cpp/jni_bridge.cpp').read_text())
  self.assertTrue((A/'java/org/megavideoprotect/app/ExtensionActionPanel.kt').exists())
 def test_navigation(self):
  s=(A/'java/org/megavideoprotect/app/MainActivity.kt').read_text()
  plugins=s.split('is Screen.Plugins -> {',1)[1].split('is Screen.PluginMusic ->',1)[0]
  self.assertIn('PluginScreen(',plugins)
  self.assertIn('!RemoteVault.connected() && root != null',plugins)
  self.assertIn('vaultRoot = root',plugins)
  self.assertIn('PluginRuntime.close()',s)
if __name__=='__main__': unittest.main()
