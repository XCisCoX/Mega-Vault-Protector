"""Deployment invariants that do not require Docker or third-party YAML."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DeploymentTests(unittest.TestCase):
    def test_plaintext_relay_is_not_published_and_proxy_mode_is_explicit(self):
        path = ROOT / 'deployment' / 'compose.yaml'
        self.assertTrue(path.exists(), 'secure compose deployment missing')
        text = path.read_text()
        server = text.split('  server:', 1)[1].split('  caddy:', 1)[0]
        self.assertNotIn('ports:', server)
        self.assertIn('MV_SYNC_PROXY_MODE: "1"', server)
        self.assertIn('--proxy-mode', server)
        self.assertIn('internal: true', text)
        self.assertIn('read_only: true', server)
        self.assertIn('no-new-privileges:true', server)
        self.assertIn('cap_drop: [ALL]', server)
        self.assertIn('443:443', text)
        docker = (ROOT / 'deployment' / 'Dockerfile').read_text()
        self.assertIn('USER 10001:10001', docker)
        self.assertIn('COPY sync_server', docker)
        self.assertNotIn('COPY . ', docker)
        caddy = (ROOT / 'deployment' / 'Caddyfile').read_text()
        self.assertIn('reverse_proxy server:8766', caddy)
        self.assertNotIn('log {', caddy)

if __name__ == '__main__':
    unittest.main()
