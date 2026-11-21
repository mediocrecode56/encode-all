# Changelog

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
