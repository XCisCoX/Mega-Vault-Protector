# MegaVault extension platform

## Platform versus first extension

The host owns installation, extension identity/version, permissions, enable/disable,
settings, navigation, cancellation, and access to protected application services.
The first music provider is a contribution to that platform, not its definition.

Version 1 has two contribution protocols:

- `monochrome-tracks-v1`: a declarative music provider using host-owned,
  permission-checked HTTPS, playback, and encrypted in-memory vault import.
- `megavault-actions-v1`: JavaScript commands using the shared bounded runtime,
  explicit JSON input, and JSON output. Text Tools is a working-source example
  independent of music services.

Packages are JSON files; action source is embedded in the package. Windows,
Linux, and Android consume the same package. No Windows DLL, Android DEX, external
Node process, or loose-file entrypoint is part of the extension format.

Platform-specific build and runtime verification must be reported separately.
The complete behavioral/security contract is in `docs/plugin-contract.md`.

## Author a command extension

Copy the source pair in `extensions/examples/text-tools/` and change its stable
ID, name/version, command descriptors, and JavaScript implementation. Keep the
three shared platform identifiers only when the extension uses portable APIs.

The script must define an object like this:

```js
globalThis.extension = {
    commands: {
        echo(input) {
            return {title: "Echo", text: input.text};
        },
    },
};
```

The command ID must appear in the manifest's `commands` array. Input is explicit
JSON, currently `{text: string, settings: object}` from the host-owned tool UI.
The UI renders `{title, text}` as plain text, never as executable HTML. JavaScript
commands are synchronous; asynchronous results are not supported in API v1.

Create the installable package with the tested, dependency-free Python packer:

```sh
python extensions/sdk/pack.py --manifest extensions/examples/text-tools/manifest.json --script extensions/examples/text-tools/main.js --output my-extension.json
```

The packer refuses to overwrite an existing output unless `--force` is passed.
It preserves the exact source text, validates action-extension metadata, bounds
source/package sizes, and refuses duplicate/unsafe command or setting IDs and
undeclared privilege requests. It does not execute the script or endorse its
author. Hosts must independently revalidate packages on load/installation.

The shipped example package can be regenerated with:

```sh
python extensions/sdk/pack.py --manifest extensions/examples/text-tools/manifest.json --script extensions/examples/text-tools/main.js --output plugins/text-tools/plugin.json --force
```

## Extension-owned settings

Action packages may declare up to 16 boolean preferences:

```json
{
  "settings": [
    {"id": "compact-json", "title": "Compact JSON output", "type": "boolean", "default": false}
  ]
}
```

The host creates the controls and persists values under the extension's identity.
Only effective validated values are provided to the command in `input.settings`.
Do not store passwords, access tokens, vault contents, or other secrets in these
preferences. Other setting types need an explicit future API, not an arbitrary
manifest object that a host silently treats as executable configuration.

Text Tools uses its preferences to choose whether line cleanup removes blank
lines and whether JSON output is compact. Music destination settings are native
vault-folder selectors and must additionally be scoped to the active vault.

## Execution boundary

The shared JavaScript runtime uses a fresh context for each command, bounded
source/input/output, heap and stack, and an execution deadline. Guests receive
no vault object/password/key, filesystem, shell, process API, browser DOM, native
library loader, network client, or standard-library IO module. A transform can
only process the explicit input it receives and return a result.

This is a language-engine isolation boundary, not a separate OS process sandbox.
It does not eliminate the need to patch the engine or validate privileged host
APIs. The hosts run commands away from the UI thread and discard stale results
after disable/close/lock. Music services remain native, permission-checked
capabilities; scripts do not gain networking by writing `fetch(...)`.

API v1 does not promise arbitrary HTML screens, unrestricted application/vault
access, background daemons, clipboard automation, cross-extension messaging,
automatic updates, or native executable plugins. Add capabilities deliberately,
with versioning, permission prompts, cancellation, and tests on every host.

## Tests

SDK packaging checks:

```sh
python -m unittest discover -s tests/extensions -v
```

These execute the real packer and test malformed package metadata. They do not
prove JavaScript isolation, player output, or Android compatibility. Shared
runtime tests, native UI/network tests, memory-import tests, and device checks
are separate evidence levels.
