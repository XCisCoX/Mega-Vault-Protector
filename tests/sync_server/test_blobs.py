"""Blob layer: content addressing, offset resume, ranges, quota, isolation."""
from pathlib import Path
import socket
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from support import ServerCase, package


class BlobTests(ServerCase):
    def setUp(self):
        super().setUp()
        self.jar, _ = self.user()
        self.device = self.token_for(self.jar)['token']
        self.vid = self.vault(self.jar, 'Blobs')['id']

    def test_put_head_get_range_and_delete(self):
        content = bytes(range(256)) * 40
        name = package(content)
        path = self.blob_path(self.vid, name)
        status, _, value = self.put_blob(self.device, self.vid, content)
        self.assertEqual((status, value), (201, {'offset': len(content), 'complete': True}))
        status, headers, _ = self.request('HEAD', path, token=self.device)
        self.assertEqual((status, headers['content-length'], headers['x-offset'], headers['x-complete']),
                         (200, str(len(content)), str(len(content)), 'true'))
        status, headers, body = self.request('GET', path, token=self.device)
        self.assertEqual((status, body), (200, content))
        self.assertEqual(headers['etag'], '"%s.mvpk"' % name)
        status, headers, body = self.request('GET', path, token=self.device, headers={'Range': 'bytes=10-19'})
        self.assertEqual((status, body, headers['content-range']),
                         (206, content[10:20], 'bytes 10-19/%d' % len(content)))
        self.assertEqual(self.request('GET', path, token=self.device, headers={'Range': 'bytes=100-'})[2],
                         content[100:])
        for bad in ('bytes=%d-' % len(content), 'bytes=5-1', 'items=0-1', 'bytes=x-y'):
            self.assertEqual(self.request('GET', path, token=self.device, headers={'Range': bad})[0], 416, bad)
        self.assertEqual(self.request('DELETE', path, token=self.device)[0], 204)
        self.assertEqual(self.request('GET', path, token=self.device)[0], 404)
        self.assertEqual(self.request('DELETE', path, token=self.device)[0], 404)
        self.assertFalse((self.data / 'blobs' / self.vid / (name + '.mvpk')).exists())

    def test_multi_chunk_upload_at_the_1mib_boundary(self):
        content = bytes(range(256)) * 12288  # 3 MiB
        name = package(content)
        for offset in range(0, len(content), 1024 * 1024):
            chunk = content[offset:offset + 1024 * 1024]
            status, _, value = self.put_blob(self.device, self.vid, chunk, name=name,
                                             offset=offset, total=len(content))
            self.assertEqual(status, 201 if offset == 0 else 200)
            self.assertEqual(value, {'offset': offset + len(chunk), 'complete': offset + len(chunk) == len(content)})
        self.assertEqual(self.request('GET', self.blob_path(self.vid, name), token=self.device)[2], content)

    def test_reput_is_idempotent_and_a_changed_size_is_a_409(self):
        content = b'ciphertext that is already there' * 10
        name = package(content)
        self.assertEqual(self.put_blob(self.device, self.vid, content)[0], 201)
        blob = self.data / 'blobs' / self.vid / (name + '.mvpk')
        stamp = blob.stat().st_mtime_ns
        status, _, value = self.put_blob(self.device, self.vid, content)
        self.assertEqual((status, value), (200, {'offset': len(content), 'complete': True}))
        self.assertEqual(blob.stat().st_mtime_ns, stamp, 'an idempotent re-put rewrote the package')
        self.assertEqual(self.put_blob(self.device, self.vid, content, total=len(content) + 1)[0], 409)

    def test_partial_upload_head_and_offset_mismatch(self):
        content = b'0123456789' * 10
        name = package(content)
        path = self.blob_path(self.vid, name)
        self.assertEqual(self.request('HEAD', path, token=self.device)[0], 404)
        self.assertEqual(self.put_blob(self.device, self.vid, content[:40], name=name,
                                       total=len(content))[2], {'offset': 40, 'complete': False})
        status, headers, _ = self.request('HEAD', path, token=self.device)
        self.assertEqual((headers['content-length'], headers['x-offset'], headers['x-complete']),
                         (str(len(content)), '40', 'false'))
        self.assertEqual(self.request('GET', path, token=self.device)[0], 404, 'a partial package is not readable')
        status, _, value = self.put_blob(self.device, self.vid, b'x' * 10, name=name,
                                         offset=0, total=len(content))
        self.assertEqual((status, value), (409, {'offset': 40}), 'a stale offset comes back with the durable one')
        self.assertEqual(self.put_blob(self.device, self.vid, content[40:], name=name, offset=40,
                                       total=len(content))[2], {'offset': len(content), 'complete': True})
        self.assertEqual(self.request('GET', path, token=self.device)[2], content)

    def test_resume_after_the_server_is_killed(self):
        content = bytes(range(256)) * 400
        name = package(content)
        self.put_blob(self.device, self.vid, content[:70000], name=name, total=len(content))
        self.stop()  # terminate(): no graceful shutdown, like a power cut
        self.start()
        path = self.blob_path(self.vid, name)
        status, headers, _ = self.request('HEAD', path, token=self.device)
        self.assertEqual((status, headers['x-offset']), (200, '70000'), 'the durable offset survives a crash')
        self.assertEqual(self.put_blob(self.device, self.vid, content[:70000], name=name,
                                       offset=0, total=len(content))[0], 409)
        self.assertEqual(self.put_blob(self.device, self.vid, content[70000:], name=name, offset=70000,
                                       total=len(content))[2], {'offset': len(content), 'complete': True})
        self.assertEqual(self.request('GET', path, token=self.device)[2], content)
        self.assertFalse((self.data / 'blobs' / self.vid / (name + '.mvpk.part')).exists())

    def test_bytes_written_after_the_last_acknowledged_chunk_are_dropped(self):
        content = bytes(range(256)) * 400
        name = package(content)
        self.put_blob(self.device, self.vid, content[:70000], name=name, total=len(content))
        self.stop()
        part = self.data / 'blobs' / self.vid / (name + '.mvpk.part')
        with part.open('ab') as f:
            f.write(b'tail written after the last acknowledged chunk')
        self.start()
        self.assertEqual(part.stat().st_size, 70000)
        self.put_blob(self.device, self.vid, content[70000:], name=name, offset=70000, total=len(content))
        self.assertEqual(self.request('GET', self.blob_path(self.vid, name), token=self.device)[2], content)

    def test_a_package_must_hash_to_its_name(self):
        content = b'encrypted payload whose name lies'
        status, _, value = self.put_blob(self.device, self.vid, content, name='0' * 64)
        self.assertEqual((status, value), (422, {'error': 'hash mismatch'}))
        self.assertEqual(self.request('HEAD', self.blob_path(self.vid, '0' * 64), token=self.device)[0], 404)
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM blobs')[0]['n'], 0)
        self.assertFalse((self.data / 'blobs' / self.vid / ('0' * 64 + '.mvpk.part')).exists())

    def test_oversized_chunk_and_oversized_package_are_refused(self):
        name = package(b'x')
        oversized = 1024 * 1024 + 1
        request = ('PUT %s?offset=0 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer %s\r\n'
                   'X-Total-Size: %d\r\nContent-Length: %d\r\n\r\n'
                   % (self.blob_path(self.vid, name), self.device, oversized, oversized))
        with socket.create_connection(('127.0.0.1', self.port), timeout=10) as sock:
            sock.sendall(request.encode())
            reply = b''
            while b'\r\n' not in reply:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                reply += chunk
        self.assertIn(b' 413 ', reply.split(b'\r\n')[0])
        status, _, value = self.put_blob(self.device, self.vid, b'x', total=1024**3 + 1)
        self.assertEqual((status, value), (413, {'error': 'package too large'}))
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM blobs')[0]['n'], 0)

    def test_quota_counts_declared_sizes_including_partials(self):
        self.stop()
        self.start('--quota-bytes', '20')
        self.assertEqual(self.put_blob(self.device, self.vid, b'a' * 10)[0], 201)
        self.assertEqual(self.put_blob(self.device, self.vid, b'b' * 10)[0], 201)
        self.assertEqual(self.put_blob(self.device, self.vid, b'c')[2], {'error': 'quota exceeded'})
        self.assertEqual(self.put_blob(self.device, self.vid, b'c')[0], 507)
        self.assertEqual(self.put_blob(self.device, self.vid, b'd' * 4, total=10)[0], 507,
                         'a partial reserves its declared size')
        first = package(b'a' * 10)
        self.assertEqual(self.request('DELETE', self.blob_path(self.vid, first), token=self.device)[0], 204)
        self.assertEqual(self.put_blob(self.device, self.vid, b'e' * 10)[0], 201)
        usage = self.request('GET', '/v1/vaults', token=self.device)[2]
        self.assertEqual(usage['quota'], {'bytes': 20, 'limit': 20})
        self.assertEqual(usage['vaults'][0]['bytes'], 20)

    def test_a_package_in_one_vault_is_invisible_in_another(self):
        other = self.vault(self.jar, 'Other')['id']
        content = b'the same ciphertext in two vaults'
        name = package(content)
        self.assertEqual(self.put_blob(self.device, self.vid, content)[0], 201)
        self.assertEqual(self.request('HEAD', self.blob_path(other, name), token=self.device)[0], 404)
        self.assertEqual(self.request('GET', self.blob_path(other, name), token=self.device)[0], 404)
        self.assertEqual(self.request('DELETE', self.blob_path(other, name), token=self.device)[0], 404)
        self.assertEqual(self.request('GET', self.blob_path(self.vid, name), token=self.device)[2], content)
        self.assertEqual(self.put_blob(self.device, other, content)[0], 201, 'another vault stores its own copy')
        self.assertEqual(self.request('DELETE', self.blob_path(self.vid, name), token=self.device)[0], 204)
        self.assertEqual(self.request('GET', self.blob_path(other, name), token=self.device)[2], content,
                         'deleting one vault\'s copy must not touch the other')

    def test_deleting_a_vault_removes_its_packages_from_disk(self):
        vault = self.vault(self.jar, 'Doomed')['id']
        content = b'packages that go away with the vault'
        name = package(content)
        self.put_blob(self.device, vault, content)
        self.assertEqual(self.request('DELETE', '/v1/vaults/' + vault, cookie=self.jar)[0], 204)
        self.assertFalse((self.data / 'blobs' / vault).exists())
        self.assertFalse((self.data / 'blobs' / vault / (name + '.mvpk')).exists())
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM blobs WHERE vault=?', (vault,))[0]['n'], 0)


if __name__ == '__main__':
    unittest.main()
