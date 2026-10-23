# Music providers, progressive playback, and streamed vault import

Status: design + core foundation. This document is the Phase 0 audit and the
architecture the later phases are built on. Update it when behaviour changes.

## 1. What exists today (Phase 0 findings)

### Byte path: remote provider → speaker

| Step | Code | Behaviour |
| --- | --- | --- |
| Search / browse | `qt-app/src/plugin_host.cpp` (`MusicPluginPage::search`, `completeRequest`) | `QNetworkAccessManager` GET of a declarative provider route, JSON parsed into rows. Redirect/host/size limits applied. |
| Play | `plugin_host.cpp` `MusicPluginPage::activate` → `/track/<id>` | **The whole track is buffered into a `QByteArray`** (capped by the `memoryMiB` setting, 32/64/128 MiB) and rejected unless `pluginFlac()` finds a FLAC header. |
| Decode | `PluginAudioPlayer::setMedia(QBuffer*)` → `MediaDecoder::open_memory(QByteArray)` | FFmpeg opens the *complete* buffer. `PcmSource` pumps PCM into `QAudioOutput`. |
| Save | `MusicPluginPage::saveAudio` → `Vault::import_bytes` | A second full copy of the buffer is handed to the core, which encrypts it to a vault package **after** the download finished. |
| Batch save | `PluginTrackDownloader` | Same shape per row, two at a time, whole body in RAM first. |

So `docs/plugin-contract.md` is accurate about its own limits: the provider path
is "full-track RAM preparation", explicitly *not* progressive streaming, and the
save is a second pass over bytes already in memory (not a second download, but
also not an import fed by the live stream).

### Byte path: vault → speaker (already streaming)

`MediaDecoder::open(vault, video_id, …)` installs a custom AVIO context
(`VaultSource`, `qt-app/src/media_decoder.cpp:253`) over
`Vault::read_video_range`, served through a sliding `StreamCache`. The package
format is already chunked and independently authenticated
(`core/src/internal/package.hpp`), and `PackageReader` decrypts chunks on demand
under a session guard. Local playback therefore already meets the bounded-memory
goal; only the *remote* path is whole-file.

### Vault import (already transactional, and near-streamable)

`Vault::import_file` / `import_bytes` → `seal_and_commit` → `seal_package`:
a vault-local staging package (`vault-data/.tmp/import_<rand>.vvp`), the full
ciphertext hashed with SHA-256 while it is written, then an atomic rename into
`vault-data/<h0h1>/<h2h3>/<sha256>.vvp` and one database transaction for the row
plus its wrapped file key. Any failure removes the staging file. **Nothing
plaintext is ever written to disk** — the source is a `std::istream`.

The only blocker for streaming it is the package header: it carries
`plaintext_size` and `chunk_count`, so the total must be known before the first
chunk is sealed (`write_package_file`, `core/src/package.cpp:178`).

### Identity

- `videos.content_sha256` (migration 6) stores the plaintext hash; a deliberate
  import is always a new item, and only replaying transports collapse.
- `MediaInfo` (title/artist) is probed from container tags per item.
- The plugin UI keeps provider identity **only in RAM** (`states_`,
  `PluginTrackDownloader`) and in `QSettings` for preferences. Nothing links a
  provider track to a vault row, so "already saved" cannot survive a restart,
  and the same track can be imported repeatedly.

### Android

Same shape as desktop: `PluginSession.play` fetches the whole body
(`network(...)` with a MiB cap) into a `ByteArray` and plays it through
`ProgressiveMediaSource(ByteArrayDataSource)`. Vault audio streams through
`MusicPlaybackService`; the provider catalogue is declarative
(`PluginProviders.kt`, `monochrome-tracks-v1` adapter).

## 2. Target architecture

```
provider (declarative manifest, contract v2)
      │  search / browse JSON
      │  resolve(track) -> stream descriptor {url, ranges?, codec, contentLength}
      ▼
RemoteByteSource            (qt-app, QNetworkAccessManager, off the UI thread)
      │  bounded ring buffer, HTTP Range resume, generation/cancel
      ├──────────────────────────────► MediaDecoder::open_source(...)
      │                                   (same AVIO contract as VaultSource)
      └──────────────────────────────► StreamImport (core)
                                          append(bytes) → AEAD chunk → staging
                                          commit() → atomic publish + row
```

Three seams are introduced, each independently testable:

1. **`MediaDecoder::open_source(std::shared_ptr<ByteSource>)`** — the AVIO
   `read`/`seek` contract `VaultSource` already implements, promoted to an
   interface. Vault, memory, and network sources become interchangeable; FFmpeg
   needs no new code and progressive decode is what a blocking `read` already
   gives us.
2. **`core::StreamImport`** (`Vault::begin_stream_import`) — the package writer
   split into `create`/`append`/`finalize`, so ciphertext is produced while
   bytes arrive. Same format, same chunk size, same atomic publish, same
   `content_sha256` (hashed incrementally), same `wrap_file_key` + transaction.
3. **`video_provenance` table (schema migration 7)** — provider identity inside
   the encrypted database: provider id, remote track id, canonical URL,
   title/artist/album, duration, codec, quality, artwork hash, content hash,
   vault file id, import status. Unique on `(provider_id, provider_track_id)`,
   so a second save of the same track resolves instead of duplicating.

Resolution order (`MusicResolver`, later phase): vault row by provenance key →
vault playback (zero network); otherwise provider stream, with optional
authorized import fed from the same live bytes.

## 3. Decisions and their reasons

- **No new package format.** Independence per chunk plus a header digest means
  an interrupted import cannot produce a playable partial package: every chunk
  tag binds the header digest, its index, its plaintext length and the
  final-chunk flag. Streaming only required the writer to be incremental.
- **Declared plaintext size is required for a streamed import.** Content-Length
  covers the normal progressive case; a chunked/unknown-length response can
  still be *played*, but its offline copy falls back to the bounded-memory path
  with an explicit message. Inventing a trailer format would change the on-disk
  contract for a rare case.
- **Playback and import share one fetch.** The network source owns the bytes;
  the importer is an observer, so a slow vault write never stalls the speaker
  and a mid-playback forward seek cannot corrupt the package. When the importer
  is behind the playhead it keeps fetching on its own bounded schedule rather
  than seeking the live window backwards.
- **Provider-specific knowledge stays in the manifest.** `monochrome-tracks-v1`
  is joined by a second protocol id rather than branching inside the player, so
  SoundCloud (and the next provider) is data plus host-owned HTTP, not a fork of
  `MusicPluginPage`.
- **Credentials never enter a manifest.** A provider that needs an app key reads
  it from extension settings the user entered; manifests are installed data.

## 4. Phase plan (what is done, what is next)

- [x] Phase 0 audit (this document).
- [x] Core: `StreamPackageWriter` + `Vault::begin_stream_import`, provenance
  schema and lookups, with a CTest suite (`VideoVaultCore.StreamImport`).
- [ ] Qt: `RemoteByteSource` + `MediaDecoder::open_source`; provider stream
  descriptors; player wiring (Phase 2).
- [ ] Import observer fed from the same source, progress and cancel (Phase 3).
- [ ] `MusicResolver` + library badges (Phase 4).
- [ ] SoundCloud extension (contract v2), Monochrome kept on v1 (Phase 1).
- [ ] Music experience redesign on `MusicLibraryPage`/`MusicWindow` (Phase 5).
- [ ] Android parity through the same seams.

## 5. Verification bar

Unit/headless: stream-import round trip, short/long/aborted/locked imports,
provenance resolution, no staging leftovers, package readback through
`PackageReader`. Desktop: build + `ctest`, then a real provider track played
while the vault entry appears, and a restart that plays from the vault with the
network blocked. Android: separate build/device evidence — never inferred.
