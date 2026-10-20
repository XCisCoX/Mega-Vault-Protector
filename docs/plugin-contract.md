# Portable extensions: version 1 contract

## Scope

An extension is a versioned JSON package, not an arbitrary native DLL, shared
object, shell executable, or Android DEX. The same package is consumed by desktop
(Windows/Linux) and Android; host UI and audio adapters are native to each
platform. Installation, identity, permissions, lifecycle, and preferences are
shared platform concepts, not features hardcoded to the first music provider.

Two contribution protocols define the initial platform:

- `monochrome-tracks-v1`: a data-only provider whose declared network and media
  actions are implemented by the host.
- `megavault-actions-v1`: portable JavaScript commands executed in the shared
  bounded runtime. Scripts receive explicit JSON input and return JSON results;
  they have no ambient filesystem, network, shell, or vault access.

New privileged capabilities require an explicit host API and permission design.
Installing a package must not make an unrelated network API compatible or grant
arbitrary native execution.

The bundled packages are `plugins/monochrome/plugin.json` and the general-purpose
example `plugins/text-tools/plugin.json`. Each build embeds those same files,
not independent platform copies. The Text Tools source and reproducible packer
live in `extensions/examples/text-tools/` and `extensions/sdk/pack.py`.

## Manifest

Required fields:

- `schemaVersion`: integer `1`.
- `id`: stable lower-case dot-separated provider identifier, used as a key, never
  accepted as an arbitrary filesystem path.
- `name`, `version`, `description`: bounded display metadata.
- `protocol`: one of the supported contribution protocols above.
- `platforms`: explicit supported host identifiers: `windows`, `linux`, `android`.
- `permissions`: `network`, `audio-playback`, `vault-import` for the music
  protocol; an empty array for JavaScript actions in API version 1.
- `network.baseUrl`: HTTPS provider API root, without credentials or fragment
  (music only).
- `network.allowedHosts`: explicit approved DNS hostnames; includes the API host
  (music only; forbidden for isolated action scripts).
- `sourceNotice`: human-readable source/rights notice, shown in settings.
- `commands`: action extensions declare up to 32 unique commands, each with a
  safe `id`, `title`, and optional `inputLabel`.
- `script`: action extensions embed UTF-8 JavaScript source, limited to 256 KiB.
  The complete package is limited to 512 KiB. Inline source avoids loose-file
  entrypoint traversal or unsafe archive extraction.
- `settings`: optional action-extension descriptors, limited to 16 unique safe
  IDs with `title`, `type: "boolean"`, and a boolean `default`. Unsupported types
  or duplicate IDs are rejected, not silently ignored.

Validate the complete manifest before installation or use. Bound manifest size,
reject unsupported schema/protocol/platform/permissions and invalid IDs/URLs,
and reject an external manifest that shadows the bundled provider ID. A local
manifest file is installation data, not trusted code. Installation/enabling must
show the network destination and the permissions involved.

Providers start disabled. Removing or disabling one removes its navigation and
cancels its requests and player. There is no silent provider update that changes
network permission.

## Protocol

The first provider uses Monochrome's track service. These routes were traced in
its frontend source and checked against the live service:

- `GET /search?q=<percent-encoded query>`: `tracks`, `releases`, `artists` arrays.
- `GET /releases/<id>`: album metadata and `tracks` array.
- `GET /artists/<id>`: artist metadata, `topTracks`, `albums`, `singles`, and
  `compilations`. Some releases embed `tracks`.
- `GET /track/<id>`: audio bytes. Live audio may use
  `application/octet-stream`; verify its signature, not just the header.

IDs are strings: do not round them through a floating-point JSON number. Tracks
can have `id` or `trackId`; releases `id` or `releaseId`; artists `id` or
`artistId`. Search artist names may be an `artistNames` array, while detail
responses use `artists: [{name, ...}]`. Durations are milliseconds.

All network URLs, including artwork and every redirect, must remain HTTPS and
within the approved host list. Use a MegaVault user agent, not a forged browser
Origin, stolen cookie, shared token, or disabled TLS verification. Surface access
refusals and service outages rather than fabricating results or silently changing
providers. Bound metadata, images, response time, redirect count, and audio size;
cancel old search generations and ignore late completions.

Network permission is not permission to probe the local machine or LAN. Reject
localhost, loopback, private/link-local address literals, credentials, and
unexpected ports in provider manifests; a base URL must not hide query/fragment
data. Provider network validation and redirect policy must not reuse the PC-share
client's deliberately different TLS trust model.

## User experience

The extension manager owns installation, enable/disable, removal of external
packages, visible permissions, and per-extension settings. Action extensions
contribute commands shown in a host-owned tool surface with explicit text input
and plain-text output. The host supplies `{"text": "...", "settings": {...}}` to the command and
renders its `{title, text}` result without interpreting HTML. Commands run off
the UI thread, and stale results from disabled/closed sessions are discarded.
Extension-owned boolean settings are rendered by the host, persisted under that
extension's ID, and provided as effective validated values. They are ordinary
preferences, not storage for credentials or vault data.

Music is one contribution type, described below, not the definition of an
extension. Future host capabilities must preserve this separation.

Enabling a provider makes a plugin-specific music area available. The normal
vault search/player are not repurposed as a remote catalogue.

- Search has visible pending, empty, error, and success states.
- Results distinguish songs, albums, and artists, with in-memory artwork.
- Albums/artists drill down to the provider's real tracks/releases.
- Selecting a playable song prepares an in-memory plugin player with
  play/pause, seek, status/progress, and Download to Vault.
- Download is a plugin-player action, never added to ordinary vault playback.
- Each provider has independent preferences: enabled, auto-save on playback,
  target encrypted-vault folder, and bounded memory allowance.
- Auto-save defaults off. Search, browsing, or a failed preparation must not
  automatically create vault items. Auto-save is triggered by actual playback.
- A deleted target folder is an error, not permission to silently save elsewhere.
- Disabling, locking, or changing vault cancels work and clears plugin state.

## Memory and vault boundary

Plaintext audio and artwork stay in application memory. Do not use plaintext
cache files, system-temp files, external-storage downloads, FFmpeg filename
staging, or disk-backed player caches for the plugin path. Reuse the loaded audio
for manual save/auto-save, rather than downloading it twice.

The host passes bytes directly to `Vault::import_bytes` (Android via a dedicated
JNI byte-array method). This writes the existing authenticated encrypted vault
package and database entry; the vault may use its own encrypted staging files.
No plugin receives the vault password, master key, arbitrary filesystem access,
or direct database access.

Pin the provider, vault session, target folder, and request generation when
starting a save. Do not redirect an old completion into a newly opened vault or
newly selected folder. Preserve the core's deduplication semantics. Stop/release
decoders and network readers before best-effort buffer wiping; avoid avoidable
plaintext copies. Enforce memory limits even when Content-Length is absent or
false. A track above the allowance must fail with an actionable message, never
fall back to disk. A full-track RAM preparation can delay playback on a slow
connection; do not describe it as progressive streaming unless the host actually
implements and tests progressive buffering.

Preferences are scoped by provider and vault identity so a folder ID from one
vault is not reused in another. No search or listening history is persisted by
default. The application can control its own file writes; an absolute guarantee
against operating-system paging, crash dumps, or third-party decoder internals
is outside this plugin contract.

## JavaScript command boundary

Scripts define `globalThis.extension = { commands: { commandId(input) { ... } } }`.
Each invocation receives a fresh runtime/context and an explicitly parsed JSON
payload. Dispatch uses an own command property, not prototype traversal or
string interpolation into executable code. Script input/output/source sizes,
heap, stack, and execution time are bounded. Timeouts also cover initialization
and JSON serialization. Results must be synchronous and JSON-serializable;
promises, cycles, undefined results, and guest exceptions fail cleanly.

The runtime does not expose QuickJS libc, `std`, `os`, Node APIs, a DOM, a module
loader, network fetch, a shell, a vault object, or keys. Network-capable music
operations remain native, permission-checked host actions. This is an embedded
language boundary, not an operating-system process sandbox or a guarantee
against vulnerabilities in the JavaScript engine.

The initial command UI is intentionally text-in/JSON-out. Arbitrary HTML UI,
background jobs, cross-extension messaging, unrestricted vault access, clipboard
automation, and executable native extensions are not silently promised by this
API version.

## Verification bar

Keep these evidence levels separate:

1. Manifest/schema and malicious-input unit tests.
2. Real host networking/parser/buffer/lifecycle tests with explicit fixtures.
3. Live service search, album, artist, and audio checks.
4. Native player decoding and actual UI interaction/screenshots.
5. Encrypted memory import, readback, deduplication, folder isolation, and lock
   cancellation tests.
6. Windows, Linux, and Android builds/device runs individually; a portable
   manifest or successful desktop test does not prove Android runtime behavior.
