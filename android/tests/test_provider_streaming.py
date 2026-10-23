"""Security/wiring guards complement the executable Kotlin fixture harness."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
K = ROOT / 'app/src/main/java/org/megavideoprotect/app'


class ProgressiveProviderGuards(unittest.TestCase):
    def test_provider_import_uses_incremental_core_not_complete_plaintext(self):
        source = (K / 'ProviderDataSource.kt').read_text(encoding='utf-8')
        for method in ('nativeBeginStreamImport', 'nativeAppendStreamImport', 'nativeCommitStreamImport', 'nativeAbortStreamImport'):
            self.assertIn(method, source)
        self.assertIn('position != next', source)
        self.assertIn('size <= 0', source)
        self.assertIn('65536', source)
        for forbidden in ('cacheDir', 'FileOutputStream', 'readBytes()', 'ByteArrayOutputStream'):
            self.assertNotIn(forbidden, source)

    def test_local_identity_precedes_provider_resolution(self):
        source = (K / 'PluginSession.kt').read_text(encoding='utf-8')
        for body in (source.split('private fun loadTrack', 1)[1].split('private fun newImport', 1)[0],
                     source.split('fun downloadTrack', 1)[1].split('private fun setDownload', 1)[0]):
            self.assertLess(body.index('nativeFindProviderTrack'), body.index('provider.resolve'))
        self.assertNotIn('nativeImportBytesForVault', source)
        self.assertIn('VaultDataSource.Factory()', source)

    def test_handles_pin_vault_and_are_revoked_on_lock_close_replace(self):
        source = (ROOT / 'app/src/main/cpp/jni_bridge.cpp').read_text(encoding='utf-8')
        for name in ('nativeLock', 'nativeClose', 'nativeCreateVault', 'nativeOpenVault'):
            body = source.split(f'JNI_METHOD({name})', 1)[1].split('\nJNI', 1)[0]
            self.assertIn('g_stream_imports.clear()', body)
        self.assertIn('std::shared_ptr<Vault> vault;', source)
        self.assertIn('length > 65536', source)
        self.assertIn('sodium_memzero(chunk.data()', source)
        self.assertIn('g_stream_imports.size() >= 3', source)

    def test_identity_is_explicit_memory_only_and_cleared(self):
        ui = (K / 'ProviderIdentitySettings.kt').read_text(encoding='utf-8')
        self.assertIn('PasswordVisualTransformation', ui)
        self.assertNotIn('rememberSaveable', ui)
        session = (K / 'PluginSession.kt').read_text(encoding='utf-8')
        self.assertIn('clientId = ""', session.split('fun close()', 2)[-1])
        self.assertIn('autoSaveAttempted', session)
        self.assertIn('generation++; loading?.cancel()', session)

    def test_rights_are_checked_before_soundcloud_download_endpoint(self):
        source = (K / 'SoundCloudTracksProvider.kt').read_text(encoding='utf-8')
        self.assertLess(source.index('require(denial.isEmpty())'), source.index('/download'))
        for marker in ('SNIP', 'PREVIEW', 'BLOCK', 'geo_blocked', 'downloadable', 'streamable'):
            self.assertIn(marker, source)
        self.assertNotIn('Cookie', source)

    def test_hls_fails_closed_instead_of_bypassing_restrictions(self):
        source = (K / 'ProviderDataSource.kt').read_text(encoding='utf-8')
        for marker in ('#EXT-X-ENDLIST', '#EXT-X-KEY:', '#EXT-X-SESSION-KEY:', '#EXT-X-BYTERANGE:', '#EXT-X-DISCONTINUITY'):
            self.assertIn(marker, source)
        self.assertIn('http.contentSize(part)', source)
        self.assertIn('if (measureParts)', source)


if __name__ == '__main__':
    unittest.main()
