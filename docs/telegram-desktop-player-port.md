# Telegram Desktop player source audit and port boundary

## Reference snapshot

The desktop player reference is the user-supplied Telegram Desktop source
snapshot whose `Telegram/build/version` reports **7.3.1**, numeric version
**7003001**. There is no `.git` directory in that snapshot, so a commit hash
cannot be established from it. No Git commit or release equivalence is implied.

The source is not a complete compilable Telegram checkout: `Telegram/lib_ui`,
`Telegram/lib_base`, `Telegram/lib_rpl`, `Telegram/lib_crl`, `Telegram/codegen`,
and `cmake` are empty submodule directories. Their upstream URLs are declared
in `.gitmodules`. Copying `media_player_widget.cpp` into Mega Vault would not
produce a usable widget: it depends on generated styles, reactive UI helpers,
session/history models and Telegram's player instance.

## Actual dependency differences

| Component | Supplied Telegram build declarations | Mega Vault desktop build |
| --- | --- | --- |
| Qt | `Telegram/build/qt_version.py`: Windows x64 defaults to **5.15.19**; optional Qt 6 / ARM uses **6.11.2** | Windows preset pins **5.12.12**; Linux discovers system Qt 5 |
| FFmpeg | `Telegram/build/prepare/prepare.py:1172`: **n8.1.3** | Existing pinned vcpkg build; encrypted Opus regression identifies **7.1.1** |
| Official libopus | `prepare.py:755`: **v1.5.2** | Not linked by the current Windows FFmpeg configuration |
| Audio output | `Telegram/CMakeLists.txt:109`, `media/audio/media_audio.cpp`: **OpenAL**; Windows fork revision `291c0fdbbd` in `prepare.py:1421` | **QAudioOutput**, fed by the existing bounded encrypted audio pipeline |
| UI toolkit | `Telegram/CMakeLists.txt:10–22`: lib_ui/base/rpl/crl, generated styles | Qt widgets plus source-backed controls and bundled Telegram coverage masks |

These are real differences, not aliases. Bundling PNGs or restyling Qt widgets
does **not** switch the audio backend to OpenAL, upgrade FFmpeg, link libopus,
or integrate Telegram's lib_ui. No unused library is added merely to make a
dependency list appear equivalent.

Opus nuance: the reference's **macOS FFmpeg configuration** explicitly enables
the native `opus` decoder (`prepare.py:1246`) and the `libopus` **encoder**
(`prepare.py:1287`). `media_audio_ffmpeg_loader.cpp` uses FFmpeg stream/codec
discovery. The presence of libopus in Telegram's dependency list is not evidence
that every music file is decoded through a libopus wrapper. The Windows
`build_ffmpeg_win.sh` referred to by `prepare.py` is missing from this snapshot;
its actual codec build flags have not been verified. Music is not automatically
transcoded to Opus in this port.

## Source-backed UI reference

Music:
- `media/player/media_player_widget.cpp`: compact bar geometry and interactions.
- `media_player_button.cpp`: transport glyph/animation behavior.
- `media_player_dropdown.cpp`, `media_player_panel.cpp`,
  `media_player_volume_controller.cpp`: menus, hover list, panel and volume.
- `media_player.style`: 35px bar, 24px transport controls, 2px/6px seek line,
  150ms progress animation, 344px panel, 280px maximum list area.

Video:
- `media/view/media_view_playback_controls.cpp` and `media_view_overlay_widget.cpp`.
- `media/view/media_view.style`: 480×72px controller with 9px radius, 3px timeline
  at y=49px, 12px seek handle, 150ms hover transitions, 40px play button,
  32px utility controls and 75px volume area.

Mega Vault must retain its authenticated vault reader, hard-lock revocation,
worker cancellation, one persistent MainWindow-owned music footer, saved
playlists, and the user's requested floating now-playing card. Telegram chat,
message, sharing, cloud download, quality-selection and account actions must
not become nonfunctional buttons in an encrypted local-vault application.

## Desktop window policy

Normal video playback stays **inside the MainWindow at the size the app already
has**. Opening a file must not maximize, fullscreen or resize the window: that
is the user's choice, and an automatic maximize also flickers the library away.
It must not open a second top-level viewer just because the transport chrome is
based on Telegram. Only **Pop out** detaches that same player into the
standalone, frameless media viewer, which opens windowed rather than maximized.
The return control redocks it into the main window. Window transitions must
preserve the decoder and playback clock. Either window can enter/leave
fullscreen without changing the other one's state; the viewer's window controls
overlay the picture instead of reserving a caption row. Dragging a restored
window is distinct from clicking to pause.

"Two players appear" has two distinct causes and both are guarded. Reparenting
a *visible* top-level widget back into the page stack leaves its old native
window mapped on Windows, so `prepareDock()` hides before changing window flags
and clearing `WA_NativeWindow`, and `presentViewer()` hides before detaching.
Separately, removing a *visible* embedded player from the stack leaves its last
painted frame in the stack's backing store, so on pop-out the player is hidden
**and** its surface blanked (`clearSurface()`) before removal, the page beneath
is repainted, and redocking repaints from the retained frame (`repaintFrame()`).
Photos keep their pixels because no worker republishes them. The navigation
regression counts visible top-level players after each transition (embedded: 0,
pop-out: exactly 1, redock: 0) and requires the library page to be current the
moment the player is popped out.

Video timing follows the audio device, not wall time. The picture is paced by
PCM `QAudioOutput` actually pulled from `AudioSink` (`consumed_samples()`), minus
that device's own buffer lead, with the base re-anchored on seek and the seek
epoch incremented so PCM decoded from the old position is discarded rather than
appended under the new timeline. Under a heavy decode (high resolution, slow
scaler) the picture freezes with the sound instead of running ahead, and small
sample-clock differences no longer accumulate over a long file. `video_av_timing`
models rate drift, a 900 ms decode stall, an underrun, seek rebasing, and the
lead clamp; it is a clock-policy test, not an acoustic measurement.

`video_window_navigation` exercises the actual MainWindow embed/pop/redock
routes, instance reuse and hard-lock closure, using an absent media row to
isolate window policy. This is intentionally not a decoding-quality test.
`telegram_video_window` exercises the production controller, seek transaction,
chrome hiding/reveal, standalone/fullscreen state and window dragging. Neither
headless test substitutes for user visual testing.

## Original assets and licensing

`qt-app/resources/telegram_player.qrc` bundles **117 unmodified original PNG
coverage masks**, including 1×, 2× and 3× variants and speed-menu glyphs.
`qt-app/third_party/telegram-player/assets.json` records the SHA-256 of each.
The source stores coverage as grayscale RGB, not as transparency: the shared
`telegram_player_icons.hpp` helper converts that coverage to premultiplied
alpha, applies the requested colour, caches the result and preserves DPI.

Original Telegram GPL-3.0-or-later attribution, full `LICENSE` and `LEGAL` are
preserved under `qt-app/third_party/telegram-player/`. The desktop build stages
these notices beside its executable, and the Windows release/beta and Linux
package recipes retain that directory. Original Telegram portions retain their
license; they are not silently relicensed as new Mega Vault code.

## Verification boundary

The `telegram_player_icons` offscreen regression checks the complete bundled
inventory, grayscale-mask interpretation, opaque and translucent tinting,
antialiased edges, all three DPI scales, actual high-DPI painting and missing
asset/traversal rejection. It has been built and passed on Windows Qt 5.12.

Geometry/input regression coverage and a successful desktop build are necessary
for the control port, but are not evidence of pixel-identical Telegram output,
acoustic equivalence or identical dependencies. Manual visual verification is
reserved for the user; do not launch the desktop GUI automatically. No Linux or
Android runtime parity is inferred from Windows tests.
