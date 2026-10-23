# Opus in the encrypted music path

Opus playback uses the existing FFmpeg decoder and custom AVIO encrypted-range
reader. There is no format conversion at import and no separate Opus player.
Existing formats and dependency versions are unchanged.

## Verified desktop capability

The Windows x64 vcpkg FFmpeg **7.1.1** linked by the application exposes:

- the native `opus` decoder (`avcodec_find_decoder(AV_CODEC_ID_OPUS)`);
- the `ogg` demuxer and Opus parser;
- the native experimental `opus` encoder, used **only to generate test input**;
- **no `libopus` decoder wrapper** in this installed build.

FFmpeg's native decoder is not the official Xiph/libopus reference library.
The experimental flag used for fixture encoding is not needed for playback.
No external libopus dependency was installed or upgraded. An unrelated FFmpeg
executable on PATH is not proof of application support: the regression checks
and uses the libraries actually linked by the target.

Desktop discovery is in `core/CMakeLists.txt`: Windows imported FFmpeg DLL/lib
targets under `MVP_VCPKG_PREFIX`; Linux pkg-config requires libavformat,
libavcodec, libavutil, libswscale and libswresample. `vcpkg.json` retains its
existing features without adding a libopus feature. Other system FFmpeg builds
must include the Ogg demuxer and Opus decoder to play Ogg/Opus.

## Headless regression

```
cmake --build --preset vs2022-x64-debug --target VideoVaultOpusAudioTests
ctest --preset vs2022-x64-debug -R '^opus_audio$' -V
```

`qt-app/tests/opus_audio.cpp` creates a real 90-second stereo 440/660 Hz tone,
encodes it using the linked FFmpeg native Opus encoder and muxes it into Ogg,
all in RAM. The test therefore needs that encoder and Ogg muxer in addition to
the playback decoder/demuxer. A minimal decode-only build need not have the
fixture generator; its absence is a test prerequisite failure, not proof that
Opus playback is unsupported. Android's minimal FFmpeg build does not include
this fixture encoder; this is a desktop Qt test target, not an Android test.

Verified Windows Debug results (build and CTest exit 0):

- 2,176,602 actual encoded bytes imported into a real encrypted vault and read
  back byte-identically, with a separate decoy catalog row.
- The selected encrypted row fed through bounded 4096-byte vault range reads
  into production `MediaDecoder::open_source`: first nonzero PCM after 114,459
  bytes / 30 reads, before reading the entire track. This demonstrates incremental
  encrypted decoding, not a network-download timing test.
- Production `LocalAudioReader::open` independently exercised the ordinary local
  queue path: 8,640,000 stereo s16 samples, RMS 4611, duration 90,002 ms.
- Midpoint seek returned 4,320,240 samples; rewind and decode worked.
- Lock rejected further local PCM reads, seeks, new opens and direct vault ranges.
- No playback-created files or plaintext Ogg/audio staging files in the isolated
  test tree. This is not an OS paging/memory-dump audit or a global filesystem
  monitoring claim.
- Existing `local_audio_pipeline` regression also passed after rebuilding it.

This is PCM/decode evidence, not acoustic audio-device or GUI validation. No GUI
was launched, and the test does not require an audio output device. FFmpeg emits
nonfatal skipped/discarded-sample timestamp warnings on this fixture; decoded
sample counts, duration, nonzero RMS, and seeks are asserted rather than ignored.

## Android configuration

`scripts/android/build-android-core.sh` enables the native `opus` decoder,
`opus` parser and `ogg` demuxer (alongside Matroska/WebM). Those are the FFmpeg
components needed to probe/decode Ogg/Opus; no official libopus wrapper is enabled.
Android audio classification includes `.opus` and `.ogg`. This change verifies
source build flags only: it does not rebuild/install an APK or claim device
playback or currently installed cached native-library contents were tested.
