"""Standalone JVM checks: no Android SDK, Gradle, emulator or device needed.

Usage:
  python android/sync-tests/run.py --tools <dir with jdk*, kotlinc/ and json.jar>
  python android/sync-tests/run.py --tools <...> --real-server

Each *Test.kt is a plain main() compiled against the app's protocol 3 sources
(SyncPolicy, SyncHttp, SyncEngine) and run on the JVM. --real-server then starts
this repository's sync_server on a disposable loopback port and runs
ServerInterop.kt against it. External json.jar is org.json:json:20240303
(Android supplies org.json in the app).
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

parser = argparse.ArgumentParser()
parser.add_argument('--tools', type=Path, required=True)
parser.add_argument('--real-server', action='store_true',
                    help="Also exercise this repository's relay using disposable scratch state")
args = parser.parse_args()
tools = args.tools.resolve()
repo = Path(__file__).resolve().parents[2]
java = next(tools.glob('jdk*/bin/java.exe'), None) or next(tools.glob('jdk*/bin/java'))
compiler_cp = os.pathsep.join(str(p) for p in (tools / 'kotlinc/lib').glob('*.jar'))
deps = str(tools / 'json.jar')
source = repo / 'android/app/src/main/java/org/megavideoprotect/app'
tests = sorted(Path(__file__).parent.glob('*Test.kt'))
output = tools / 'sync-tests.jar'
subprocess.run([
    str(java), '-cp', compiler_cp, 'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
    '-kotlin-home', str(tools / 'kotlinc'), '-cp', deps, '-include-runtime', '-d', str(output),
    *[str(source / (name + '.kt')) for name in ['SyncPolicy', 'SyncHttp', 'SyncEngine']],
    str(Path(__file__).parent / 'RelayFixture.kt'),
    *[str(test) for test in tests],
    str(Path(__file__).parent / 'ServerInterop.kt'),
], check=True)

keystore = tools / 'sync-fixture.p12'
if not keystore.exists():
    keytool = java.parent / ('keytool.exe' if os.name == 'nt' else 'keytool')
    subprocess.run([
        str(keytool), '-genkeypair', '-alias', 'fixture', '-keyalg', 'RSA', '-keysize', '2048',
        '-validity', '30', '-dname', 'CN=localhost', '-ext', 'SAN=dns:localhost,ip:127.0.0.1',
        '-storetype', 'PKCS12', '-keystore', str(keystore), '-storepass', 'changeit',
        '-keypass', 'changeit', '-noprompt',
    ], check=True)
for test in tests:
    subprocess.run([
        str(java), '-Djava.io.tmpdir=' + str(tools), '-Dsync.test.keystore=' + str(keystore),
        '-cp', str(output) + os.pathsep + deps, 'org.megavideoprotect.app.' + test.stem + 'Kt',
    ], check=True)
print(f'{len(tests)} standalone Kotlin test programs passed')

if args.real_server:
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(repo))
    from sync_server.__main__ import Server, Store
    with tempfile.TemporaryDirectory(prefix='android-relay-interop-', dir=tools) as temporary:
        data = Path(temporary) / 'relay'
        printed = subprocess.run(
            [sys.executable, '-B', '-m', 'sync_server', '--data', str(data), 'key'],
            cwd=repo, capture_output=True, text=True,
        )
        if printed.returncode != 0 or 'library key:' not in printed.stdout:
            raise SystemExit(
                '--real-server needs the protocol 3 sync_server: the one with the `key` command and '
                '/v3/files. The sync_server in this working tree answered differently:\n'
                + (printed.stderr or printed.stdout)
            )
        values = dict(line.split(':', 1) for line in printed.stdout.strip().splitlines() if ':' in line)
        key = values['library key'].strip()
        digest = values['key sha256'].strip()
        server = Server(
            ('127.0.0.1', 0), Store(data),
            argparse.Namespace(workers=4, request_timeout=30, quota_bytes=64 * 1024 * 1024,
                               max_file_bytes=16 * 1024 * 1024),
            None, bytes.fromhex(digest),
        )
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([
                str(java), '-Djava.io.tmpdir=' + str(tools), '-cp', str(output) + os.pathsep + deps,
                'org.megavideoprotect.app.ServerInteropKt',
            ], input='\n'.join([f'http://127.0.0.1:{server.server_port}', key, temporary]) + '\n',
                text=True, check=True)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
