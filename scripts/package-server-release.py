"""Pack the self-hosted sync server as one folder inside a zip.

    python scripts/package-server-release.py --source <repo> --output server.zip

The archive holds exactly one application folder, ``sync_server/``, with the
console (``web/``), the container deployment (``deployment/``) and the tests
already inside it: copying that folder to a host is the whole install. README,
LICENSE and a SHA256 manifest sit next to it.

Operator secrets never ship: .env files, TLS keys, SQLite databases, sync data
directories and byte-code caches are refused, not merely skipped by accident.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import zipfile

INCLUDE = ('sync_server', 'README.md', 'LICENSE')
# Named files that carry no useful extension.
LITERAL = {'LICENSE', 'README.md', 'Dockerfile', 'Caddyfile', '.env.example', '.gitignore'}
# Source, docs and console assets only. Anything else -- a stray note, a
# database, a key -- stays on the operator's machine even if it is not secret,
# because an allowlist cannot guess what someone dropped in a folder.
ALLOWED_SUFFIXES = {'.py', '.md', '.html', '.js', '.css', '.svg', '.png', '.ico',
                    '.json', '.yaml', '.yml', '.sh'}
REFUSED_NAMES = {'.env', 'sync.sqlite3', 'sync.sqlite3-wal', 'sync.sqlite3-shm'}
REFUSED_SUFFIXES = ('.pem', '.key', '.pyc', '.mvpk', '.part', '.sqlite3', '.db')
REFUSED_DIRS = {'__pycache__', 'sync-data', 'data', '.git', '.venv'}


def refused(path: Path) -> bool:
    """True when this file must never leave the operator's machine."""
    parts = set(path.parts)
    if parts & REFUSED_DIRS:
        return True
    if path.name in REFUSED_NAMES:
        return True
    # .env.example documents the shape; .env is a credential.
    if path.name.startswith('.env') and path.name != '.env.example':
        return True
    return path.name.endswith(REFUSED_SUFFIXES)


def allowed(path: Path) -> bool:
    """True when this file is part of the server as published."""
    if refused(path):
        return False
    return path.name in LITERAL or path.suffix.lower() in ALLOWED_SUFFIXES


def collect(source: Path):
    """Every file to ship, as (archive name, absolute path), sorted."""
    entries = []
    for name in INCLUDE:
        item = source / name
        if not item.exists():
            raise SystemExit('missing required path: %s' % item)
        if item.is_file():
            entries.append((name, item))
            continue
        for root, directories, files in os.walk(item):
            directories[:] = sorted(d for d in directories if d not in REFUSED_DIRS)
            for file in sorted(files):
                path = Path(root) / file
                if not allowed(path.relative_to(source)):
                    continue
                entries.append((path.relative_to(source).as_posix(), path))
    return sorted(entries, key=lambda pair: pair[0])


def write_zip(entries, destination: Path):
    manifest = {}
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as bundle:
        for name, path in entries:
            data = path.read_bytes()
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o644 & 0xFFFF) << 16
            bundle.writestr(info, data)
            manifest[name] = hashlib.sha256(data).hexdigest()
        # Deterministic timestamps keep two packs of the same tree identical.
        info = zipfile.ZipInfo('SHA256SUMS.json', date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        bundle.writestr(info, json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='repository root to pack')
    parser.add_argument('--output', required=True, type=Path, help='zip file to write')
    args = parser.parse_args(argv)

    source = args.source.resolve()
    entries = collect(source)
    if not entries:
        raise SystemExit('nothing to pack under %s' % source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    manifest = write_zip(entries, args.output)
    total = sum(path.stat().st_size for _, path in entries)
    print('packed %d files (%d bytes) into %s' % (len(manifest), total, args.output))
    return 0


if __name__ == '__main__':
    sys.exit(main())
