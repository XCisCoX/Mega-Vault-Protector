# SoundCloud provider research: access, implementations, and distribution

Research snapshot: **2026-10-23 UTC**. This is a source review, not a successful SoundCloud playback/download test. No authorized SoundCloud application identity or user session was available; no identity was invented, web-player credentials extracted, account tokens read, or media downloaded. Upstream files were retrieved live with authenticated `gh api` repository/commit/content reads; upstream programs were **not executed**.

## Decision summary

- Prefer a small, independently implemented adapter to the **official public API**, with approved application registration and supported OAuth, rather than bundling a downloader and inheriting its credential extraction and stream-saving defaults. The official specification uses `api.soundcloud.com`, authenticated search, and authenticated stream resolution; it is not the desktop adapter's client-ID-only `api-v2` contract.[24][25]
- Study yt-dlp for metadata normalization, pagination, incomplete playlist entries, and format/preview handling. Its original-download gate is separate from stream extraction: successfully obtaining a stream is **not** its proof of original-download permission.[1]
- scdl v3 is primarily a yt-dlp wrapper, not a separately maintained streaming engine; its README explicitly says feature development is inactive, despite recent automated dependency updates.[7][12]
- downcloud offers a compact Bun implementation and useful playlist hydration/remux examples, but automatically extracts a client identity from web assets when none is configured, and falls back to saving HLS when an original download is unavailable. Those behaviors are **not acceptable defaults** for this project's authorized-download policy.[16][17][18]
- **Release blockers in the reviewed desktop source:** no supported OAuth flow; undocumented `api-v2` access presented too much like an official API integration; incorrect handling of official `access: "blocked"`. The existing original-only save path is substantially safer than saving the playback stream. See the read-only findings below.

## 1. Pinned upstream snapshot and maintenance

These are retrieved default-branch commit SHAs, not claims that released binaries contain exactly those commits.

| Repository | Retrieved commit SHA | Commit date (UTC) | Maintenance evidence |
| --- | --- | --- | --- |
| `yt-dlp/yt-dlp` | `43a45ae37419b2a90db710c9fd06cd250ef524d3` | 2026-10-21 | GitHub latest release: `2026.10.21`, published 2026-10-21. The SoundCloud extractor's latest path-specific commit was `8bdfbfd4461a643e5c37a232b0efd7bcd86a3091`, 2026-07-01; repository activity is not the same as recent SoundCloud changes.[1][27][28] |
| `scdl-org/scdl` | `282da6c5b87aad313ee98aa8df75da6a40baaeaf` | 2026-10-22 | Source metadata says `3.0.9`.[5] README: v3 wraps yt-dlp, development inactive, bug fixes welcome.[7] A daily workflow bumps the yt-dlp minimum and scdl patch version.[12] GitHub's `/releases/latest` returned the much older `v2.4.0` (2021-12-28), so that endpoint alone is misleading about current source/package maintenance.[30] |
| `yaaaarn/downcloud` | `081b079e7213e3acef40110a5cad8cd7b5e80a09` | 2026-09-07 | Source package version `2.0.0`; latest GitHub release `v2.0.0`, published 2026-07-13. These timestamps establish activity, not reliability, security review, or an upstream support commitment.[14][32] |
| **Official** `soundcloud/api` | `33092b81ff8011f1d97441ea6202d176c1cf19c2` | 2026-09-30 | SoundCloud's own repository contains its public OpenAPI specification and authentication guidance. Its README recommends direct API integration and says its old Ruby/Python/JS SDKs are not maintained.[22][23] |

### Retrieval limitation: official website versus official source

Direct requests to `https://developers.soundcloud.com/docs/api/guide`, `/reference`, and `/terms-of-use` failed with connection refusal; a real browser also returned `ERR_CONNECTION_REFUSED`. The hostname resolved here to private address `10.10.34.36`, while an HTTPS control request to `example.com` succeeded. Thus the website failure is an environment/network limitation, **not evidence that SoundCloud removed those documents**.

The official API facts below are grounded in the **live-fetched, pinned `soundcloud/api` repository**, not an imagined successful fetch of the website. That repository explicitly identifies its OpenAPI document and links the guide/reference.[22][23] The website API terms and help-center download article could not be reviewed; no claim of current contractual approval or comprehensive legal compliance is made. Recheck the current [API terms](https://developers.soundcloud.com/docs/api/terms-of-use), [API guide](https://developers.soundcloud.com/docs/api/guide), and [API reference](https://developers.soundcloud.com/docs/api/reference) before distribution.

## 2. Official public API: what is actually documented

### Authentication and identity

The public specification declares `https://api.soundcloud.com` as its server, global authorization, and OAuth 2.1 authorization-code/client-credentials flows. Its authorization header description is `OAuth ACCESS_TOKEN`; a bare application `client_id` is not the documented replacement for an access token.[24]

SoundCloud's authentication guidance describes authorization code **with PKCE** for user-delegated operations and client credentials for public resources, using `https://secure.soundcloud.com/authorize` and `https://secure.soundcloud.com/oauth/token`. It also describes refresh-token rotation and recommends token reuse instead of repeatedly obtaining client-credentials tokens.[25] The official README provides application registration through `you/apps` or an official credentials CLI; neither route was run in this review.[22]

**Native-app caution:** the OpenAPI token schema lists a `client_secret`, while the concise PKCE guidance does not fully settle every native registration/deployment detail. Do not infer that PKCE permits embedding a confidential application's secret in a distributable desktop binary. Confirm the registered application's supported flow; use an approved confidential backend where required, or a genuinely supported public-native flow. Keep secrets out of manifests, logs, command lines, and ordinary settings.[24][25]

### Search, metadata, playlists, and stream formats

| Capability | Official documented surface | Boundary |
| --- | --- | --- |
| Search | `GET /tracks?q=...`, `/users?q=...`, `/playlists?q=...`; search accepts tokens from either documented flow. | Different from reverse-engineered `/search/tracks` on `api-v2`.[24] |
| Link resolution | `GET /resolve`; user and playlist resource endpoints, including playlist track retrieval. | Implement the official schema rather than assuming web-player numeric IDs and field shapes are stable.[24] |
| Access classification | `access: playable`, `preview`, or `blocked`. | `playable` means full listening, `preview` a snippet, `blocked` metadata only; none is itself a download grant.[24] |
| Streaming | `GET /tracks/{track_urn}/streams`, described as requiring continued authentication; separate preview endpoint. | Current `Streams` schema lists `hls_aac_160_url`, `hls_mp3_128_url`, `preview_mp3_128_url`; it does **not** promise a progressive-file alternative for every track.[24] |
| Original-file downloads | Track schema has `downloadable` and nullable `download_url`; description says the URL is omitted/null when not downloadable. | The fetched specification does **not** declare the adapter's `api-v2 /tracks/{id}/download → redirectUri` exchange as a public endpoint contract. Treat the returned official download capability and successful authorization as authoritative, not a hand-built URL.[24] |

These are documented interface semantics, **not an availability/SLA guarantee**. A public track, a subscription, a cached URL, successful playback, or possession of an application ID must not be treated as permission for permanent offline copies or redistribution. Recommended application policy: save only when current metadata permits download **and** the authorized original-download capability succeeds; never fall back from a refused original to HLS/progressive stream capture. This is a conservative product policy, not a finding that the unreachable terms expressly authorize encrypted-vault storage.

## 3. Reverse-engineered implementations

### yt-dlp SoundCloud extractor

At the pinned commit, `SoundcloudBaseIE` uses `https://api-v2.soundcloud.com/`. It reads/caches a client ID extracted from SoundCloud's home-page JavaScript, and can refresh it after 401/403. This is an implementation observation, **not supported app registration**, and must not be transplanted as an identity-acquisition solution.[1]

- **Metadata:** normalizes uploader, canonical URL, title, timestamps, duration, description, content-license string, counts, genre/tags, publisher artist, and artwork. A track's `license` string remains content metadata; it does not license the extractor or prove the uploader owns all rights.[1]
- **Search/browse:** `scsearch` uses `search/tracks`, paginated `collection`/`next_href`. User collections include uploads, reposts, likes and playlists. Playlist entries are resolved individually or supplemented through track lookup where necessary, rather than assuming every embedded entry is complete.[1]
- **Progressive/HLS:** enumerates `media.transcodings`; resolves a transcoding URL to a media URL; distinguishes progressive HTTP, HLS, encrypted-HLS, codec/MIME, quality and bitrate. Preview detection additionally examines `snipped` and preview URL patterns. DRM-marked `ctr-`/`cbc-` formats are skipped and DRM may be reported.[1]
- **Original download:** attempts `/tracks/{id}/download` only when `downloadable` **and** `has_downloads_left` are true; handles original-download 401 as requiring registration/login and 403 as unavailable for that client; accepts `redirectUri`, checks the resulting file, and labels it `Original`.[1]
- **Critical policy difference:** stream formats are enumerated independently of the original-download gate, and preview formats are merely de-prioritized, not categorically forbidden. Therefore normal yt-dlp download success is not an appropriate authorization check for this project's vault import.[1]

Useful ideas to reimplement: normalized provenance, ordered playlist hydration, bounded pagination, explicit preview states, independent playback/download rights, and clear access-refusal handling. Do not copy login internals, dynamic client-ID extraction, encrypted-stream/key handling, or a generic “best audio” download fallback.

### scdl-org/scdl

The current v3 CLI obtains search/account information through `soundcloud-v2`, then invokes `yt_dlp.YoutubeDL`; it adds compatibility flags, output naming, archives/sync, and Mutagen metadata/artwork postprocessing.[5][7][10]
`-s` selects the first search result rather than exposing a rich search UI; playlist title/uploader/index can become album/album-artist/track tags.[7][10]

The default format selector starts at `ba` (best audio). `--only-original` restricts it to `format_id=download`; `--no-original` excludes that format. Consequently its **default** behavior is not “artist-authorized originals only”; streaming extraction and HLS support are largely delegated to yt-dlp.[10] Its setup can replace an invalid configured client ID with a dynamically generated one, so even explicitly supplying an identity is not a fail-closed guarantee in this wrapper.[10]

**Do not adopt wholesale:** dependency churn is partly automated; its debug argument logging can include the supplied auth token in generated yt-dlp arguments; arbitrary forwarded downloader arguments and destructive playlist-sync semantics are poor boundaries for a vault integration.[10][12] Those are source observations, not executed exploit demonstrations.

### yaaaarn/downcloud

A TypeScript/Bun CLI and library. It resolves links against `api-v2`, follows artist `next_href` pages, and hydrates incomplete playlist entries through individual track requests. The reviewed `api.ts`/`client.ts` expose URL resolution and artist/playlist enumeration, not an equivalent general text-search implementation to the desktop's three search modes.[15][17]

For audio it first tries an original only with `downloadable && has_downloads_left && oauthToken`. If no original URL is obtained, it selects HLS (preferring an `fmp4` MIME), resolves it, and hands the URL to FFmpeg. The reviewed audio selector has **no progressive preference**. FFmpeg copies/remuxes streams or transcodes selected output formats and adds title/artist/album/description/URL/artwork metadata.[18]

Its default identity resolver checks environment/cache, then reads SoundCloud web assets for a client ID. User OAuth can come from explicit configuration, environment, or Bun's secrets store. The README's credential guidance is not a substitute for the official delegated OAuth flow.[16][25] This research inspected code describing those mechanisms but **did not execute them or retrieve any credential**.

**Do not adopt wholesale:** original failure leads to persistent HLS output rather than a permission refusal; external FFmpeg arguments may contain OAuth header material; fetched URLs/playlist continuations have no explicit project-style host allowlist in the reviewed functions; artwork uses temporary plaintext files and audio output is an ordinary file.[15][18]
`saveAudio` waits for FFmpeg but does not check its nonzero exit status before printing success, so its console success message alone is not verification.[18]
Its published benchmark is an author's one-machine comparison, not a measurement of this Qt encrypted-vault pipeline.[8]

## 4. Exact license declarations, dependencies, and AGPL implications

The project root [LICENSE](../LICENSE) contains **GNU AGPL version 3**. The distinctions below concern software reuse/distribution, **not rights to SoundCloud recordings**. This is engineering due diligence, not legal advice.

| Component | Retrieved license evidence | Dependency/runtime evidence | Recommendation for this AGPLv3 project |
| --- | --- | --- | --- |
| yt-dlp source | `LICENSE` is **Unlicense**; `pyproject.toml` declares `Unlicense`. Upstream says repository/PyPI source/wheel contain only Unlicense code.[2][3][4] | Python **>=3.11**; no mandatory Python dependencies in project metadata. Optional default group includes Brotli/brotlicffi, certifi, Mutagen, pycryptodomex, requests, urllib3, websockets, yt-dlp-ejs; separate curl-cffi extra. FFmpeg/ffprobe support postprocessing; YouTube-specific JS tooling is not established as necessary for a SoundCloud-only adapter.[2][4] | Source reuse is not the copyleft blocker, but still track provenance and audit actual bundled dependencies. Prefer studying/reimplementing the small needed concepts rather than shipping a general downloader. |
| yt-dlp standalone binaries | Upstream explicitly says PyInstaller executables include **GPLv3-or-later** code and the combined work is GPLv3+; other release variants have MIT/ISC inclusions. Do not label every release artifact “Unlicense”.[4] | Distribution-specific third-party dependencies and native binaries require their own inventory; FFmpeg license depends on build configuration.[4] | Record the exact artifact/version, notices, corresponding-source route, and bundled native-library licenses before redistribution. A separately spawned executable is not automatically covered by the source package's license declaration. |
| scdl | Root license contains **GPL version 2**; package classifier says GPLv2. The reviewed declarations do **not establish** `GPL-2.0-or-later`.[5][6] | `3.0.9`: Python >=3.10 declared; `docopt-ng>=0.9.0`, `mutagen>=1.47.0`, `soundcloud-v2>=1.6.1`, `yt-dlp[curl-cffi]>=2026.10.21`; FFmpeg documented. Its current yt-dlp minimum itself requires Python >=3.11, so scdl's declared Python floor alone is insufficient.[2][5][7] | **Do not copy/link scdl code into the AGPL combined program without clarifying the grant.** If GPLv2-only, it cannot simply be upgraded to GPLv3/AGPLv3. Generic “or later” sample text in a GPL license appendix is not an author-specific grant. A permissively reimplemented technique avoids this uncertainty. |
| downcloud | Root license contains **GPL version 3**; `package.json` explicitly declares **`GPL-3.0-or-later`**.[9][14] | `2.0.0`: runtime deps `chalk ^5.6.2`, `commander ^15.0.0`; TypeScript `^5` peer; `@types/bun latest`, `@types/commander ^2.12.5` dev deps; Bun APIs and external FFmpeg. Compiled Bun binaries also require runtime/embedded-dependency review.[8][14][18] | GPLv3 code can be combined with AGPLv3 under the explicit combination mechanism in AGPLv3 §13 (see project LICENSE); preserve GPL notices/terms for the GPL portion and satisfy the resulting network/source obligations. This is not permission to copy without attribution or to reuse content. |

**Unresolved dependency scope:** manifests above establish names/version constraints, not a fully audited resolved SBOM. This review did not verify every transitive dependency's license, native FFmpeg build flags, Bun bundled components, or release-binary contents. Do not turn the table into a “distribution cleared” claim.

### Distribution/security checklist

1. **Source obligations:** provide the applicable license texts, copyright/change notices and corresponding source/build instructions for the exact shipped versions under AGPLv3 §§4–6. For covered modified network-service deployments, apply §13's remote-user source offer; that obligation is not triggered merely because a local desktop player makes an outbound API request. GPLv3 combination is expressly addressed in §13; GPLv2 compatibility is not thereby granted. See [project LICENSE](../LICENSE).
2. **Keep software licensing separate from service/content permission:** an Unlicense/GPL/AGPL grant covers software, not SoundCloud API approval, artist copyright, subscription entitlement, caching, export, or redistribution. Do not add terms that contradict recipients' software-license freedoms; keep external service restrictions identified separately.
3. **Avoid downloader defaults:** no web-player client-ID scraping, password/session extraction, browser impersonation as an access remedy, retries with a borrowed identity, stream-to-file fallback, subscription/geo/DRM bypass, or user-controlled arbitrary subprocess flags.
4. **Credentials and network:** approved OAuth only; OS-protected secret storage where needed; never bundle a confidential shared secret. Validate every API/CDN/redirect/HLS/continuation URL, reject non-HTTPS/private/foreign targets as appropriate, separate authorization-header audiences, redact signed URLs, and cap bytes, redirects, pages, segments and retries. Keep cancellation and source ownership explicit.
5. **Media safety:** use the host's constrained HTTP layer and existing decoder seam, not unrestricted FFmpeg network access. No plaintext audio staging for a vault feature; do not persist playlist/private-link secret tokens in provenance URLs. Pin dependencies and verify actual process exit codes and decoded/imported output, not progress messages.

## 5. Read-only assessment of the current desktop adapter

Inspected working-tree source under `qt-app/` and `plugins/soundcloud/`, with repository HEAD `af771438422a8966821e9a975aa94c1450b912a4`. These files include concurrent, uncommitted implementation work; **HEAD is not a content pin for those edits**. Locations below identify functions and the reviewed line ranges; reread before patching. No source was changed and no GUI was launched.

### Serious actual access/rights concerns for integration

**A. Missing supported authentication — live-access/release blocker.** `SoundCloudPage::apiKey`, `promptForKey`, and `apiUrl` (`qt-app/src/soundcloud_page.cpp`, reviewed lines 112–134) store a user-entered `client_id` in `QSettings` and append it to `api-v2` requests. `SoundCloudTransport::get` has no OAuth authorization header or token exchange/refresh flow. This is not the authenticated public-API contract documented by SoundCloud.[24][25] A legitimate client ID alone does not establish that these internal routes will accept it; original download can additionally require a user-authorized session, as yt-dlp's handling confirms.[1] Do not declare provider playback/import operational merely because fixture tests pass. Plain settings are acceptable for a **public identifier only**, not a future client secret/access/refresh token.

**B. `blocked` state is missed — concrete fail-open classification defect.** In `qt-app/src/soundcloud_provider.cpp`, `is_blocked` (reviewed lines 51–55) accepts `policy == "BLOCK"` or `access == "block"`, but the official schema says **`access == "blocked"`**.[24] Given `streamable: true`, no `policy`, and `access: "blocked"`, `parse_track` leaves `blocked=false` and `playback_rights` returns `None`; `download_rights` also returns `None` if `downloadable=true`. This is a static branch analysis, not a live service-bypass demonstration. The server/original endpoint can still refuse access. Fix the state mapping and fail closed on unknown/restricted access before making “never bypasses restrictions” claims.

**C. Preview protection is narrower than the formats consumed.** `is_preview` checks top-level policy/access; `progressive_stream_url`/`hls_stream_url` select transcoding entries without inspecting `snipped` or preview URL indicators. yt-dlp explicitly treats those as additional preview evidence.[1] A restricted rendition could therefore be displayed/played as full audio when top-level metadata does not expose a recognized restriction. This does **not** demonstrate unauthorized full-track recovery, and the current original-only save path reduces the offline risk. Add rendition-level preview classification and preserve refusal states instead of assuming “a URL exists” means full access.

**D. Undocumented original endpoint is mislabeled.** `soundcloud_provider.hpp` calls `download_url` an “official download endpoint”, but its implementation builds `https://api-v2.soundcloud.com/tracks/{id}/download`. That exact exchange is observed in reverse-engineered clients, not declared as a public route in the retrieved official specification.[1][24] Update wording and implement/confirm the authorized public download capability; do not promise that an app identity enables original downloads. `has_downloads_left` is also not captured by the current parser; a true `downloadable` flag alone does not prove present original availability.[1]

### Safeguards actually present (retain them)

- `SoundCloudPage::startImport` (reviewed lines 317–344) re-fetches track metadata, rechecks `download_rights`, calls the original-download endpoint, and refuses when no allowed original URL is returned. **It does not substitute the playing stream.** This is source evidence of a safe decision boundary, not evidence of a successful authorized live import.
- Playback and import are distinct requests/sources; provenance uses provider/track identity. The reviewed HLS path is memory-only and save is disabled for HLS playback. This conservative restriction also prevents an otherwise authorized original download when playback happens to be HLS; that is a functional limitation, not a reason to save HLS instead.
- `SoundCloudTransport::permits/get` enforces HTTPS/443, exact API hosts and one-label `*.sndcdn.com`, manual bounded redirects and response sizes, and cookie suppression. The page only sends client identity/track authorization to API transcoding hosts. No web-player credential scraping was found in the reviewed adapter.
- `SoundCloudHlsPlaylist::parse` rejects encrypted/key-bearing manifests, byte-range/partial/missing segments and non-finite media playlists; `SoundCloudHlsSource` uses bounded memory and custom FFmpeg IO with format/protocol restrictions. Preserve those boundaries rather than importing a generic downloader's unrestricted network path.

## 6. Honest live-verification exit criteria

**Not performed:** authenticated catalogue search; authorized full-track playback; original-download resolution; encrypted import; restart/offline playback; real preview/blocked/expired-token checks. Public source retrieval succeeded; SoundCloud website retrieval did not. No fabricated “live run” or placeholder identity can close this gap.

Before calling the provider complete, obtain a legitimately registered test application through SoundCloud's supported registration process and an explicitly authorized test track/user flow; resolve the native OAuth deployment requirement first.[22][25] Then record sanitized status codes and timings for search, playlist hydration, progressive-if-offered/HLS playback, blocked/preview refusals, fresh original-download authorization, interrupted import cleanup, encrypted readback, and offline replay. Record only public track IDs and redacted error categories—not signed media URLs, secrets or tokens. A refused original must remain refused even when a stream is playable.

## Sources

[1] https://github.com/yt-dlp/yt-dlp/blob/43a45ae37419b2a90db710c9fd06cd250ef524d3/yt_dlp/extractor/soundcloud.py
[2] https://github.com/yt-dlp/yt-dlp/blob/43a45ae37419b2a90db710c9fd06cd250ef524d3/pyproject.toml
[3] https://github.com/yt-dlp/yt-dlp/blob/43a45ae37419b2a90db710c9fd06cd250ef524d3/LICENSE
[4] https://github.com/yt-dlp/yt-dlp/blob/43a45ae37419b2a90db710c9fd06cd250ef524d3/README.md
[5] https://github.com/scdl-org/scdl/blob/282da6c5b87aad313ee98aa8df75da6a40baaeaf/pyproject.toml
[6] https://github.com/scdl-org/scdl/blob/282da6c5b87aad313ee98aa8df75da6a40baaeaf/LICENSE
[7] https://github.com/scdl-org/scdl/blob/282da6c5b87aad313ee98aa8df75da6a40baaeaf/README.md
[8] https://github.com/yaaaarn/downcloud/blob/081b079e7213e3acef40110a5cad8cd7b5e80a09/README.md
[9] https://github.com/yaaaarn/downcloud/blob/081b079e7213e3acef40110a5cad8cd7b5e80a09/LICENSE
[10] https://github.com/scdl-org/scdl/blob/282da6c5b87aad313ee98aa8df75da6a40baaeaf/scdl/scdl.py
[12] https://github.com/scdl-org/scdl/blob/282da6c5b87aad313ee98aa8df75da6a40baaeaf/.github/workflows/update-yt-dlp.yml
[14] https://github.com/yaaaarn/downcloud/blob/081b079e7213e3acef40110a5cad8cd7b5e80a09/package.json
[15] https://github.com/yaaaarn/downcloud/blob/081b079e7213e3acef40110a5cad8cd7b5e80a09/src/api.ts
[16] https://github.com/yaaaarn/downcloud/blob/081b079e7213e3acef40110a5cad8cd7b5e80a09/src/auth.ts
[17] https://github.com/yaaaarn/downcloud/blob/081b079e7213e3acef40110a5cad8cd7b5e80a09/src/client.ts
[18] https://github.com/yaaaarn/downcloud/blob/081b079e7213e3acef40110a5cad8cd7b5e80a09/src/audio.ts
[22] https://github.com/soundcloud/api/blob/33092b81ff8011f1d97441ea6202d176c1cf19c2/README.md
[23] https://github.com/soundcloud/api/blob/33092b81ff8011f1d97441ea6202d176c1cf19c2/openapi/README.md
[24] https://github.com/soundcloud/api/blob/33092b81ff8011f1d97441ea6202d176c1cf19c2/openapi/api.yaml
[25] https://github.com/soundcloud/api/blob/33092b81ff8011f1d97441ea6202d176c1cf19c2/.cursor/skills/soundcloud-api-auth/SKILL.md
[27] https://github.com/yt-dlp/yt-dlp/commit/8bdfbfd4461a643e5c37a232b0efd7bcd86a3091
[28] https://github.com/yt-dlp/yt-dlp/releases/tag/2026.10.21
[30] https://github.com/scdl-org/scdl/releases/tag/v2.4.0
[32] https://github.com/yaaaarn/downcloud/releases/tag/v2.0.0
