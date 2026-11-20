# Encode All

A small, fast, native Windows app (C++ and Win32, 1.5 MB, no runtime) to download, convert and
remux video and audio with yt-dlp, FFmpeg and FFprobe. Paste a link or drop a file; it picks the
best source quality and tells you exactly what will happen before anything runs.

```
ENCODE ALL v1.0.1                                                        tools ready   _  []  X
[DOWNLOAD]  CONVERT   REMUX                                             (history) (log) (settings)
VIDEO       [MP4|MOV|Original]  [H.264|H.265]  QUALITY [extreme|high|normal]
AUDIO ONLY  [MP3 320|WAV|Original]
[x] [thumb] 03:12  A video title
             2160p60 av1 + opus -> MP4 (H.264 CRF 16 + AAC 320k)  ~812 MiB
QUEUE  1 running  2 waiting
[=========          ]  48%  DL  LOSSY   A video title        encoding H.264 CRF 16   eta 00:41
```

## Download
- Videos, playlists (thumbnails, durations, checkboxes, ranges like `1-5,8`) and segments (start/end per item).
- The best source streams are always chosen and fetched bit-for-bit: highest resolution, then frame rate, then HDR, then codec (prefer H.264/AAC sources at equal quality if you like).
- **VIDEO**
  - **MP4**: then H.264 or H.265 + AAC 320 kb/s. QUALITY extreme / high / normal = CRF 12 / 14 / 16, preset slow, on the fastest encoder here (see Convert). Streams already in that codec (and AAC audio) are copied untouched.
  - **MOV**: the same video choices, with exact audio (copied, or PCM when MOV cannot hold it). Good for editing.
  - **Original**: the best quality exactly as it comes (VP9, AV1, Opus...), bit-for-bit.
- **AUDIO ONLY** (no video is downloaded): **MP3 320** (labelled lossy; MP3 cannot be bit-exact), **WAV** (exact decode), **Original** (the audio stream bit-for-bit).
- Segments are cut at keyframes with stream copy, so they can start a little early rather than being re-encoded.
- Fast and resilient: 8 parallel fragments, retries, resumable `.part` files, automatic yt-dlp update and retry when a site changes, sign-in cookies from your browser for private or age-restricted videos.

## Convert
- Any input FFmpeg reads. Targets: H.264, H.265 (MP4 or MOV), DNxHR LB, SQ, HQ, HQX, 444 (MOV), copy video, FLAC, MP3, WAV.
- Encoder: automatically the fastest that works on your PC: NVENC, then Quick Sync, then AMF (each tested with a real encode), then software x264/x265. Settings can pin one; lossless stays on x264/x265.
- CRF/CQ, bitrate, lossless; resolution, frame rate, pixel format; AAC, exact PCM or copied audio; FLAC level, MP3 CBR/VBR, bit depth, sample rate, channels.
- Presets: lossless, max quality, balanced, compact, plus your own saved presets.
- Every plan is labelled **STREAM COPY**, **LOSSLESS ENCODE** or **LOSSY ENCODE**, lists what happens to each stream, estimates the size (variable-bitrate estimates are marked), warns about bitrates above what the source needs, upscaling, bit-depth or chroma loss, and recommends Remux when the codec already matches.
- Metadata, chapters, colour tags and HDR10 mastering data are carried over.

## Remux
- Change container without re-encoding: MP4 or MOV.
- Audio: **Original** (copied bit-for-bit; exact PCM only where the container cannot hold a track) or **Linear PCM** for every track (exact, 16-bit, 24-bit or 32-bit float).
- Every track can be copied, decoded to PCM, converted to text subtitles, or excluded. Incompatibilities are listed before you start (with MOV or MP4 one click away when it keeps everything). Nothing is dropped silently.

## Queue
Downloads, conversions and remuxes share one queue with progress, speed, ETA, pause and resume (the
process tree is suspended), cancel, retry, and per-kind concurrency. It survives restarts and updates.
Outputs are written under a temporary name, verified with FFprobe and only then renamed; existing
files are never overwritten.

## Install and updates
- Run `EncodeAll-Setup-<version>.exe` (per user, no admin). First start downloads yt-dlp, FFmpeg and Deno from their official releases and verifies them; no Python, PATH or manual steps.
- Tools update in the background and never while a task uses them; the previous version is kept for one-click rollback.
- Encode All updates itself from GitHub Releases (SHA-256 checked) and switches on the next start; if a new version fails to start twice, the previous one comes back.
- Portable mode: put an empty file named `portable` next to `EncodeAll.exe`.

## Keys
`Ctrl+1/2/3` sections, `Enter` analyse links, `Ctrl+Enter` start, `Ctrl+O` add files, `Ctrl+V` paste links,
`Ctrl+L` log, `Ctrl+,` settings, `Up/Down` select task, `Space` pause/resume, `F5` retry, `Del` remove, `Esc` close panel.

## Build
Windows with MinGW-w64 GCC 13+, CMake and Ninja: `.\build.ps1 -Test`. `.\build.ps1 -Release` also needs Inno Setup 6.
Tests: `build\unit_tests.exe` (logic, under a second), `build\integration_tests.exe` (real media and real downloads) and `tests\update-test.ps1` (self-update and rollback).

License: MIT. Third-party programs: see [THIRD_PARTY.md](THIRD_PARTY.md).
