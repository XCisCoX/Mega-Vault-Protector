"""Real authenticated proxy/TLS origin fixtures. No personal vault or internet.
python qt-app/tests/run_proxy_tls_fixtures.py <VideoVaultProxyTests.exe>
"""
import base64
import http.server
import json
import os
from pathlib import Path
import select
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse
USER, PASSWORD = b"fixture-user", b"fixture-password"
events, origins, errors = [], [], []
def exact(sock, size):
    result = b""
    while len(result) < size:
        data = sock.recv(size - len(result))
        if not data: raise EOFError("truncated handshake")
        result += data
    return result
class Origin(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        origins.append((self.path, dict(self.headers)))
        body = b"proxy-origin-ok"
        if self.path.startswith("/search"): body = b'{"fixture":true,"tracks":[]}'
        elif self.path.startswith("/repos/"): body = json.dumps({"tag_name":"v1.0.0","assets":[]}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, format, *args): pass
class Proxy(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            self.request.settimeout(20)
            if getattr(self.server, "kind") == "socks5":
                version, count = exact(self.request, 2)
                assert version == 5 and 2 in exact(self.request, count)
                self.request.sendall(b"\x05\x02")
                assert exact(self.request, 1) == b"\x01"
                user = exact(self.request, exact(self.request, 1)[0]); password = exact(self.request, exact(self.request, 1)[0])
                assert (user, password) == (USER, PASSWORD)
                self.request.sendall(b"\x01\x00")
                version, cmd, reserved, atyp = exact(self.request, 4)
                assert (version, cmd, atyp) == (5, 1, 3), "SOCKS must resolve remotely (ATYP=3)"
                host = exact(self.request, exact(self.request, 1)[0]).decode(); port = struct.unpack("!H", exact(self.request, 2))[0]
                assert host in ("proxy-only.invalid", "api.github.com")
                target = tls_origin.server_port if port == 443 else port
                assert target in (origin.server_port, tls_origin.server_port)
                upstream = socket.create_connection(("127.0.0.1", target))
                self.request.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00")
                events.append(("socks5", host, port, "authenticated remote DNS"))
            else:
                data = b""
                while b"\r\n\r\n" not in data:
                    chunk = self.request.recv(4096)
                    if not chunk: return
                    data += chunk
                lines = data.split(b"\r\n")
                expected = b"Proxy-Authorization: Basic " + base64.b64encode(USER + b":" + PASSWORD)
                if not any(line.lower() == expected.lower() for line in lines):
                    self.request.sendall(b'HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="fixture"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'); return
                method, target, version = lines[0].split(b" ")
                if method == b"CONNECT":
                    host, port = target.decode().rsplit(":",1)
                    assert host in ("proxy-only.invalid", "api.github.com") and int(port) == 443
                    upstream = socket.create_connection(("127.0.0.1", tls_origin.server_port))
                    self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                else:
                    url = urllib.parse.urlsplit(target.decode()); host, port = url.hostname, url.port
                    assert host == "proxy-only.invalid" and port == origin.server_port
                    upstream = socket.create_connection(("127.0.0.1", port))
                    lines[0] = method + b" " + url.path.encode() + b" " + version
                    lines = [line for line in lines if not line.lower().startswith(b"proxy-")]
                    upstream.sendall(b"\r\n".join(lines))
                events.append(("http",host,port,"authenticated"))
            with upstream:
                while True:
                    readable,_,_ = select.select([self.request,upstream],[],[],20)
                    if not readable: return
                    for source in readable:
                        chunk = source.recv(65536)
                        if not chunk: return
                        (upstream if source is self.request else self.request).sendall(chunk)
        except (ConnectionResetError,ConnectionAbortedError,BrokenPipeError): pass
        except Exception as exc: errors.append(repr(exc))
class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
    kind="http"
def start(server):
    threading.Thread(target=server.serve_forever,daemon=True).start(); return server
with tempfile.TemporaryDirectory(prefix="mvp-proxy-",dir=os.environ.get("TMPDIR")) as temporary:
    certificate,key=Path(temporary)/"ca.pem",Path(temporary)/"key.pem"
    subprocess.run(["openssl","req","-x509","-newkey","rsa:2048","-nodes","-keyout",str(key),"-out",str(certificate),"-days","1","-subj","/CN=proxy-only.invalid","-addext","subjectAltName=DNS:proxy-only.invalid,DNS:api.github.com","-addext","basicConstraints=critical,CA:TRUE"],check=True,capture_output=True)
    origin=start(http.server.ThreadingHTTPServer(("127.0.0.1",0),Origin))
    tls_origin=http.server.ThreadingHTTPServer(("127.0.0.1",0),Origin)
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(certificate,key)
    tls_origin.socket=context.wrap_socket(tls_origin.socket,server_side=True); start(tls_origin)
    try:
        for kind in ("http","socks5"):
            proxy=Server(("127.0.0.1",0),Proxy); proxy.kind=kind; start(proxy)
            try:
                result=subprocess.run([sys.argv[1],kind,str(proxy.server_address[1]),str(origin.server_port),str(certificate)],capture_output=True,text=True,timeout=65)
                print(kind,result.returncode,result.stdout.strip(),result.stderr.strip())
                assert result.returncode==0,"production client failed proxy fixture"
            finally: proxy.shutdown(); proxy.server_close()
        assert not errors,errors
        assert len(origins)==6,origins
        assert len(events)==6,events
        assert all(not any(k.lower().startswith("proxy-") for k in headers) for _,headers in origins),"proxy auth leaked to origin"
        assert all(PASSWORD.decode() not in str(h) and USER.decode() not in str(h) for _,h in origins),"credentials leaked to origin"
        print("PASS",events,"origin requests:",len(origins),"credentials absent at HTTP/TLS origins")
    finally: origin.shutdown(); origin.server_close(); tls_origin.shutdown(); tls_origin.server_close()
