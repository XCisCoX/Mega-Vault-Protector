# Portable extensions

Packages are shared by the Windows/Linux desktop and Android hosts. The common
platform owns installation, permissions, enable/disable, commands, settings, and
lifecycle. It does not load arbitrary Windows DLLs or Android native code.

`monochrome/plugin.json` contributes the first music provider.
`text-tools/plugin.json` demonstrates general-purpose JavaScript commands, not
another music API. The command runtime is bounded and exposes no ambient host IO.

## Source

Monochrome Music searches and obtains audio from Monochrome's track service.
It does **not** extract audio directly from Spotify. The original source uses
Spotify CSV metadata to match recordings elsewhere; the MegaVault plugin uses
catalogue search instead. Read [the source audit](../docs/monochrome-source-audit.md)
for the traced files and the live checks.

The provider's operator controls catalogue availability and service access. A
service outage/refusal must be shown as an error. Only save content you have
permission to download. No Spotify password or shared service cookie belongs in
this manifest.

## Music host behavior

The portable contract is [documented here](../docs/plugin-contract.md). The
plugin host provides:

- Explicit enable/disable and displayed provider permissions.
- A plugin-only music area with song/album/artist search and artwork.
- Album/artist browsing and an in-memory player.
- Download to Vault in that player, separate from ordinary vault playback.
- Per-provider settings for optional save-on-play and an encrypted-vault folder.
- A bounded RAM allowance; tracks too large fail instead of spilling to disk.
- Cancellation and memory cleanup on disable, vault lock, or leaving the player.

Automatic saving is off unless the user enables it. Its destination is a folder
**inside the current encrypted vault**, not a plaintext Downloads directory.
The same prepared bytes are used for playback and saving. The host imports them
with the shared vault's memory API, preserving encryption and deduplication.

Build, runtime, and device verification are separate; the presence of a manifest
is not proof that every host has been exercised.

## Authoring a compatible provider

Use the manifest's fields as the version-1 format, choose a unique stable ID,
explicitly declare the supported platforms and approved HTTPS hostnames, and
implement the declared protocol at your own authorized service. Imported
manifests cannot shadow the bundled provider. A manifest does not make a server
with an unrelated response format compatible: a new protocol requires a host
adapter on each supported platform.

Network permissions do not grant vault passwords/keys, arbitrary file writes,
shell access, or arbitrary executable code. Installation must not silently enable
a provider or broaden another provider's permissions.

## Authoring a general-purpose action extension

Start with `extensions/examples/text-tools/manifest.json` and `main.js`. Commands
receive explicit JSON input and return `{title, text}` for the host-owned tool
surface. No Node installation is required for the native runtime, and scripts
must not depend on Node, a browser DOM, `fetch`, or local files.

Build the portable installable JSON package:

```sh
python extensions/sdk/pack.py --manifest extensions/examples/text-tools/manifest.json --script extensions/examples/text-tools/main.js --output my-extension.json
```

The SDK refuses invalid identifiers/versions, unsupported platforms/protocols,
unexpected permissions, unsafe/duplicate command IDs, oversized scripts/packages,
and overwriting an existing output unless explicitly given `--force`. The host
must revalidate the package on installation; running the packer is not a trust
or signature endorsement. See `extensions/README.md` for API limits.

## Shared memory-boundary regression test

`VideoVaultCore.PluginMemory` generates a WAV fixture in RAM, imports it directly
into a real encrypted vault, probes it using FFmpeg through the vault reader,
checks range reads, byte-exact readback, target folder, deduplication, refusal of
invalid/locked destinations, encrypted persisted content, and reopen. It never
creates a plaintext source audio file.

This test covers the shared C++ storage boundary, not remote service licensing,
network availability, UI appearance, or platform-specific audio output.
