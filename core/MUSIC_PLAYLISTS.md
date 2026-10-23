# Encrypted music playlists (schema 8)

## Public core API

Declared in `include/videovault/core/vault.hpp`, namespace `videovault::core`:

```cpp
struct MusicPlaylist {
    std::int64_t id;
    std::string name;
    std::uint64_t created_at; // Unix seconds
    std::uint64_t updated_at; // Unix seconds, not a conflict/revision token
    std::vector<std::int64_t> track_ids; // vault item ids, in playback order
};

Result<std::vector<MusicPlaylist>> Vault::list_music_playlists() const;
Result<std::int64_t> Vault::create_music_playlist(std::string_view name);
Result<bool> Vault::rename_music_playlist(std::int64_t playlist_id, std::string_view name);
Result<bool> Vault::delete_music_playlist(std::int64_t playlist_id);
Result<bool> Vault::set_music_playlist_tracks(
    std::int64_t playlist_id, std::span<const std::int64_t> track_ids);
```

Create returns the new playlist id. A listing includes empty playlists and is
ordered by playlist id, with each `track_ids` sequence in playback order.
Listing is a single database snapshot and never opens/probes track packages.
Resolve track ids against the existing `list_videos()` result for display or
pass them to the existing vault audio playback path. `MediaInfo::album` is now
available alongside title/artist from the selected item's lazy `media_info()`
probe; absent embedded tags produce an empty string. No per-list media probes
were introduced.

For add/remove/reorder, edit a copy of `track_ids` from a listing and submit the
complete desired sequence to `set_music_playlist_tracks`. An empty span clears
the collection. There are no duplicate entries within a playlist; a track may
belong to multiple playlists. An identical assignment succeeds. Duplicate
playlist names are allowed; names are trimmed using the existing library-entry
rule (1–120 UTF-8 bytes, no control characters or reserved path punctuation).

All five calls acquire the existing Vault mutex and reject a locked vault with
the existing `InvalidArgument` / `vault is locked` convention. Unknown playlist
or item ids, repeated track ids, documents and video items return
`InvalidArgument`. Membership validation reads the authenticated encrypted
package through FFmpeg: an audio stream must exist, and video streams other
than attached cover art are rejected. A filename extension or provider identity
is not sufficient. Missing/corrupted packages retain the core's existing error
codes. Run assignments on a worker for large playlists; validation can perform
media I/O, whereas listings do not. Returned strings/ids are caller-owned
metadata snapshots, not decryption capabilities; UI caches must clear on lock.

## Storage and cleanup

Migration 8 runs transactionally on both create and open after migration 7.
It adds `music_playlists`, `music_playlist_tracks`, a reverse video-id index,
and cleanup triggers inside the existing keyed SQLCipher database. No JSON,
plaintext sidecar, new database, key or application-specific encryption format
is used. SQLCipher's existing key/rekey and foreign-key settings apply.

A track assignment validates the full input before writing, after reserving
the database writer so another open handle cannot replace a track during
validation. One `BEGIN IMMEDIATE` transaction updates its timestamp and replaces the ordered links;
any insert/constraint/commit failure rolls back old links and metadata together.
The API is last-writer-wins, like the existing mutable core collections; it does
not provide optimistic concurrency/version checks. Item and playlist ids are
local to their vault, not transferable global identifiers.

Deleting a playlist cascades only its links, never the audio items/packages.
Deleting a track directly or recursively through a folder cascades its links
from every playlist while preserving the remaining relative order. Internal
position gaps after a cascade are harmless; the next assignment compacts them.
Existing post-commit package-file cleanup behavior is unchanged.

Replacing any item's opaque content detaches that item from playlists in the
same package-update transaction: its previous audio validation no longer
applies. This is conservative even for audio-to-audio replacement; explicitly
re-add the replacement after validation if desired. Renaming, moving, tagging
and password rekey do not detach tracks.

## Sync contract and explicit gap

Playlists are **vault-local only in this version**. Existing package exchange,
text-record JSON and protocol-4 file/folder/tag sync payloads are unchanged;
ordinary item sync remains interoperable, but it does not transmit playlist
names, order or membership. A full encrypted vault copy/backup includes them;
exporting/importing selected files does not. Do not claim a playlist is synced
or serialize its local numeric ids into a remote playlist.

Future cross-device playlist sync needs stable playlist identities, a mapping
to the sync record ids of tracks, deletion tombstones and a defined concurrent
ordering/conflict policy. Those require coordinated desktop/Android/relay
changes and are not silently added to the existing record contracts here.

## Regression verification

`VideoVaultCore.MusicPlaylists` / target `VideoVaultMusicPlaylistTests` uses real
SQLCipher databases, authenticated packages and generated WAV/AVI media. It
covers lifecycle, ordered add/remove/reorder/clear, restart persistence,
rekey, lock/wrong-password rejection, vault isolation, filename-spoofed
PDF/audio+video rejection, embedded album tags, database-only listing,
file/folder deletion cascades, content replacement, rollback after a deliberately
failed second insert, plaintext/unkeyed-read rejection, and schema-7 migration
failure/rollback/retry while preserving existing tags/provenance.

Normal build/test:

```sh
cmake --build --preset vs2022-x64-debug --target VideoVaultMusicPlaylistTests
ctest --preset vs2022-x64-debug -R '^VideoVaultCore.MusicPlaylists$' --output-on-failure
```

If another agent is changing Qt source/CMake at the same time, avoid changing
its shared build cache. An isolated core-only build can use the same preset:

```sh
cmake --preset vs2022-x64 -B out/build/music-playlists-core -DMVP_BUILD_APP=OFF \
  -DZ_VCPKG_POWERSHELL_PATH='C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'
cmake --build out/build/music-playlists-core --config Debug
ctest --test-dir out/build/music-playlists-core -C Debug --output-on-failure
```
