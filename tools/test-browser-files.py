"""Real browser acceptance tests for the Files website, using disposable data only.

Optional developer dependency: Python playwright plus its Chromium runtime.
Run from the repository root: python tools/test-browser-files.py -v
No live host, user account or installed GUI application is touched.
"""
from pathlib import Path
import hashlib
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'sync_server' / 'tests'))
from support import ServerCase, scratch_dir
from playwright.sync_api import sync_playwright, expect


class BrowserFilesAcceptance(ServerCase):
    def setUp(self):
        super().setUp()
        self.jar, _ = self.user()
        self.library = self.vault(self.jar, 'Browser acceptance library')
        self.automation = sync_playwright().start()
        self.browser = self.automation.chromium.launch(headless=True)
        self.context = self.browser.new_context(viewport={'width': 1366, 'height': 900}, accept_downloads=True)
        self.context.add_cookies([{
            'name': 'mv_session', 'value': self.jar,
            'url': 'http://127.0.0.1:%d' % self.port,
            'httpOnly': True, 'sameSite': 'Lax',
        }])
        self.page = self.context.new_page()
        self.page.set_default_timeout(15_000)
        self.errors = []
        self.page.on('pageerror', lambda failure: self.errors.append(str(failure)))
        self.page.goto('http://127.0.0.1:%d/' % self.port)

    def tearDown(self):
        if getattr(self, 'browser', None):
            if self._outcome and not self._outcome.success:
                target = scratch_dir() / ('files-acceptance-%s.png' % self._testMethodName)
                self.page.screenshot(path=str(target), full_page=True)
                print('Failure screenshot:', target)
            self.context.close()
            self.browser.close()
            self.automation.stop()
        super().tearDown()

    def test_files_home_is_clear_and_fits_small_screens(self):
        expect(self.page.get_by_role('heading', name='Files', exact=True).first).to_be_visible()
        expect(self.page.get_by_role('button', name='Add files', exact=True)).to_be_visible()
        expect(self.page.get_by_text('Issue a device token', exact=True)).to_have_count(0)
        for width in (320, 390, 768, 1366):
            self.page.set_viewport_size({'width': width, 'height': 850})
            self.assertTrue(self.page.evaluate('document.documentElement.scrollWidth <= innerWidth'),
                            'The page must not clip horizontally at %dpx' % width)
        self.page.emulate_media(color_scheme='dark')
        expect(self.page.get_by_role('button', name='Add files', exact=True)).to_be_visible()
        self.assertEqual(self.errors, [], 'Browser runtime errors are not cosmetic')

    def test_real_upload_download_trash_restore(self):
        payload = b'Browser acceptance confidential bytes.\n' * 2048
        name = 'Browser acceptance notes.txt'
        test_secret = 'disposable-browser-unlock-fixture'
        outgoing = []
        self.page.on('request', lambda request: outgoing.append(request.post_data_buffer or b''))
        expect(self.page.get_by_role('button', name='Add files', exact=True)).to_be_visible()
        # A new empty library still needs a local-only file password. It must
        # never be sent to the relay, including in login or record metadata.
        unlock = self.page.get_by_role('button', name='Unlock files', exact=True)
        if unlock.count() and unlock.is_visible():
            unlock.click()
            self.page.get_by_label('File password', exact=True).fill(test_secret)
            self.page.get_by_role('dialog').get_by_role('button', name='Unlock', exact=True).click()
        self.page.get_by_test_id('file-picker').set_input_files({
            'name': name, 'mimeType': 'text/plain', 'buffer': payload,
        })
        password = self.page.get_by_label('File password', exact=True)
        if password.count() and password.is_visible():
            password.fill(test_secret)
            self.page.get_by_role('dialog').get_by_role('button', name='Unlock', exact=True).click()
        row = self.page.locator('[data-record-id]').filter(has_text=name).first
        expect(row).to_be_visible(timeout=90_000)
        rid = row.get_attribute('data-record-id')
        self.assertTrue(rid)
        # Read back using the real native API to prove browser-created records
        # are in the same library that the phone and desktop will synchronize.
        token = self.token_for(self.jar, 'acceptance-native-client')['token']
        records = self.records(token, self.library['id'])['records']
        saved = next(record for record in records if record['id'] == rid)
        self.assertEqual(saved['name'], name)
        status, _, ciphertext = self.request('GET', self.blob_path(self.library['id'], saved['package']), token=token)
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(ciphertext).hexdigest(), saved['package'])
        self.assertNotIn(payload[:80], ciphertext)
        self.assertTrue(all(test_secret.encode() not in body for body in outgoing),
                        'A file-unlock secret reached the server')
        self.assertTrue(all(payload[:80] not in body for body in outgoing),
                        'Plaintext file content reached the server')
        row.click()
        with self.page.expect_download(timeout=90_000) as download_info:
            self.page.get_by_role('button', name='Download', exact=True).click()
        download = download_info.value
        self.assertEqual(Path(download.path()).read_bytes(), payload)
        self.page.get_by_role('button', name='Move to Trash', exact=True).click()
        confirmation = self.page.get_by_role('dialog')
        if confirmation.count() and confirmation.is_visible():
            confirmation.get_by_role('button', name='Move to Trash', exact=True).click()
        expect(self.page.locator('[data-record-id="%s"]' % rid)).to_have_count(0)
        self.assertTrue(next(r for r in self.records(token, self.library['id'])['records'] if r['id'] == rid)['deleted'])
        self.page.get_by_role('button', name='Trash', exact=True).click()
        trashed = self.page.locator('[data-record-id="%s"]' % rid)
        expect(trashed).to_be_visible()
        trashed.click()
        self.page.get_by_role('button', name='Restore', exact=True).click()
        expect(trashed).to_have_count(0)
        self.assertFalse(next(r for r in self.records(token, self.library['id'])['records'] if r['id'] == rid)['deleted'])
        self.assertEqual(self.errors, [])


if __name__ == '__main__':
    unittest.main()
