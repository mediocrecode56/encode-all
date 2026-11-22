# Encode All

A small, fast, native Windows app (C++ and Win32, 1.5 MB, no runtime) to download, convert and
remux video and audio with yt-dlp, FFmpeg and FFprobe. Paste a link or drop a file; it picks the
best source quality and tells you exactly what will happen before anything runs.

```
ENCODE ALL v1.0.7                                                                      _  []  X
[DOWNLOAD 2]  CONVERT   REMUX                                           (history) (log) (settings)
[ (link)  Paste links, or type one and press Enter                                     paste ]
VIDEO       [MP4|MOV|Original]  [H.264|H.265]  QUALITY [extreme|high|normal]
AUDIO ONLY  [MP3 320|WAV|Original]
SOUND       CHANNELS [source (5.1) v]
LINKS  1 ready (~812 MiB)  .  1 running            [pause all]  [download all 1 (v)]
[thumb]  A video title                                  ~812 MiB    (v) (open) (folder) (x)
         03:12  .  2160p60 AV1 + Opus 5.1 -> MP4 H.264 CRF 16 + AAC
[thumb]  Another title                          48%  12 MB/s  00:41  (||) (open) (folder) (x)
         downloading  ________________
```

## Download
- Videos, playlists (thumbnails, durations, checkboxes, ranges like `1-5,8`, start one entry or all checked) and segments (start/end per item).
- The best source streams are always chosen and fetched bit-for-bit: highest resolution, then frame rate, then HDR, then codec (prefer H.264/AAC sources at equal quality if you like).
- **VIDEO**
  - **MP4**: then H.264 or H.265 + AAC 320 kb/s. QUALITY extreme / high / normal = CRF 12 / 14 / 16, preset slow, on the fastest encoder here (see Convert). Streams already in that codec (and AAC audio) are copied untouched.
  - **MOV**: the same video choices, with exact audio (copied, or PCM when MOV cannot hold it). Good for editing.
  - **Original**: the best quality exactly as it comes (VP9, AV1, Opus...), bit-for-bit.
- **AUDIO ONLY** (no video is downloaded): **MP3 320** (labelled lossy; MP3 cannot be bit-exact), **WAV** (exact decode), **Original** (the audio stream bit-for-bit).
- **SOUND**: CHANNELS shows what the link carries (mono, stereo, 5.1) and keeps it (**source**), or makes the audio **stereo** or **mono** for MP4, MOV, MP3 and WAV. Original outputs stay as they come.
- Segments are cut at keyframes with stream copy, so they can start a little early rather than being re-encoded.
- Fast and resilient: 8 parallel fragments, retries, resumable `.part` files, automatic yt-dlp update and retry when a site changes, sign-in cookies from your browser for private or age-restricted videos.

## Convert
- Any input FFmpeg reads. Targets: H.264, H.265 (MP4 or MOV), DNxHR LB, SQ, HQ, HQX, 444 (MOV), copy video, FLAC, MP3, WAV.
- Encoder: automatically the fastest that works on your PC: NVENC, then Quick Sync, then AMF (each tested with a real encode), then software x264/x265. Settings can pin one; lossless stays on x264/x265.
- Separate CRF and Mbps controls: quality or lossless, average bitrate target, or maximum video bitrate cap. The footer estimates the pending outputs. MP4 and MOV always use Faststart.
- Choose a portrait, landscape, source-aspect or custom canvas. Fit keeps the whole image, Fill crops to cover, Stretch changes proportions, and Centered crops or pads without zoom. Preview renders a random frame with the selected filters; it does not simulate compression or HDR playback.
- AAC, exact PCM or copied audio. WAV, FLAC and MP3 hold one track; use a video target or Remux for multi-track input.
- Filters, each its own dropdown: PICTURE size (up to 8K or custom), frame rate, pixel format (8/10-bit, 4:2:0/4:2:2/4:4:4), deinterlace (auto, every frame, double rate), rotate or mirror, denoise, sharpen; SOUND channels (source, stereo or mono; the box names what the files hold, such as "source (5.1)"), loudness (-23, -16 or -14 LUFS), sample rate for audio files.
- Odd frame sizes, rotated phone clips, 192 kHz or surround audio are handled: the plan says what is cropped, turned, resampled or downmixed so the output always plays.
- Presets: lossless, max quality, balanced, compact, plus your own saved presets.
- Every plan is labelled **STREAM COPY**, **LOSSLESS ENCODE** or **LOSSY ENCODE**, lists what happens to each stream, estimates the size (variable-bitrate estimates are marked), warns about bitrates above what the source needs, upscaling, bit-depth or chroma loss, and recommends Remux when the codec already matches.
- Metadata, chapters, colour tags and HDR10 mastering data are carried over.

## Remux
- Change container without re-encoding: MP4 or MOV.
- Audio: **Original** (copied bit-for-bit; exact PCM only where the container cannot hold a track), **Linear PCM** for every track (exact, 16-bit, 24-bit or 32-bit float), or **AAC** 128/192/256/320k (labelled lossy; AAC tracks stay copied; surround gets a rate per channel).
- CHANNELS (as on Download and Convert): **source** keeps every track as it is; **stereo** or **mono** converts the tracks that differ (AAC in MP4, exact PCM in MOV). A track set to COPY stays bit-for-bit.
- Every track can be copied, decoded to PCM, encoded to AAC, converted to text subtitles, or excluded. Incompatibilities are listed before you start (with MOV or MP4 one click away when it keeps everything). Nothing is dropped silently.

## One list per section
Each section has one list: what you added and what is running or finished, newest first. Every row
starts, pauses (the process tree is suspended), resumes, retries, cancels or removes itself, opens its
link or file and shows its folder; "download all" (convert all, remux all) starts everything that is ready. A click on a row shows
its details (plan, tracks, segment, result, log). Concurrency is per kind (Settings); tasks survive
restarts and updates.
Outputs are written under a temporary name and renamed only after every stream is checked against
the plan (codec, size, bit depth, chroma, channels, sample rate, duration) and the start and end decode;
existing files are never overwritten.

## Install and updates
- Run `EncodeAll-Setup-<version>.exe` (per user, no admin). First start downloads yt-dlp, FFmpeg and Deno from their official releases and verifies them; no Python, PATH or manual steps.
- Tools update in the background and never while a task uses them; the previous version is kept for rollback. Settings shows clickable tool warnings, Auto Repair and a report. Rebuild tools asks first and replaces only managed tool files, not your preferences, media, queue or history.
- Encode All updates itself from GitHub Releases (SHA-256 checked) and switches on the next start; if a new version fails to start twice, the previous one comes back.
- Portable mode: put an empty file named `portable` next to `EncodeAll.exe`.

## Keys
`Ctrl+1/2/3` sections, `Enter` read the typed link, `Ctrl+Enter` download / convert / remux all, `Ctrl+O` add files, `Ctrl+V` paste links,
`Ctrl+L` log, `Ctrl+,` settings, `Up/Down` select a row, `Space` start/pause/resume, `F5` retry, `Del` remove (cancel when running), `Esc` close panel.

## Build
Windows with MinGW-w64 GCC 13+, CMake and Ninja: `.\build.ps1 -Test`. `.\build.ps1 -Release` also needs Inno Setup 6.
Tests: `build\unit_tests.exe` (logic, under a second), `build\integration_tests.exe` (real media through every target and mode, and real downloads) and `tests\update-test.ps1` (self-update and rollback).

License: MIT. Third-party programs: see [THIRD_PARTY.md](THIRD_PARTY.md).
