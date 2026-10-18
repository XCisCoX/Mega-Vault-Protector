"""The server download is standalone and never includes operator secrets."""
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

ROOT = Path(__file__).resolve().parents[1]


class ServerPackageTests(unittest.TestCase):
    def test_archive_is_standalone_and_allowlisted(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
            source = Path(tmp) / 'source'
            source.mkdir()
            for directory in ('sync_server', 'sync_web', 'deployment', 'docs', 'tests/sync_server'):
                shutil.copytree(ROOT / directory, source / directory)
            shutil.copy2(ROOT / 'LICENSE', source / 'LICENSE')
            for name in ('deployment/.env', 'deployment/tls/relay-key.pem',
                         'sync_server/private.sqlite3', 'sync_web/debug.txt',
                         'deployment/operator-backup.zip'):
                target = source / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('DO NOT SHIP THIS SECRET')
            archive = Path(tmp) / 'server.zip'
            result = subprocess.run(
                [sys.executable, str(ROOT / 'scripts/package-server-release.py'),
                 '--source', str(source), '--output', str(archive)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(archive) as bundle:
                self.assertIsNone(bundle.testzip())
                names = set(bundle.namelist())
                for name in ('README.md', 'LICENSE', 'sync_server/__main__.py',
                             'sync_server/store.py', 'sync_web/index.html',
                             'deployment/.env.example', 'deployment/tls/.gitignore',
                             'deployment/compose.yaml', 'deployment/Dockerfile',
                             'tests/sync_server/test_accounts.py'):
                    self.assertIn(name, names)
                manifest = json.loads(bundle.read('SHA256SUMS.json'))
                self.assertEqual(set(manifest), names - {'SHA256SUMS.json'})
                for name, digest in manifest.items():
                    self.assertEqual(hashlib.sha256(bundle.read(name)).hexdigest(), digest)
                    self.assertNotIn(b'DO NOT SHIP THIS SECRET', bundle.read(name))
                self.assertFalse(any('__pycache__' in name or name.endswith('.pem') for name in names))
                extracted = Path(tmp) / 'extracted'
                bundle.extractall(extracted)
            cli = subprocess.run([sys.executable, '-m', 'sync_server', '--help'],
                                 cwd=extracted, capture_output=True, text=True, timeout=30)
            self.assertEqual(cli.returncode, 0, cli.stderr)
            self.assertIn('serve', cli.stdout)


if __name__ == '__main__':
    unittest.main()
