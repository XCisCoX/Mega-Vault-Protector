# Cloud TLS

## Build and run the server

The current release-packaging workflows package the desktop application and Android APK, not a standalone cloud server. Build `MegaVaultCloud` from source using the desktop dependencies in [CONTRIBUTING.md](../CONTRIBUTING.md#development-setup) and the [README](../README.md). In the current CMake files, the server is included automatically for non-Android builds when `MVP_BUILD_APP=ON` (the desktop presets already set this); there is no separate cloud opt-in option. The explicit target below builds just the server and its dependencies.

From the repository root on Linux:

```sh
cmake --preset linux-release
cmake --build --preset linux-release --target MegaVaultCloud -j "$(nproc)"
out/build/linux-release/qt-app/MegaVaultCloud cloud/config.json
```

On Windows, first adapt the machine-specific Qt, vcpkg, and SDK paths as described in [CONTRIBUTING.md](../CONTRIBUTING.md#windows). In a Visual Studio 2022 developer shell:

```text
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-release --target MegaVaultCloud
.\out\build\vs2022-x64\qt-app\Release\MegaVaultCloud.exe cloud/config.json
```

Substitute your local preset and output path if you use a `CMakeUserPresets.json`. Keep the runtime dependencies available: on Windows, Qt's `bin` directory must be on `PATH` and the required dependency DLLs must be present (CMake stages linked runtime DLLs beside the executable); Linux uses the installed shared libraries. These commands run from the build tree, not a self-contained server package. Create the config described below before running, or use `--vault` instead.

## Configuration and command-line overrides

Create the vault in Mega Vault Protect first. Create `cloud/config.json` with that folder, the port, the bind address, and `plain`:

```
{
  "vault": "D:/vaults/my-vault",
  "port": 8766,
  "bind": "0.0.0.0",
  "plain": false
}
```

No password and no user goes in the config. Everyone who signs in types the vault password in their own app, and that is what opens the vault on the trusted server.

```text
MegaVaultCloud [--vault <folder>] [--port <n>] [--bind <address>] [--plain] [config.json]
```

The positional config path is optional. Without one, the server looks for `config.json` beside its executable, then `cloud/config.json` relative to the working directory. If neither exists, it can run using flags alone. An explicitly selected or discovered config file must be readable.

- `--vault <folder>` overrides the config's `vault`; an existing vault is required.
- `--port <n>` overrides `port` for a positive integer; the resulting port must be 1–65535. The default is 8766. Zero or a nonnumeric value does not override the config/default.
- `--bind <address>` overrides `bind`; use an IP address. The default is `0.0.0.0` (all IPv4 interfaces).
- `--plain` overrides `plain` to enable HTTP without TLS. There is no inverse flag: to turn TLS back on, omit `--plain` and set `plain` to false in any loaded config. The default is false.
- `--help` (or `-h`) prints usage.

For example, from the repository root on Linux, the following overrides the configured bind address and port while retaining the configured vault and TLS setting:

```sh
out/build/linux-release/qt-app/MegaVaultCloud cloud/config.json --bind 127.0.0.1 --port 8767
```

To supply the vault by flag, use `--vault "/path/to/my-vault"`; if a config is discovered it is still read, and flags override its corresponding values. Never put the vault password on the command line or in the config.

## Trust and permissions

This is not zero-knowledge cloud storage: the first successful sign-in opens the vault in the server process. `MegaVaultCloud` retains the vault password in process memory while it runs to authenticate later sign-ins, and session expiration does not lock the vault. Trust the server host and administrators with vault secrets and decrypted content. TLS protects the connection, not content from the server itself.

Authenticated cloud clients can delete files and folders in the shared vault. The LAN peer-sharing read-only restrictions do not apply to cloud deletion. Keep independent backups. See [SECURITY.md](../SECURITY.md#cloud-server-trust-boundary).

## Built-in certificate

Leave `"plain": false`. When `MegaVaultCloud` starts it prints a public key:

```
Public key:
9f2c...e41b
```

That value is the SHA-256 fingerprint of the certificate the server generated for this run. Paste it into Cloud login on the phone and on the PC, next to the address `host:port`, and then type the vault password. The password only goes out after the certificate matches, and the vault opens with the first sign-in that presents it.

Each launch creates a new certificate, so copy the new public key after every restart. The printed vault path tells you which vault the key belongs to.

## Your own certificate

To keep one certificate, or to use a certificate from a real authority, terminate TLS in a proxy and let the cloud process speak plain HTTP on the machine only.

1. In `cloud/config.json` set `"plain": true` and `"bind": "127.0.0.1"`.
2. Start `MegaVaultCloud` with that config and no conflicting overrides. With the example port it listens on `127.0.0.1:8766` without TLS.
3. Point Caddy (or nginx) at `127.0.0.1:8766` and give Caddy the certificate.

Caddy example:

```
your.example.com {
    reverse_proxy 127.0.0.1:8766
}
```

4. Fingerprint the certificate clients will see:

```
openssl x509 -in cert.pem -outform der | openssl dgst -sha256
```

5. Cloud login uses `your.example.com:443`, that public key, and the vault password.

Do not set `"plain": true` on a port that other machines can reach. Plain mode has no encryption of its own.

## What a signed-in device sees

Desktop and Android cloud interfaces show remote folders and files, green ticks for cached copies, signed-in devices, and sync speed (slow, normal, fast). The desktop cloud browser opens cached files through the system's associated application; Android uses its own supported viewers and playback screens. This is not local/cloud or desktop/Android feature parity. A cloud delete affects the shared vault, not just a local copy.

Both clients download decrypted content into local cloud caches (Qt's application cache location under `cloud/` on desktop, `cacheDir/cloud/` on Android). These are plaintext copies, not encrypted vault packages, and leaving a view is not a guarantee of their deletion. Protect the client device and account as well as the server; see [SECURITY.md](../SECURITY.md#cloud-server-trust-boundary).
