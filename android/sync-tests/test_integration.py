"""Static integration guards for the protocol 3 Android sync client.

These read the sources; they are not Android instrumentation and cannot show
runtime behaviour. Runtime evidence is the standalone Kotlin suite and the
real-relay interop run described in android/SYNC_V3.md.
"""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

MAIN = Path(__file__).resolve().parents[1] / 'app/src/main'
APP = MAIN / 'java/org/megavideoprotect/app'
XMLNS = '{http://schemas.android.com/apk/res/android}'


def source(name: str) -> str:
    return (APP / name).read_text(encoding='utf-8')


class IntegrationGuards(unittest.TestCase):
    def test_sync_state_excluded_from_backup_and_device_transfer(self):
        application = ET.parse(MAIN / 'AndroidManifest.xml').getroot().find('application')
        self.assertEqual(application.attrib[XMLNS + 'allowBackup'], 'false')
        self.assertEqual(application.attrib.get(XMLNS + 'dataExtractionRules'), '@xml/sync_data_extraction_rules')
        rules = ET.parse(MAIN / 'res/xml/sync_data_extraction_rules.xml').getroot()
        for mode in ('cloud-backup', 'device-transfer'):
            exclusions = {(e.attrib['domain'], e.attrib['path']) for e in rules.find(mode).findall('exclude')}
            self.assertIn(('sharedpref', 'sync_v3.xml'), exclusions)
            self.assertIn(('sharedpref', 'sync_v2.xml'), exclusions)

    def test_sync_is_a_library_key_hash_bucket(self):
        http = source('SyncHttp.kt')
        engine = source('SyncEngine.kt')
        self.assertIn('"Bearer $libraryKey"', http)
        self.assertIn('The server rejected this library key.', http)
        self.assertIn('/v3/files', engine)
        for protocol_token in ('"HEAD"', '"PUT"', '"GET"', '"DELETE"', 'X-Total-Size', 'X-Offset', 'X-Complete', '"Range"'):
            self.assertIn(protocol_token, engine)
        self.assertIn('isPackageName', source('SyncPolicy.kt'))

    def test_protocol_2_identity_and_revision_state_is_gone(self):
        code = '\n'.join(
            source(name) for name in
            ('SyncPolicy.kt', 'SyncHttp.kt', 'SyncEngine.kt', 'SyncScreen.kt', 'AndroidSyncStore.kt')
        )
        for banned in (r'/v2/', r'\bdevice_id\b', r'\btoken\b', r'\brevision\b',
                       r'\boperation_id\b', r'\bconflict\b', r'\btombstone', r'\bpairing\b', r'\bpair\b'):
            self.assertIsNone(re.search(banned, code), banned)

    def test_one_small_screen_section_without_a_retry_loop(self):
        screen = source('SyncScreen.kt')
        self.assertEqual(screen.count('MvpButton("Sync"'), 1)
        self.assertNotIn('delay(', screen)
        self.assertNotIn('repeatOnLifecycle', screen)
        self.assertNotIn('Pair', screen)
        self.assertIn('Library key', screen)
        self.assertLess(len(screen.splitlines()), 260)

    def test_ledger_and_native_bridge(self):
        adapter = source('AndroidSyncStore.kt')
        self.assertIn('CoreBridge.nativeExportPackage', adapter)
        self.assertIn('CoreBridge.nativeImportPackage', adapter)
        self.assertIn('context.noBackupFilesDir', adapter)
        self.assertIn('AES/GCM/NoPadding', adapter)
        engine = source('SyncEngine.kt')
        for token in ('"seen"', '"remote"', '"source"', 'fileHash(', 'vault.import(', 'vault.export('):
            self.assertIn(token, engine)

    def test_legacy_cloud_login_not_reachable(self):
        login = source('LoginScreen.kt')
        self.assertNotIn('Cloud login', login)
        self.assertNotIn('cloudAddress', login)
        self.assertIn('RemoteVault.login(code, password)', login)  # PC share is retained

    def test_pc_share_cannot_override_sync_tls_defaults(self):
        remote = source('RemoteVault.kt')
        self.assertNotIn('setDefaultSSLSocketFactory', remote)
        self.assertNotIn('setDefaultHostnameVerifier', remote)


if __name__ == '__main__':
    unittest.main()
