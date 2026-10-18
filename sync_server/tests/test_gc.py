"""Garbage collection: unreferenced packages, abandoned uploads, dry runs.

Every test drives the real CLI over a real data directory, so what is asserted
is what an operator gets from `sync_server --data DIR gc`.
"""
from pathlib import Path
import os
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import ServerCase, package


class GarbageCollection(ServerCase):
    def setUp(self):
        super().setUp()
        self.jar, self.account = self.user()
        self.vault_id = self.vault(self.jar)['id']
        self.token = self.token_for(self.jar)['token']

    # -- helpers ----------------------------------------------------------

    def age(self, name, seconds=7200):
        """Backdate a package or partial so the sweep may take it."""
        path = self.data / 'blobs' / self.vault_id / name
        self.assertTrue(path.is_file(), path)
        when = time.time() - seconds
        os.utime(path, (when, when))
        return path

    def sweep(self, *args):
        result = self.cli('gc', *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def quota(self):
        return self.vaults(self.jar)['quota']['bytes']

    def blob_rows(self):
        return self.rows('SELECT vault,name,size,offset,complete FROM blobs ORDER BY name')

    # -- tests ------------------------------------------------------------

    def test_purged_package_is_collected_and_frees_quota(self):
        content = b'MVPK' + os.urandom(4096)
        address = package(content)
        status, _, _ = self.put_blob(self.token, self.vault_id, content)
        self.assertEqual(status, 201)
        status, _, _ = self.put_record(self.token, self.vault_id, 'item-1', 0,
                                       package=address, size=len(content))
        self.assertEqual(status, 201)
        self.assertEqual(self.quota(), len(content))

        status, _, _ = self.request('DELETE', '/v1/vaults/%s/records/item-1?purge=1' % self.vault_id,
                                    token=self.token)
        self.assertEqual(status, 204)
        # A purge frees the record, not the bytes: that is what the sweep is for.
        self.assertEqual(self.quota(), len(content))

        path = self.age(address + '.mvpk')
        out = self.sweep('--older-than-hours', '1')
        self.assertIn('1 package(s)', out, out)
        self.assertFalse(path.exists(), 'the purged package is still on disk')
        self.assertEqual(self.blob_rows(), [])
        self.assertEqual(self.quota(), 0)

    def test_referenced_package_is_never_collected(self):
        content = b'MVPK' + os.urandom(2048)
        address = package(content)
        self.put_blob(self.token, self.vault_id, content)
        self.put_record(self.token, self.vault_id, 'live', 0, package=address, size=len(content))
        path = self.age(address + '.mvpk')

        self.sweep('--older-than-hours', '1')

        self.assertTrue(path.exists(), 'a package a record points at was deleted')
        self.assertEqual(len(self.blob_rows()), 1)
        status, _, _ = self.request('GET', self.blob_path(self.vault_id, address), token=self.token)
        self.assertEqual(status, 200)

    def test_abandoned_upload_is_collected(self):
        content = b'MVPK' + os.urandom(300000)
        address = package(content)
        # Only the first 64 KiB of a 300 KiB package ever arrives.
        status, _, value = self.put_blob(self.token, self.vault_id, content[:65536],
                                         name=address, total=len(content))
        self.assertEqual(status, 201, value)
        self.assertFalse(value['complete'])
        self.assertEqual(self.quota(), len(content), 'a partial reserves its declared size')

        part = self.age(address + '.mvpk.part')
        out = self.sweep('--older-than-hours', '1')
        self.assertIn('1 abandoned upload(s)', out, out)
        self.assertFalse(part.exists())
        self.assertEqual(self.blob_rows(), [])
        self.assertEqual(self.quota(), 0)

    def test_recent_unreferenced_package_is_kept(self):
        content = b'MVPK' + os.urandom(1024)
        address = package(content)
        self.put_blob(self.token, self.vault_id, content)  # no record yet: a writer is mid-publish
        path = self.data / 'blobs' / self.vault_id / (address + '.mvpk')
        self.assertTrue(path.is_file())

        out = self.sweep('--older-than-hours', '2')

        self.assertIn('0 package(s)', out, out)
        self.assertTrue(path.exists(), 'a fresh upload was deleted before its record arrived')
        self.assertEqual(len(self.blob_rows()), 1)

    def test_dry_run_reports_without_deleting(self):
        content = b'MVPK' + os.urandom(512)
        address = package(content)
        self.put_blob(self.token, self.vault_id, content)
        path = self.age(address + '.mvpk')

        out = self.sweep('--older-than-hours', '1', '--dry-run')

        self.assertIn('dry run', out, out)
        self.assertIn('1 package(s)', out, out)
        self.assertTrue(path.exists(), 'a dry run deleted bytes')
        self.assertEqual(len(self.blob_rows()), 1)

    def test_vault_scope_leaves_other_vaults_alone(self):
        other = self.vault(self.jar, 'Other')['id']
        kept = b'MVPK' + os.urandom(700)
        stray = b'MVPK' + os.urandom(700)
        self.put_blob(self.token, self.vault_id, kept)
        self.put_blob(self.token, other, stray)
        kept_path = self.age(package(kept) + '.mvpk')
        stray_path = self.data / 'blobs' / other / (package(stray) + '.mvpk')
        self.assertTrue(stray_path.is_file())
        when = time.time() - 7200
        os.utime(stray_path, (when, when))

        out = self.sweep('--older-than-hours', '1', '--vault', other)

        self.assertIn('1 package(s)', out, out)
        self.assertTrue(kept_path.exists(), 'the sweep crossed into another vault')
        self.assertFalse(stray_path.exists())

    def test_startup_sweep_is_optional(self):
        stray = b'MVPK' + os.urandom(300)
        self.put_blob(self.token, self.vault_id, stray)
        path = self.age(package(stray) + '.mvpk')

        self.stop()
        self.start('--gc-hours', '0')
        self.assertTrue(path.exists(), '--gc-hours 0 must disable the startup sweep')

        self.stop()
        self.start('--gc-hours', '1')
        self.assertFalse(path.exists(), 'the startup sweep did not run')
        self.assertEqual(self.blob_rows(), [])


if __name__ == '__main__':
    unittest.main()
