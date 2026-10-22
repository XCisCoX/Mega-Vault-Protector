"""Static integration guards for the legacy protocol 3 and active protocol 4 clients.

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
            # Protocol 3 has no account identity; the active protocol 4 screen
            # legitimately uses device tokens and tombstones, so guard it below.
            ('SyncPolicy.kt', 'SyncHttp.kt', 'SyncEngine.kt', 'AndroidSyncStore.kt')
        )
        for banned in (r'/v2/', r'\bdevice_id\b', r'\btoken\b', r'\brevision\b',
                       r'\boperation_id\b', r'\bconflict\b', r'\btombstone', r'\bpairing\b', r'\bpair\b'):
            self.assertIsNone(re.search(banned, code), banned)

    def test_active_screen_uses_protocol_4_without_legacy_pairing(self):
        screen = source('SyncScreen.kt')
        client = source('SyncV4Client.kt')
        for banned in (r'/v2/', r'\bdevice_id\b', r'\boperation_id\b', r'\brevision\b',
                       r'\bpairing\b', r'\bpair\b'):
            self.assertIsNone(re.search(banned, screen + client, re.IGNORECASE), banned)
        self.assertIn('const val PROTOCOL = 4', client)
        self.assertIn('client.protocol()', screen)
        self.assertIn('client.login(email, password)', screen)
        self.assertIn('client.issueToken("phone")', screen)
        self.assertIn('store.save(created)', screen)
        self.assertIn('SyncV4Auto.adopt(fresh)', screen)

    def test_manual_sync_is_process_owned_and_not_a_screen_retry_loop(self):
        screen = source('SyncScreen.kt')
        self.assertEqual(len(re.findall(r'else\s+"Sync now"', screen)), 1)
        self.assertRegex(screen, r'onClick\s*=\s*onSync,[\s\S]*?enabled\s*=\s*!busy\s*&&\s*!running')
        self.assertIn('onSync = { syncNow() }', screen)
        self.assertIn('if (busy || SyncV4Run.running) return', screen)
        self.assertIn('SyncV4Run.start(context, store, current)', screen)
        self.assertNotIn('delay(', screen)
        self.assertNotIn('repeatOnLifecycle', screen)

    def test_account_and_package_passwords_are_separate_and_masked(self):
        screen = source('SyncScreen.kt')
        self.assertRegex(screen, r'MvpInput\(password,\s*onPassword,[^\n]*isPassword\s*=\s*true')
        self.assertRegex(screen, r'MvpInput\(\s*packageSecret,\s*onSecret,[\s\S]*?isPassword\s*=\s*true')
        self.assertIn('if (packageSecret.isBlank())', screen)
        self.assertIn('enabled = !busy && chosen != null && packageSecret.isNotBlank()', screen)
        self.assertIn('packageSecret = packageSecret', screen)
        disconnect = screen.split('fun disconnect() {', 1)[1].split('LaunchedEffect', 1)[0]
        for cleanup in ('store.clear()', 'SyncV4Auto.adopt(null)', 'link = null',
                        'packageSecret = ""', 'password = ""', 'items = emptyList()'):
            self.assertIn(cleanup, disconnect)

    def test_protocol_4_credentials_are_keystore_sealed_and_namespace_bound(self):
        store = source('SyncV4State.kt')
        for marker in ('AndroidKeyStore', 'AES/GCM/NoPadding', 'GCMParameterSpec(128',
                       'cipher.updateAAD(namespace.toByteArray', 'cipher.doFinal(',
                       'prefs.edit().remove(namespace)'):
            self.assertIn(marker, store)
        self.assertNotIn('putString("token"', store)
        self.assertNotIn('putString("packageSecret"', store)

    def test_permanent_deletion_has_a_separate_confirmation_action(self):
        screen = source('SyncScreen.kt')
        self.assertRegex(screen, r'text\s*=\s*"Delete forever",\s*onClick\s*=\s*onAskPurge')
        confirmation = screen.split('if (askingPurge) {', 1)[1].split('Spacer(Modifier.height(14.dp))', 1)[0]
        self.assertIn('onClick = onCancelPurge', confirmation)
        self.assertIn('onClick = onConfirmPurge', confirmation)
        self.assertIn('onAskPurge = { askingPurge = true }', screen)
        self.assertIn('act { engine -> engine.purgeItems(ids) }', screen)

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
        # PC share login is retained. The typed password is moved into a local
        # before the call so Compose state does not hold it across the KDF, so
        # the second argument's name is not part of the contract: match the call.
        self.assertRegex(login, r'RemoteVault\.login\(\s*code\s*,\s*[A-Za-z_][A-Za-z0-9_]*\s*\)')

    def test_pc_share_cannot_override_sync_tls_defaults(self):
        remote = source('RemoteVault.kt')
        self.assertNotIn('setDefaultSSLSocketFactory', remote)
        self.assertNotIn('setDefaultHostnameVerifier', remote)


if __name__ == '__main__':
    unittest.main()
