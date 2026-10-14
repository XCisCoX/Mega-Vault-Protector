"""Static integration guards; not a substitute for Android instrumentation."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1] / 'app/src/main'
ANDROID = '{http://schemas.android.com/apk/res/android}'

class IntegrationGuards(unittest.TestCase):
    def test_sync_secrets_excluded_from_backup_and_device_transfer(self):
        application = ET.parse(ROOT / 'AndroidManifest.xml').getroot().find('application')
        self.assertEqual(application.attrib[ANDROID + 'allowBackup'], 'false')
        self.assertEqual(application.attrib.get(ANDROID + 'dataExtractionRules'), '@xml/sync_data_extraction_rules')
        rules = ET.parse(ROOT / 'res/xml/sync_data_extraction_rules.xml').getroot()
        for mode in ('cloud-backup', 'device-transfer'):
            exclusions = [(e.attrib['domain'], e.attrib['path']) for e in rules.find(mode).findall('exclude')]
            self.assertIn(('sharedpref', 'sync_v2.xml'), exclusions)
    def test_legacy_cloud_login_not_reachable(self):
        login = (ROOT / 'java/org/megavideoprotect/app/LoginScreen.kt').read_text(encoding='utf-8')
        self.assertNotIn('Cloud login', login)
        self.assertNotIn('cloudAddress', login)
        self.assertIn('RemoteVault.login(code, password)', login)  # PC retained
    def test_pc_share_cannot_override_sync_tls_defaults(self):
        remote = (ROOT / 'java/org/megavideoprotect/app/RemoteVault.kt').read_text(encoding='utf-8')
        self.assertNotIn('setDefaultSSLSocketFactory', remote)
        self.assertNotIn('setDefaultHostnameVerifier', remote)
    def test_bridge_uses_real_mvpk_apis(self):
        adapter = (ROOT / 'java/org/megavideoprotect/app/AndroidSyncStore.kt').read_text(encoding='utf-8')
        self.assertIn('CoreBridge.nativeExportPackage', adapter)
        self.assertIn('CoreBridge.nativeImportPackage', adapter)
        self.assertIn('getInt("count")', adapter)
        self.assertIn('context.noBackupFilesDir', adapter)

if __name__ == '__main__':
    unittest.main()
