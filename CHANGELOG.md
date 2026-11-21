# Changelog

## 1.0.5 - 2026-11-21
- Output integrity: before a file gets its name, every stream is checked against the plan (type, codec, size, bit depth, chroma, channels, sample rate) and the start and end of the file must decode. A mismatch fails the task and keeps nothing. Found and fixed with this:
  - odd frame sizes (321x241): x264 and x265 refused them at 4:2:0 and silently rounded them at 4:2:2, and DNxHR silently dropped a row. 4:2:0, 4:2:2 and DNxHR now crop to even sides and say so; 4:4:4 keeps odd sizes;
  - rotated phone clips: resizing used the stored sides, so "720p" of a portrait clip came out distorted. The rotation is applied first;
  - MP3 from 5.1 or 96/192 kHz sources failed: MP3 is now stereo at most and 48 kHz at most, resampled and downmixed with a note;
  - AAC from 192 kHz sources failed: it now uses a rate AAC stores (48 or 44.1 kHz). AAC bitrates follow the channel count (mono half, 5.1 a quarter per channel), so mono no longer asks for more than AAC can carry;
  - a lossless encode with a resize, frame rate, crop, bit depth or chroma change was labelled LOSSLESS; it is LOSSY now;
  - FLAC, MP3 and WAV outputs were never labelled lossless or lossy (always COPY); a 32-bit float WAV of a 24-bit source is exact and labelled so.
  - a decoder crash during that check (seen on this unstable test PC: a good 4K file crashed one decode in five) gets one more try, so a good file is never thrown away; a real decode error still fails.
- Remux: audio can now be converted to AAC, 128, 192, 256 or 320k (labelled lossy; AAC tracks stay copied). Each track's menu has it too.
- Convert filters, each in its own dropdown: PICTURE (size up to 8K or custom, frame rate up to 120 or custom, 8/10-bit 4:2:0/4:2:2/4:4:4, deinterlace, rotate or mirror, denoise, sharpen) and SOUND (stereo or mono downmix, loudness -23/-16/-14 LUFS, sample rate for audio files), with "clear" per row. Audio filters re-encode only the tracks they change; loudness keeps the source sample rate.
- "start all" is now "download all" (convert all, remux all) with its icon; downloads start with a download icon, files with an arrow (play only resumes).
- Rows: more room above, below and between the icons, larger thumbnails, and a short second line ("H.264 1080p60 · AAC stereo · 03:21 · 183 MiB"; for links "1080p60 VP9 + Opus → MP4 H.264 CRF 16 + AAC"). The full plan and log stay in the row's details.
- Fixes: dropping a big folder started one FFprobe per file at once (now four at a time) and listed non-media files (now skipped; an empty drop says so); a probe could see FFmpeg swapped by an update mid-way; the exe carried two manifests (WinLibs' default and ours), now only ours.
- Tests: an integration matrix of edge-case sources (odd sizes, rotation, 10-bit 4:2:2, interlaced, 5.1, 192 kHz, Opus/FLAC/ASS/SRT/chapters, VP9, ALAC with cover) through every Convert target, audio mode and filter, the fastest encoder and every Remux mode; each output is probed independently and decoded end to end.

## 1.0.4 - 2026-11-21
- Readability: text is 13 px Segoe UI and 12 px Consolas (was 12 and 11), the title 17 px; icons are about a third larger (row actions, header, bar), with bigger checkboxes, dropdown marks and window buttons. Thumbnails are larger. Spacing keeps the compact scale, so the window stays as small as 1.0.3.
- Labels no longer cut off: the label column fits "AUDIO ONLY", the download toggles (metadata, chapters...) measure their own checkbox, and Settings gives its labels and notes more room.
- Fix: the playlist RANGE box counts by the playlist numbers the rows show. Before, after an entry was started or removed, "5-10" picked the wrong entries. Entries without a playlist number get their position.
- Space on a failed or canceled task retries it (as its row's button does).

## 1.0.3 - 2026-11-21
- No more queue panel: every section works the same way. A bar at the top takes links (Download; every link on the clipboard is read at once) or files (Convert, Remux: click or drop), the options sit under it, and one list holds each item before and after it starts, newest first. Each row has its own actions: start (or pause, resume, retry), open the link or play the file, show its folder, remove or cancel. "start all" at the top right starts everything that is ready (Ctrl+Enter); pause all, resume all and clear finished act on that section only. A started item stays where it was and keeps its thumbnail.
- A click on a row unfolds its details under it: the segment boxes of a link, the plan and source streams of a conversion, the tracks of a remux, or the result, error, output, log and play/convert/remux of a task. Up/Down select rows, Space starts, pauses or resumes, Delete removes.
- 30% smaller: every size is 0.7 of what it was (fonts 12 px Segoe UI and 11 px Consolas), the window opens at 900x640 and can shrink further.
- Polish: line icons for start, pause, retry, open, folder, link and add; icon buttons name themselves in the header's hint line; section tabs show how many of their tasks are active; files show their extension where downloads show a thumbnail; a hairline progress bar under running rows.
- Removed: the sliding queue animation (there is no queue to slide), the checkboxes on single links and files (each row has its own start and remove).
- Fixes: "start all" no longer drops a playlist whose entries are all unchecked; the playlist range box follows the all/none buttons and the entry checkboxes; the encoder check flag is now thread-safe; a layout can no longer start inside another (an edit's change notice during a layout).
- Cleanup: the queue's own view, its sort and its animation code are gone; one row builder serves all three sections.

## 1.0.2 - 2026-11-21
- Convert: target, quality, speed, audio and filters (size, frame rate, pixel format, one sectioned list) sit in one TARGET row of identical dropdowns, wrapping on narrow windows; subtitles and faststart moved to the SAVE TO row (as on Download) with the encoder at its right. Presets: the four built-ins, then one dropdown to pick, save or delete your own (the dead "custom" cell and the separate save button are gone). Target and MP3 lists are grouped under section titles.
- Remux: container and audio in one row.
- Settings: the queue slides away when Settings opens and back when it closes (160 ms, no flicker; nothing else moves).
- Quitting with running tasks asks in the app's own dialog instead of a Windows message box; edit boxes use the app's own cut/copy/paste menu; the cookies file uses the same file dialog as Add files.
- Icons are anti-aliased with whole-pixel strokes, so the clock, gear and close marks are smooth and the window buttons stay sharp at every scale.
- Fixes: removing a link or file no longer loses or shifts the selection; the playlist range box shows the selected playlist's own range; "check now" never freezes the window while the startup update check waits; finished background threads are released as new ones start (they piled up until exit); text clipped to a negative width no longer spills over its neighbours; the log view repaints only changed lines.
- Cleanup: one helper for notices and one for the file dialog, a dead `.tmp.reserve` delete removed, singular and plural messages ("1 download queued").

## 1.0.1 - 2026-11-20
- Encoder: every encode picks the fastest encoder that works on this PC (NVENC, Quick Sync, AMF, then x264/x265), downloads included. The choice moved out of Convert into Settings (Encoding); presets no longer pin an encoder. Convert shows which encoder will run.
- Remux: MP4 or MOV (MKV removed). New audio choice: Original (copied) or Linear PCM for every track, exact or 16-bit, 24-bit or 32-bit float. AAC is no longer a Remux option.
- Clean log and queue: the integration tests now keep their own data folder (`build	est-data`), so the app no longer shows test tasks, history and log lines; the app log no longer records each start.
- Dropdowns and menus are drawn by the app itself (theme, fonts, check mark, keyboard and wheel), opened under the control instead of at the mouse.
- History, log and settings are icons at the right of the DOWNLOAD / CONVERT / REMUX row. Settings has sections with a label column and short notes; the log sits on a panel with dimmed times; history has WHEN / TITLE / FILE columns and an empty state.
- Every build also copies `EncodeAll.exe` to the project folder, so the latest version is always there to run.
- Polish: one label column for every row, scroll marks on all lists, empty states that say what to do, remove buttons as icons, track actions as dropdowns, file counts beside the action buttons, more even spacing.

## 1.0.0 - 2026-11-20
- First release: Download (yt-dlp; best streams fetched bit-for-bit; MP4 or MOV with H.264/H.265 at CRF 12/14/16, Original as it comes, audio-only MP3 320/WAV/Original; playlists with thumbnails, ranges and segments), Convert (H.264, H.265, DNxHR LB to 444, FLAC, MP3, WAV; NVENC, Quick Sync, AMF with software fallback), Remux (per-track copy, PCM, AAC, text or exclude, with container advice).
- One queue with pause, resume, cancel, retry, per-kind concurrency and persistence across restarts.
- Tools install and update themselves from official releases, verified by SHA-256, with rollback.
- Self-update through GitHub Releases with automatic recovery from a failed start.
- Monochrome interface: Segoe UI with boxed controls, Consolas for technical detail, dark and light themes, per-monitor DPI.
