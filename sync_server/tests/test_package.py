"""The server download is one folder and never contains operator secrets.

`scripts/package-server-release.py` is what an operator unpacks: the archive
holds one application folder, `sync_server/`, with the console, the container
deployment and the tests already inside it. That only works if those live in
that folder, so this test packs a real tree with planted secrets, checks the
layout and that the extracted copy actually runs.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
# Assembled, not written out: this file ships in the archive, and the check
# below asserts the marker is absent from every entry.
SECRET = 'DO NOT ' + 'SHIP THIS SECRET'


class ServerPackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.source = Path(self.tmp.name) / 'source'
        self.source.mkdir()
        for name in ('sync_server', 'README.md', 'LICENSE'):
            item = ROOT / name
            if item.is_dir():
                shutil.copytree(item, self.source / name,
                                ignore=shutil.ignore_patterns('__pycache__'))
            else:
                shutil.copy2(item, self.source / name)

    def tearDown(self):
        self.tmp.cleanup()

    def plant(self, name, text=SECRET):
        target = self.source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)

    def pack(self):
        archive = Path(self.tmp.name) / 'server.zip'
        result = subprocess.run(
            [sys.executable, str(ROOT / 'scripts/package-server-release.py'),
             '--source', str(self.source), '--output', str(archive)],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        return archive

    def test_archive_is_one_folder_and_allowlisted(self):
        for name in ('sync_server/deployment/.env',
                     'sync_server/deployment/tls/relay-key.pem',
                     'sync_server/private.sqlite3',
                     'sync_server/web/debug.txt',
                     'sync_server/data/sync.sqlite3',
                     'sync_server/tests/__pycache__/support.cpython-312.pyc'):
            self.plant(name)
        archive = self.pack()
        with zipfile.ZipFile(archive) as bundle:
            self.assertIsNone(bundle.testzip())
            names = set(bundle.namelist())
            for name in ('README.md', 'LICENSE',
                         'sync_server/__main__.py', 'sync_server/store.py',
                         'sync_server/web/index.html', 'sync_server/web/app.js',
                         'sync_server/deployment/.env.example',
                         'sync_server/deployment/tls/.gitignore',
                         'sync_server/deployment/compose.yaml',
                         'sync_server/deployment/Dockerfile',
                         'sync_server/tests/test_accounts.py'):
                self.assertIn(name, names)
            self.assertFalse(any('__pycache__' in name or name.endswith(('.pem', '.sqlite3'))
                                 or name == 'sync_server/deployment/.env'
                                 for name in names), sorted(names))
            manifest = json.loads(bundle.read('SHA256SUMS.json'))
            self.assertEqual(set(manifest), names - {'SHA256SUMS.json'})
            for name, digest in manifest.items():
                self.assertEqual(hashlib.sha256(bundle.read(name)).hexdigest(), digest, name)
                self.assertNotIn(SECRET.encode(), bundle.read(name), name)
            extracted = Path(self.tmp.name) / 'extracted'
            bundle.extractall(extracted)

        # The single folder is the whole install: the CLI runs and finds its UI.
        cli = subprocess.run([sys.executable, '-m', 'sync_server', '--help'],
                             cwd=extracted, capture_output=True, text=True, timeout=30)
        self.assertEqual(cli.returncode, 0, cli.stderr)
        self.assertIn('serve', cli.stdout)
        self.assertTrue((extracted / 'sync_server' / 'web' / 'index.html').is_file())
        self.assertTrue((extracted / 'sync_server' / 'deployment' / 'compose.yaml').is_file())

    def test_repacking_the_same_tree_is_byte_identical(self):
        first = self.pack().read_bytes()
        second = self.pack().read_bytes()
        self.assertEqual(first, second, 'the archive must be reproducible')


if __name__ == '__main__':
    unittest.main()
