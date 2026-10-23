# Self-hosted browser package cryptography

These are **unaltered upstream distribution artifacts**, fetched with `npm pack`
into the Hermes scratch directory and extracted locally (not a CDN loader).
Only their public filenames have been changed:

| Served filename | npm package / exact version | Original archive member |
|---|---|---|
| `sodium.js` | `libsodium-sumo@0.8.4` | `package/dist/modules-sumo/libsodium-sumo.js` |
| `sodium-wrappers.js` | `libsodium-wrappers-sumo@0.8.4` | `package/dist/modules-sumo/libsodium-wrappers.js` |

The UMD core embeds its WASM payload; there is **no separate `.wasm` download**.
The tested runtime reports libsodium **1.0.22**. SUMO is needed for
`crypto_pwhash` (not exposed by the small standard wrappers build).

## Provenance, licensing, integrity

- JS/bindings source: <https://github.com/jedisct1/libsodium.js>
- Core source: <https://github.com/jedisct1/libsodium/tree/1.0.22-RELEASE>
- JS tarball: <https://registry.npmjs.org/libsodium-sumo/-/libsodium-sumo-0.8.4.tgz>
- Wrappers tarball: <https://registry.npmjs.org/libsodium-wrappers-sumo/-/libsodium-wrappers-sumo-0.8.4.tgz>
- Both npm packages are ISC licensed; original LICENSE files are retained as
  `libsodium-sumo.LICENSE` and `libsodium-wrappers-sumo.LICENSE`.
- `libsodium-core.LICENSE` is copied verbatim from
  <https://raw.githubusercontent.com/jedisct1/libsodium/1.0.22-RELEASE/LICENSE>.
- Libsodium's official documentation links its independent security audit:
  <https://doc.libsodium.org/> →
  <https://www.privateinternetaccess.com/blog/libsodium-audit-results/>.
  That audit history is **not** a claim that these exact JS bindings, the new
  package parser, or every change in 1.0.22 has been independently audited.

SHA-256 (raw bytes; preserve vendored LF endings):

```text
a3d0b7eb2c24ba6b5401d88717e65bc6a92786d211bc8b91ed746d24226180f3  sodium.js
4e2c5442899321d7d9aea69bef71a3c24a6eab5911b74a5c1ad8bc8c733b0a15  sodium-wrappers.js
ce7b8ba14db085aadb72359226ccf7273225db31dad653242a7726a10dabbbd4  libsodium-sumo.LICENSE
ce7b8ba14db085aadb72359226ccf7273225db31dad653242a7726a10dabbbd4  libsodium-wrappers-sumo.LICENSE
508a76d186356c0dd807a670ef510964f8724557024796a2c426c6c0e19ab683  libsodium-core.LICENSE
c51e7573c7b048acd07ec7b8677640208e9c00e0ab11416d3895bf6e299e5f7e  libsodium-sumo-0.8.4.tgz
74f0e2d9a885fa97c8a5e3b04e4a119fa9447abf4cebbbc24240212c26bb4150  libsodium-wrappers-sumo-0.8.4.tgz
```

npm archive SHA-512 integrity values:

```text
libsodium-sumo@0.8.4:
sha512-TMtHShQfVVsaxDygyapvUC3o7YsPgXa/hRWeIgzyFz6w5k/1hirGptCxp1U7XwW3rCskaTTYKgV10v86UiGgNw==
libsodium-wrappers-sumo@0.8.4:
sha512-ql7hcgulKZ3ekfa2DGAogcCKsWU0diA/0nArz1CFzh93WQdb46/Kj18ka/Hifq6uA3Ush34Pc6vU/6HXeRwUkg==
```

Reproduce the download with:

```sh
npm pack libsodium-sumo@0.8.4 libsodium-wrappers-sumo@0.8.4 --pack-destination <scratch-directory>
```

No npm install, build SDK, node_modules, or remote dependency is needed at runtime.

## Browser integration

Serve these scripts from the application origin, in this order (classic scripts,
not ES modules); load `package-crypto.js` before the application script:

```html
<script defer src="/vendor/sodium.js"></script>
<script defer src="/vendor/sodium-wrappers.js"></script>
<script defer src="/package-crypto.js"></script>
```

The wrappers create `window.sodium` with its `ready` promise. The codec awaits
that promise internally. The API is:

```js
const encrypted = await MvpPackage.encrypt(fileOrBlob, password, fraction => {});
const files = await MvpPackage.decrypt(encryptedUint8ArrayOrArrayBuffer, password, fraction => {});
// files: [{name: relativePath, bytes: Uint8Array}]
```

No password/key leaves the browser and nothing is stored in localStorage.
SHA-256 for a server blob identifier can be computed separately using WebCrypto.
Progress reports only numeric fractions, never file contents; `1` on decrypt is
emitted only after the authenticated end record and physical EOF validate.
Directories are validated but not returned; `name` retains native relative paths.
An unnamed Blob is written as `file.bin`.

Strict CSP must permit **`'wasm-unsafe-eval'`**, not `'unsafe-eval'`, in
`script-src`, alongside `'self'`. It does not require `data:`, `blob:`, inline
scripts, eval/Function, a WASM CDN, or an external worker. Example crypto-only
harness policy:

```text
default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'
```

Without WebAssembly/CSP WASM permission, fail closed; do not weaken the policy
to enable upstream's JavaScript fallback. Serve over HTTPS (localhost is used
only by the local test harness). Compressed reads require modern
`DecompressionStream('deflate')`; no permissive decompression fallback is used.

## Format and safety limits

The codec matches `core/src/share_package.cpp`, not `.vvp` vault-storage files:

- Magic `4d 56 50 4b a7 01 c3 00`, version 1, flags 0/1, reserved bytes zero.
- Little-endian KDF fields: **65536 KiB**, **3 iterations**, **parallelism 1**;
  16-byte salt, 24-byte secretstream header. Header length: 64 bytes.
- Only libsodium `crypto_pwhash_ALG_ARGON2ID13`, with a 32-byte output.
  Reject every other header KDF tuple **before invoking the KDF**. Native's
  general vault KDF allows other dimensions, but the native package writer uses
  this exact tuple. Libsodium's pwhash does not expose arbitrary parallelism.
- Little-endian 32-bit ciphertext frame lengths, XChaCha20-Poly1305 secretstream,
  native File/Chunk/Directory/End records. Writer emits uncompressed 1 MiB chunks.
- Reader accepts native uncompressed and zlib compressed chunks, validates
  decompression through EOF (including checksum/truncation/trailing-byte errors),
  and requires exact decoded length. Input is fed to DecompressionStream in
  at most **256-byte pieces** to limit transient expansion before backpressure;
  output exceeding the declared at-most-1-MiB chunk size is cancelled immediately.
- **128 MiB per file and total decoded file bytes** (`MAX_FILE_BYTES`).
- **130 MiB encrypted input** (`MAX_PACKAGE_BYTES`).
- **256 files**, **256 directories**, **4096 records**; limits are exported.
- Maximum ciphertext frame **1,048,990 bytes**, matching native's
  `compressBound(1 MiB) + 64 + secretstream_ABYTES` ceiling.
- Paths: max 1024 UTF-8 bytes; components max 120 bytes. Reject invalid UTF-8,
  absolute/traversal/empty components, backslashes, controls, native-disallowed
  punctuation, trailing dot/space, duplicate case-insensitive names, and
  file/directory conflicts. The browser does not write archive paths to disk.
- Password: 1–4096 UTF-8 bytes, no Unicode normalization. No empty passwords.
- Copy input before async work; authenticate every frame, enforce ordering and
  declared/actual file sizes, require a one-byte End record with FINAL tag and
  physical EOF, reject unknown tags/records, premature FINAL, missing/truncated
  frames, and trailing bytes. No file array escapes before complete validation.

JavaScript cannot promise secure locked memory or wipe immutable password
strings/engine copies. The codec wipes its password bytes, derived key,
plaintext record buffers, failed staged outputs and allocated WASM stream state.
State cleanup uses the pinned wrappers' exposed `libsodium` module and its
52-byte secretstream state allocation; review that integration on upgrades.

This is a bounded **buffering** API, not an unbounded streaming downloader.
Concurrent calls multiply the memory budget. A large decrypt can hold the
caller input, private ciphertext copy, decoded files, and libsodium's 64 MiB
KDF heap simultaneously; 128 MiB is not a 128 MiB process-memory ceiling.
Argon2 runs synchronously inside the async API, so it can briefly block the main
thread. Limits intentionally reject some otherwise-valid large native packages.

## Verification and reproducible tests

From the repository root, with Node (tested on Node 26):

```sh
node --test tools/web-crypto-test.cjs
```

This uses the real browser UMD entrypoints in a VM that disables string code
creation (WASM permitted), reference libsodium records, malformed authenticated
fixtures, wrong passwords, truncation/trailing data, hostile KDF dimensions,
paths/counts/sizes, compressed-stream corruption/expansion, and writer limits.
The native test is explicitly skipped unless `MVP_CRYPTO_NATIVE` is supplied.

For real native interoperability on the configured Windows build:

1. Run `cmd.exe /c tools\\web-crypto-test-build-native.bat` to rebuild the existing
   Release core and link `web-crypto-test-native.cpp` into the Hermes scratch dir.
   This uses the installed Visual Studio/vcpkg setup; it installs nothing.
2. Put `%LOCALAPPDATA%\\MegaVideoProtect\\vcpkg_installed\\x64-windows\\bin` on PATH.
3. Set `MVP_CRYPTO_NATIVE` to the scratch `web-crypto-native.exe` and optionally
   `MVP_CRYPTO_FIXTURES` to a scratch directory; run the Node command above.

The native helper calls **actual `Vault::export_package` / `import_package`**.
The test verifies both native compression modes decode in JS, and imports the
browser writer's package into a different native vault, comparing all 1,048,579
payload bytes. Fixtures and vaults are disposable and never user data.

For a real browser CSP test, use the helper's `export <scratch-fixture-directory>`
mode, set `MVP_CRYPTO_FIXTURES` to that directory, then run
`node tools/web-crypto-test-server.cjs`. Open <http://127.0.0.1:18973/>. The page
exposes `window.testResult` and verifies native fixtures, browser roundtrip,
wrong password/final tampering, zlib trailing-byte rejection, and no CSP
violations. It was executed successfully in Chromium 145 with the policy above;
libsodium reported 1.0.22. Stop the loopback-only test server when finished.
