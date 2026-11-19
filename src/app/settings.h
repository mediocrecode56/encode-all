// Settings, presets and history, stored as small JSON files in the data folder.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "../core/json.h"
#include "../dl/ytdlp.h"
#include "../media/encode.h"

namespace ea {

std::wstring dataDir();  // portable "data\" beside the exe, else %LOCALAPPDATA%\Encode All
bool isPortable();

struct Settings {
    std::wstring dlDir, convertDir, remuxDir;  // empty convert/remux dir = next to the source
    DlOut dlOut = DlOut::MP4;
    int dlCrf = 16;  // MP4/MOV download quality: 12 extreme, 14 high, 16 normal
    bool dlHevc = false;  // MP4/MOV download codec: H.265 instead of H.264
    bool metadata = true, chapters = true, thumbnail = false, subs = false;
    int fragments = 8;
    std::wstring cookiesBrowser, cookiesFile;
    int maxDownloads = 3, maxEncodes = 1, maxRemux = 2;
    bool lowPriority = true;
    int theme = 0;  // 0 system, 1 dark, 2 light
    bool autoUpdateApp = true, autoUpdateDeps = true, nightlyFallback = true;
    bool keepAudioSource = false;
    ConvertSpec convert;
    Preset convertPreset = Preset::Balanced;
    Container remuxContainer = Container::MP4;
    int remuxAacKbps = 256;
    int winX = INT32_MIN, winY = INT32_MIN, winW = 0, winH = 0;
    bool winMax = false;
    int tab = 0;
    json::Value encoderCaps;  // cached EncoderCaps with the FFmpeg hash it belongs to
    std::string encoderCapsFor;
    int64_t lastAppCheck = 0;

    json::Value toJson() const;
    void fromJson(const json::Value& v);
};

bool loadSettings(Settings& s);
bool saveSettings(const Settings& s);

struct CustomPreset {
    std::wstring name;
    ConvertSpec spec;
};
std::vector<CustomPreset> loadPresets();
bool savePresets(const std::vector<CustomPreset>& p);

struct HistoryItem {
    std::wstring when, title, url, path;
};
void appendHistory(const HistoryItem& h);
std::vector<HistoryItem> loadHistory(size_t max = 500);

// App log: one file, trimmed to 1 MiB on start.
void logLine(const std::wstring& line);
std::vector<std::wstring> recentLog(size_t max);

}  // namespace ea
