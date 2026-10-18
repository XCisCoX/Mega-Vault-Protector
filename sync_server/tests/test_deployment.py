"""Deployment invariants: the relay is only reachable through the TLS proxy."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DeploymentTests(unittest.TestCase):
    def test_relay_is_not_published_and_tls_terminates_at_the_edge(self):
        compose = (ROOT / 'sync_server' / 'deployment' / 'compose.yaml').read_text()
        server = compose.split('  server:', 1)[1].split('  caddy:', 1)[0]
        self.assertNotIn('ports:', server, 'the relay must not publish a plaintext port')
        self.assertIn('internal: true', compose)
        self.assertIn('read_only: true', server)
        self.assertIn('no-new-privileges:true', server)
        self.assertIn('cap_drop: [ALL]', server)
        self.assertIn('443:443', compose)
        caddy = (ROOT / 'sync_server' / 'deployment' / 'Caddyfile').read_text()
        self.assertIn('reverse_proxy server:8766', caddy)
        self.assertNotIn('log {', caddy, 'no access logs may record package names or keys')
        docker = (ROOT / 'sync_server' / 'deployment' / 'Dockerfile').read_text()
        self.assertIn('USER 10001:10001', docker)
        self.assertIn('COPY sync_server', docker)


if __name__ == '__main__':
    unittest.main()
