# Third-party software

Encode All contains no third-party code. On first start it downloads these programs from their
official GitHub releases into its data folder, checks each against the SHA-256 the project
publishes, and runs them as separate processes. They are not bundled with the installer or the
update packages.

| Program | Source | License |
|---|---|---|
| yt-dlp | https://github.com/yt-dlp/yt-dlp (nightly: yt-dlp/yt-dlp-nightly-builds) | Unlicense; the Windows build bundles components under their own licenses (see its README) |
| FFmpeg, FFprobe | https://github.com/yt-dlp/FFmpeg-Builds (`ffmpeg-master-latest-win64-gpl.zip`) | GPL version 3; source: https://github.com/FFmpeg/FFmpeg and the build scripts at the URL |
| Deno | https://github.com/denoland/deno | MIT |

Each tool's own license file is kept beside it (FFmpeg's `LICENSE.txt` in `deps\ffmpeg\current`).
You can replace or remove them at any time; Encode All reinstalls a missing tool on its next start.
