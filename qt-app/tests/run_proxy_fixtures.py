"""Real loopback origin + authenticated HTTP/SOCKS5 proxies; no internet or vault.
Run: python qt-app/tests/run_proxy_fixtures.py <VideoVaultProxyTests.exe>
The reserved .invalid name must reach SOCKS as a domain, not be resolved locally.
"""
import base64
import http.server
import select
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import urllib.parse

USER, PASSWORD = b"fixture-user", b"fixture-password"
events = []
origins = []
errors = []


def exact(sock, size):
    result = b""
    while len(result) < size:
        data = sock.recv(size - len(result))
        if not data:
            raise EOFError("truncated proxy handshake")
        result += data
    return result


class Origin(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        origins.append(dict(self.headers))
        body = b"proxy-origin-ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Proxy(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(8)
            if self.server.kind == "socks5":
                version, count = exact(self.request, 2)
                assert version == 5 and 2 in exact(self.request, count)
                self.request.sendall(b"\x05\x02")
                assert exact(self.request, 1) == b"\x01"
                user = exact(self.request, exact(self.request, 1)[0])
                password = exact(self.request, exact(self.request, 1)[0])
                assert (user, password) == (USER, PASSWORD)
                self.request.sendall(b"\x01\x00")
                version, cmd, reserved, atyp = exact(self.request, 4)
                assert (version, cmd, atyp) == (5, 1, 3), "SOCKS must use remote DNS ATYP=3"
                host = exact(self.request, exact(self.request, 1)[0]).decode()
                port = struct.unpack("!H", exact(self.request, 2))[0]
                assert host == "proxy-only.invalid" and port == origin.server_port
                upstream = socket.create_connection(("127.0.0.1", port))
                self.request.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00")
                events.append(("socks5", host, "authenticated", "remote DNS"))
            else:
                data = b""
                while b"\r\n\r\n" not in data:
                    data += self.request.recv(4096)
                lines = data.split(b"\r\n")
                expected = b"Proxy-Authorization: Basic " + base64.b64encode(USER + b":" + PASSWORD)
                if not any(line.lower() == expected.lower() for line in lines):
                    self.request.sendall(b'HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="fixture"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                    return
                method, target, version = lines[0].split(b" ")
                url = urllib.parse.urlsplit(target.decode())
                assert url.hostname == "proxy-only.invalid" and url.port == origin.server_port
                upstream = socket.create_connection(("127.0.0.1", url.port))
                lines[0] = method + b" " + url.path.encode() + b" " + version
                lines = [line for line in lines if not line.lower().startswith(b"proxy-")]
                upstream.sendall(b"\r\n".join(lines))
                events.append(("http", url.hostname, "authenticated"))
            with upstream:
                while True:
                    readable, _, _ = select.select([self.request, upstream], [], [], 8)
                    if not readable:
                        return
                    for source in readable:
                        chunk = source.recv(65536)
                        if not chunk:
                            return
                        (upstream if source is self.request else self.request).sendall(chunk)
        except Exception as exc:
            errors.append(repr(exc))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def start(server):
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


origin = start(http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin))
try:
    for kind in ("http", "socks5"):
        proxy = Server(("127.0.0.1", 0), Proxy)
        proxy.kind = kind
        start(proxy)
        try:
            result = subprocess.run([sys.argv[1], kind, str(proxy.server_address[1]), str(origin.server_port)], capture_output=True, text=True, timeout=40)
            print(kind, result.returncode, result.stdout.strip(), result.stderr.strip())
            assert result.returncode == 0, "production client failed proxy fixture"
        finally:
            proxy.shutdown()
            proxy.server_close()
    assert not errors, errors
    assert len(origins) == 2, origins
    assert len(events) == 2, events
    assert all(not any(key.lower().startswith("proxy-") for key in headers) for headers in origins), "proxy auth leaked to origin"
    assert all(PASSWORD.decode() not in str(headers) and USER.decode() not in str(headers) for headers in origins), "proxy credentials leaked to origin"
    print("PASS", events, "origin requests:", len(origins), "proxy credentials absent at origin")
finally:
    origin.shutdown()
    origin.server_close()
