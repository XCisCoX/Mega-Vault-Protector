# Monochrome source investigation

## What was inspected

Local source supplied by the user:
`C:/Users/parvizi/Desktop/monochrome-main (1)/monochrome-main`.
Its `package.json` identifies the frontend as Monochrome 2.5.1 and declares
Apache-2.0. No frontend code was copied into MegaVault by this investigation.
This report describes that local copy, not a claim about every Monochrome release.

## Finding: not a direct Spotify audio downloader

The supplied frontend's Spotify import UI instructs users to export playlist CSV
through Exportify (`index.html:609-615`).
`js/playlist-importer.js:213-220` recognizes track/artist/album/ISRC headers and a
Spotify ID header. The actual import loop at lines 393-415 looks for an exact
ISRC match, then searches title and artist. Recognizing a Spotify ID column is
not evidence of fetching audio bytes from Spotify.

The catalogue facade first searches the Monochrome track service
(`js/music-api.js:110-137`), with Apple Music and older TIDAL catalogue paths as
fallbacks. The default track-service base is declared at
`js/tracks-api.js:8`. Native service tracks resolve to its `/track/<id>` endpoint
(lines 788-805); external catalogue entries are matched by metadata
(lines 818-885). The supplied frontend does not establish the server's ultimate
upstream source or distribution rights.

Download enrichment resolves that service URL in `js/api.js:2895-2953`. The
actual download path starts at line 3050, fetches bytes at line 3196, and passes
bytes through the metadata/download pipeline. `js/downloads.js:1151-1227`
connects the UI action, enrichment, download, and selected output writer.
Some older comments still refer to TIDAL; the executed audio-resolution code is
the stronger evidence for the current path.

## Executed checks

Investigation date: October 19, 2026.

1. Executed the original `TracksStreamerAPI.getStreamUrl` and
   `scoreTrackCandidate` exports using Node. Verified that its generated audio
   URL belongs to Monochrome's service, not Spotify, and that an exact ISRC
   receives the original matcher's score of 200. These checks execute the
   original frontend logic; they do not validate a server audio file.
2. A Python urllib catalogue request with its default identity received
   Cloudflare HTTP 403 / error 1010. A real-browser request succeeded. A subsequent
   native Node request with the honest user agent
   `MegaVaultProtect/2.0.3 (music-provider investigation)` also succeeded, so the
   earlier 403 was not a general prohibition on native clients. No forged Origin,
   shared token, user cookie, or disabled TLS verification was used.
3. Live `/search?q=Ludwig%20van%20Beethoven`,
   `/releases/166472736192020480`, and `/artists/155479365088448512` returned HTTP
   200 JSON with the shapes described in `plugin-contract.md`. Detail results
   use nested `artists` objects; search results often use `artistNames` arrays.
4. A bounded range request for `/track/168671849784291328` returned HTTP 206,
   4096 bytes, a `fLaC` signature, and a reported full length of 34,506,727 bytes.
   Its Content-Type was `application/octet-stream`, not `audio/flac`. No redirect
   was returned for that request. This establishes an actual audio prefix, not
   successful playback of the entire recording.
5. A separate live `/track/282968934603341824` request completed with HTTP 200
   and 7,628,484 bytes held in RAM, also starting with `fLaC`. The attempted
   standalone FFmpeg pipe-decode check was blocked because the installed CLI
   exits before execution with Windows `0xc0000135` (missing runtime dependency).
   That check is NOT evidence of successful audio decoding. No plaintext audio
   file was created by either network probe.

The source-probe and audio-probe scripts are development-only scratch artifacts,
not part of the shipped plugin. They do not write audio to disk.

## Implications for the first MegaVault plugin

- Name it Monochrome Music, not Spotify Downloader. Search/play/download use
  Monochrome's service; a future Spotify metadata integration is separate.
- Keep service access failures visible. Current service availability is not a
  compatibility or availability guarantee.
- Preserve IDs as strings and implement both artist-name response shapes.
- Validate audio signatures while allowing the observed octet-stream header.
- Keep the provider data portable; use native host adapters for networking,
  playback, and encrypted in-memory vault import. Do not ship a Windows-only DLL
  as the cross-platform plugin format.
- The user's requirement replaces CSV-first import with a plugin catalogue
  search/player, isolated settings, and optional save-on-play to a chosen vault
  folder. See `plugin-contract.md` for the requested behavior and security
  boundary; implementation/test evidence must be reported separately.
