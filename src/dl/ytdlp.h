// yt-dlp: info parsing, our own format choice (so the user sees exactly what is downloaded),
// download arguments, progress lines and error classes.
#pragma once
#include <string>
#include <vector>

#include "../media/compat.h"

namespace ea {

struct YtFormat {
    std::string id, ext, vcodec, acodec, protocol, note, lang, dynamicRange;
    int width = 0, height = 0, channels = 0, asr = 0, langPref = -1;
    double fps = 0, tbr = 0, vbr = 0, abr = 0, filesize = 0, quality = 0;
    bool drm = false;
    bool hasVideo() const;
    bool hasAudio() const;
    bool drc() const;
    double bytes(double duration) const;  // filesize, approx, or bitrate x duration
};

struct YtInfo {
    std::string id, title, uploader, webpageUrl, extractor, liveStatus;
    double duration = 0;
    int chapters = 0;
    bool hasSubs = false;
    std::string thumb;
    std::vector<YtFormat> formats;
};

struct YtEntry {
    int index = 0;  // 1-based playlist position
    std::string id, url, title, thumb;
    double duration = 0;
};

struct YtResult {
    bool ok = false;
    bool playlist = false;
    std::wstring error;
    YtInfo info;                  // single video
    std::string playlistTitle;
    std::vector<YtEntry> entries;  // playlist
};

YtResult parseYtJson(const std::string& json);
std::string pickThumb(const std::vector<std::pair<std::string, int>>& urlsWidths);  // nearest to 320 px, JPEG first

// MP4 and MOV: the best streams, then H.264 or H.265 at the chosen CRF (12 extreme, 14 high, 16 normal); sources
// already in that codec are copied. MP4 audio becomes AAC 320 kb/s (AAC is copied); MOV audio stays exact (copy, else PCM).
// Original: the best streams as they come. Audio outputs download no video.
enum class DlOut { MP4, MOV, Original, AudioMP3, AudioWAV, AudioOriginal };
const wchar_t* dlOutName(DlOut o);
bool dlOutIsAudio(DlOut o);

struct DlPrefs {
    DlOut out = DlOut::MP4;
    bool preferH264 = false;  // at equal resolution, frame rate and dynamic range only
    int crf = 16;             // MP4/MOV video quality
    bool hevc = false;        // MP4/MOV target codec: H.265 instead of H.264
    int channels = 0;         // MP4, MOV, MP3, WAV: 0 source, 1 mono, 2 stereo (Original ignores it)
};

struct Selection {
    bool ok = false;
    std::wstring error;
    std::string videoId, audioId;  // audioId empty for a single combined format
    YtFormat v, a;
    Container container = Container::MP4;
    std::wstring ext;              // extension of the bit-exact download (".mkv", ".m4a")
    std::wstring finalExt;         // extension after the optional MP4/MOV conversion
    bool needsConvert = false;
    std::wstring summary;          // "1080p60 HDR10 av1 + opus 160 kb/s -> MP4 (copy)" (log)
    std::wstring brief;            // "1080p60 VP9 + Opus \u2192 MP4 H.264 CRF 16 + AAC" (list row)
    std::vector<std::wstring> notes;
    double estBytes = 0;
    std::string formatSpec() const;  // "399+251"
};

Selection selectFormats(const YtInfo& info, const DlPrefs& prefs);

struct DlOptions {
    std::wstring url, infoJsonPath;  // infoJsonPath wins when set
    std::wstring outDir, outStem;    // final file is outDir\outStem.<ext>
    std::wstring ffmpegDir, deno, pathFile;
    Selection sel;
    bool audioExtract = false;       // -x (copy) for audio-only outputs
    bool metadata = true, chapters = true, thumbnail = false, subs = false;
    std::wstring sectionStart, sectionEnd;  // "" = whole
    std::wstring cookiesBrowser, cookiesFile;
    int fragments = 8;
};

std::vector<std::wstring> buildInfoArgs(const std::wstring& url, bool flatPlaylist, const std::wstring& deno, const DlOptions* auth);
std::vector<std::wstring> buildDownloadArgs(const DlOptions& o);

struct DlProgress {
    bool isDownload = false, isPost = false;
    std::string status, formatId, postprocessor;
    double done = -1, total = -1, speed = -1, eta = -1;
};
bool parseProgressLine(const std::string& line, DlProgress& p);

enum class DlError { None, Extraction, Auth, Unavailable, Geo, Format, Network, Disk, Live, Url, Unknown };
DlError classifyError(const std::string& text, std::wstring& message, std::wstring& hint);

}  // namespace ea
