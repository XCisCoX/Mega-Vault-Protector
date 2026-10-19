"""Executable SDK packaging checks; not proof of a host's JS isolation."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PACKER = ROOT / "extensions/sdk/pack.py"


class ExtensionPackagingTests(unittest.TestCase):
    @staticmethod
    def metadata():
        return {
            "schemaVersion": 1,
            "id": "org.example.text-tools",
            "name": "Text tools",
            "version": "1.0.0",
            "description": "Local text transformations.",
            "protocol": "megavault-actions-v1",
            "platforms": ["windows", "linux", "android"],
            "permissions": [],
            "commands": [{"id": "echo", "title": "Echo", "inputLabel": "Text"}],
        }

    def test_package_embeds_the_exact_script_in_a_portable_manifest(self):
        with tempfile.TemporaryDirectory(prefix="mvp-extension-pack-") as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            script = root / "main.js"
            output = root / "extension.json"
            metadata = {
                "schemaVersion": 1,
                "id": "org.example.text-tools",
                "name": "Text tools",
                "version": "1.0.0",
                "description": "Local text transformations.",
                "protocol": "megavault-actions-v1",
                "platforms": ["windows", "linux", "android"],
                "permissions": [],
                "commands": [{"id": "echo", "title": "Echo", "inputLabel": "Text"}],
            }
            source = 'globalThis.extension = {commands: {echo: input => ({title: "Echo", text: input.text + " ✓"})}};\n'
            manifest.write_text(json.dumps(metadata), encoding="utf-8")
            script.write_bytes(source.encode("utf-8"))
            result = subprocess.run(
                [sys.executable, str(PACKER), "--manifest", str(manifest),
                 "--script", str(script), "--output", str(output)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            packed = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(packed.pop("script"), source)
            self.assertEqual(packed, metadata)

    def test_invalid_manifests_cannot_be_packaged(self):
        invalid = [
            {"schemaVersion": True},
            {"schemaVersion": 999},
            {"id": "../../vault"},
            {"id": "org.example/escape"},
            {"id": "ORG.example"},
            {"name": ""},
            {"name": "hidden\nline"},
            {"version": "latest"},
            {"protocol": "native-dll"},
            {"platforms": []},
            {"platforms": ["windows", "windows"]},
            {"platforms": ["unsupported-host"]},
            {"permissions": ["shell"]},
            {"permissions": ["network"]},
            {"commands": []},
            {"commands": [{"id": "constructor", "title": "Unsafe", "inputLabel": "Text"}]},
            {"commands": [{"id": "__proto__", "title": "Unsafe", "inputLabel": "Text"}]},
            {"commands": [{"id": "echo", "title": "Echo"}, {"id": "echo", "title": "Again"}]},
            {"network": {"baseUrl": "https://example.com", "allowedHosts": ["example.com"]}},
            {"settings": [{"id": "option", "title": "Option", "type": "secret", "default": "key"}]},
            {"settings": [{"id": "option", "title": "Option", "type": "boolean", "default": "false"}]},
            {"settings": [{"id": "../escape", "title": "Option", "type": "boolean", "default": True}]},
            {"settings": [{"id": "constructor", "title": "Option", "type": "boolean", "default": True}]},
            {"settings": [
                {"id": "option", "title": "One", "type": "boolean", "default": True},
                {"id": "option", "title": "Two", "type": "boolean", "default": False},
            ]},
        ]
        with tempfile.TemporaryDirectory(prefix="mvp-extension-invalid-") as directory:
            root = Path(directory)
            script = root / "main.js"
            script.write_text("globalThis.extension = {commands:{}};", encoding="utf-8")
            for index, update in enumerate(invalid):
                with self.subTest(invalid=update):
                    metadata = self.metadata()
                    metadata.update(update)
                    manifest = root / f"manifest-{index}.json"
                    output = root / f"extension-{index}.json"
                    manifest.write_text(json.dumps(metadata), encoding="utf-8")
                    result = subprocess.run(
                        [sys.executable, str(PACKER), "--manifest", str(manifest),
                         "--script", str(script), "--output", str(output)],
                        capture_output=True, text=True, check=False,
                    )
                    self.assertNotEqual(result.returncode, 0, "Invalid manifest was accepted")
                    self.assertFalse(output.exists(), "An invalid package was persisted")


if __name__ == "__main__":
    unittest.main()
