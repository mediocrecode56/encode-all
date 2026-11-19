# Changelog

## 1.0.0 - 2026-11-20
- First release: Download (yt-dlp; best streams fetched bit-for-bit; MP4 or MOV with H.264/H.265 at CRF 12/14/16, Original as it comes, audio-only MP3 320/WAV/Original; playlists with thumbnails, ranges and segments), Convert (H.264, H.265, DNxHR LB to 444, FLAC, MP3, WAV; NVENC, Quick Sync, AMF with software fallback), Remux (per-track copy, PCM, AAC, text or exclude, with container advice).
- One queue with pause, resume, cancel, retry, per-kind concurrency and persistence across restarts.
- Tools install and update themselves from official releases, verified by SHA-256, with rollback.
- Self-update through GitHub Releases with automatic recovery from a failed start.
- Monochrome interface: Segoe UI with boxed controls, Consolas for technical detail, dark and light themes, per-monitor DPI.
