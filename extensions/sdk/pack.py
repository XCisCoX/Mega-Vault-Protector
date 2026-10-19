#!/usr/bin/env python3
"""Pack a portable MegaVault action extension; no third-party Python packages.

This authoring tool is not a sandbox. Installed extensions are revalidated and
executed by the native hosts, never by Python or Node on the user's behalf.
"""
import argparse
import json
from pathlib import Path
import re
import sys


MAX_MANIFEST_BYTES = 512 * 1024
MAX_SCRIPT_BYTES = 256 * 1024
PLATFORMS = {"windows", "linux", "android"}


def _text(value, field: str, maximum: int) -> str:
    if (not isinstance(value, str) or not value.strip() or len(value) > maximum
            or any(ord(character) < 32 for character in value)):
        raise ValueError(f"Invalid {field}")
    return value


def validate_manifest(payload: dict) -> None:
    """Validate action-extension packages; hosts independently validate on load."""
    if type(payload.get("schemaVersion")) is not int or payload["schemaVersion"] != 1:
        raise ValueError("Unsupported schemaVersion")
    identifier = _text(payload.get("id"), "id", 120)
    if not re.fullmatch(r"[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+", identifier):
        raise ValueError("id must be a lower-case, dot-separated identifier")
    _text(payload.get("name"), "name", 80)
    _text(payload.get("description"), "description", 1000)
    version = _text(payload.get("version"), "version", 64)
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[a-zA-Z0-9.-]+)?", version):
        raise ValueError("version must be a semantic version, for example 1.0.0")
    if payload.get("protocol") != "megavault-actions-v1":
        raise ValueError("This SDK packer supports megavault-actions-v1 scripts only")
    platforms = payload.get("platforms")
    if (not isinstance(platforms, list) or not platforms
            or any(not isinstance(item, str) or item not in PLATFORMS for item in platforms)
            or len(set(platforms)) != len(platforms)):
        raise ValueError("platforms must be a nonempty unique list of supported hosts")
    if payload.get("permissions") != [] or "network" in payload:
        raise ValueError("Action scripts have no host/network privileges in API version 1")
    commands = payload.get("commands")
    if not isinstance(commands, list) or not 1 <= len(commands) <= 32:
        raise ValueError("commands must contain 1 to 32 actions")
    seen = set()
    for command in commands:
        if not isinstance(command, dict):
            raise ValueError("Each command must be an object")
        name = _text(command.get("id"), "command id", 64)
        if (not re.fullmatch(r"[a-z][a-z0-9-]*", name)
                or name in {"constructor", "prototype", "__proto__"} or name in seen):
            raise ValueError("Command ids must be unique, safe identifiers")
        seen.add(name)
        _text(command.get("title"), "command title", 80)
        if "inputLabel" in command:
            _text(command["inputLabel"], "command inputLabel", 120)
    settings = payload.get("settings", [])
    if not isinstance(settings, list) or len(settings) > 16:
        raise ValueError("settings must be an array of up to 16 boolean preferences")
    seen_settings = set()
    for setting in settings:
        if not isinstance(setting, dict):
            raise ValueError("Each setting must be an object")
        name = _text(setting.get("id"), "setting id", 64)
        if (not re.fullmatch(r"[a-z][a-z0-9-]*", name)
                or name in {"constructor", "prototype", "__proto__"} or name in seen_settings):
            raise ValueError("Setting ids must be unique, safe identifiers")
        seen_settings.add(name)
        _text(setting.get("title"), "setting title", 80)
        if setting.get("type") != "boolean" or type(setting.get("default")) is not bool:
            raise ValueError("Only boolean settings with boolean defaults are supported")
    source = payload.get("script")
    if not isinstance(source, str) or not source.strip() or len(source.encode("utf-8")) > MAX_SCRIPT_BYTES:
        raise ValueError("script must contain JavaScript source up to 256 KiB")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def pack(manifest: Path, script: Path, output: Path, force: bool = False) -> None:
    with manifest.open("rb") as input_file:
        raw = input_file.read(MAX_MANIFEST_BYTES + 1)
    if len(raw) > MAX_MANIFEST_BYTES:
        raise ValueError("Manifest exceeds 512 KiB")
    with script.open("rb") as input_file:
        source = input_file.read(MAX_SCRIPT_BYTES + 1)
    if len(source) > MAX_SCRIPT_BYTES:
        raise ValueError("Script exceeds 256 KiB")
    payload = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    if not isinstance(payload, dict):
        raise ValueError("Manifest must be a JSON object")
    payload["script"] = source.decode("utf-8")
    validate_manifest(payload)
    encoded = (json.dumps(payload, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(encoded) > MAX_MANIFEST_BYTES:
        raise ValueError("Packed extension exceeds 512 KiB")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb" if force else "xb") as destination:
        destination.write(encoded)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--script", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--force", action="store_true", help="Replace an existing generated package")
    args = parser.parse_args()
    try:
        pack(args.manifest, args.script, args.output, args.force)
    except (OSError, ValueError) as error:
        print(f"Cannot pack extension: {error}", file=sys.stderr)
        return 1
    print(f"Packed extension: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
