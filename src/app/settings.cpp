#include "settings.h"

#include <algorithm>
#include <windows.h>

#include <deque>

#include "../core/util.h"

namespace ea {

bool isPortable() {
    static int p = -1;
    if (p < 0) p = fileExists(joinPath(dirName(exePath()), L"portable")) ? 1 : 0;
    return p == 1;
}

static std::wstring gDataDir;

void setDataDir(const std::wstring& dir) { gDataDir = dir; }

std::wstring dataDir() {
    std::wstring& d = gDataDir;
    static bool made = false;
    if (d.empty()) {
        d = isPortable() ? joinPath(dirName(exePath()), L"data") : joinPath(localAppData(), L"Encode All");
    }
    if (!made) made = makeDirs(d);
    return d;
}

static std::wstring settingsPath() { return joinPath(dataDir(), L"settings.json"); }

json::Value Settings::toJson() const {
    json::Value v = json::Value::object();
    v.set("dlDir", dlDir);
    v.set("convertDir", convertDir);
    v.set("remuxDir", remuxDir);
    v.set("dlOut", (int)dlOut);
    v.set("dlCrf", dlCrf);
    v.set("dlHevc", dlHevc);
    v.set("metadata", metadata);
    v.set("chapters", chapters);
    v.set("thumbnail", thumbnail);
    v.set("subs", subs);
    v.set("fragments", fragments);
    v.set("cookiesBrowser", cookiesBrowser);
    v.set("cookiesFile", cookiesFile);
    v.set("maxDownloads", maxDownloads);
    v.set("maxEncodes", maxEncodes);
    v.set("maxRemux", maxRemux);
    v.set("lowPriority", lowPriority);
    v.set("theme", theme);
    v.set("autoUpdateApp", autoUpdateApp);
    v.set("autoUpdateDeps", autoUpdateDeps);
    v.set("nightlyFallback", nightlyFallback);
    v.set("keepAudioSource", keepAudioSource);
    v.set("convert", convert.toJson());
    v.set("convertPreset", (int)convertPreset);
    v.set("remuxContainer", (int)remuxContainer);
    v.set("remuxAudio", (int)remuxAudio);
    v.set("remuxAacKbps", remuxAacKbps);
    v.set("remuxPcmBits", remuxPcmBits);
    v.set("encoder", (int)encoder);
    v.set("winX", winX);
    v.set("winY", winY);
    v.set("winW", winW);
    v.set("winH", winH);
    v.set("winMax", winMax);
    v.set("tab", tab);
    v.set("encoderCaps", encoderCaps);
    v.set("encoderCapsFor", encoderCapsFor);
    v.set("lastAppCheck", (long long)lastAppCheck);
    return v;
}

void Settings::fromJson(const json::Value& v) {
    Settings d;
    dlDir = v["dlDir"].wstr();
    convertDir = v["convertDir"].wstr();
    remuxDir = v["remuxDir"].wstr();
    dlOut = (DlOut)std::clamp((int)v["dlOut"].num(0), 0, 5);
    dlCrf = std::clamp((int)v["dlCrf"].num(16), 0, 51);
    dlHevc = v["dlHevc"].boolean(false);
    metadata = v["metadata"].boolean(d.metadata);
    chapters = v["chapters"].boolean(d.chapters);
    thumbnail = v["thumbnail"].boolean(d.thumbnail);
    subs = v["subs"].boolean(d.subs);
    fragments = std::clamp((int)v["fragments"].num(8), 1, 32);
    cookiesBrowser = v["cookiesBrowser"].wstr();
    cookiesFile = v["cookiesFile"].wstr();
    maxDownloads = std::clamp((int)v["maxDownloads"].num(3), 1, 8);
    maxEncodes = std::clamp((int)v["maxEncodes"].num(1), 1, 4);
    maxRemux = std::clamp((int)v["maxRemux"].num(2), 1, 8);
    lowPriority = v["lowPriority"].boolean(true);
    theme = std::clamp((int)v["theme"].num(0), 0, 2);
    autoUpdateApp = v["autoUpdateApp"].boolean(true);
    autoUpdateDeps = v["autoUpdateDeps"].boolean(true);
    nightlyFallback = v["nightlyFallback"].boolean(true);
    keepAudioSource = v["keepAudioSource"].boolean(false);
    if (v["convert"].isObj()) convert = ConvertSpec::fromJson(v["convert"]);
    convertPreset = (Preset)std::clamp((int)v["convertPreset"].num(2), 0, 4);
    remuxContainer = (Container)std::clamp((int)v["remuxContainer"].num(0), 0, 1);  // MP4 or MOV (an old MKV choice becomes MP4)
    // 1.0.4 and earlier stored a PCM switch.
    remuxAudio = v.has("remuxAudio") ? (RemuxAudio)std::clamp((int)v["remuxAudio"].num(0), 0, 2)
                                     : v["remuxPcm"].boolean(false) ? RemuxAudio::PCM : RemuxAudio::Original;
    remuxAacKbps = std::clamp((int)v["remuxAacKbps"].num(256), 64, 512);
    remuxPcmBits = (int)v["remuxPcmBits"].num(0);
    if (remuxPcmBits != 16 && remuxPcmBits != 24 && remuxPcmBits != 32) remuxPcmBits = 0;
    encoder = (Accel)std::clamp((int)v["encoder"].num(0), 0, (int)Accel::AMF);
    winX = (int)v["winX"].num(INT32_MIN);
    winY = (int)v["winY"].num(INT32_MIN);
    winW = (int)v["winW"].num(0);
    winH = (int)v["winH"].num(0);
    winMax = v["winMax"].boolean(false);
    tab = std::clamp((int)v["tab"].num(0), 0, 2);
    encoderCaps = v["encoderCaps"];
    encoderCapsFor = v["encoderCapsFor"].str();
    lastAppCheck = v["lastAppCheck"].integer(0);
}

bool loadSettings(Settings& s) {
    std::string t;
    s = Settings();
    if (readFile(settingsPath(), t)) {
        json::Value v;
        if (json::parse(t, v)) s.fromJson(v);
    }
    if (s.dlDir.empty()) s.dlDir = downloadsFolder();
    return true;
}

bool saveSettings(const Settings& s) { return writeFileAtomic(settingsPath(), json::dump(s.toJson(), true)); }

std::vector<CustomPreset> loadPresets() {
    std::vector<CustomPreset> r;
    std::string t;
    json::Value v;
    if (readFile(joinPath(dataDir(), L"presets.json"), t) && json::parse(t, v))
        for (auto& p : v.a) r.push_back({p["name"].wstr(), ConvertSpec::fromJson(p["spec"])});
    return r;
}

bool savePresets(const std::vector<CustomPreset>& ps) {
    json::Value a = json::Value::array();
    for (auto& p : ps) {
        json::Value o = json::Value::object();
        o.set("name", p.name);
        o.set("spec", p.spec.toJson());
        a.push(o);
    }
    return writeFileAtomic(joinPath(dataDir(), L"presets.json"), json::dump(a, true));
}

static std::mutex gHistMu;

void appendHistory(const HistoryItem& h) {
    std::lock_guard<std::mutex> lk(gHistMu);
    std::wstring p = joinPath(dataDir(), L"history.jsonl");
    json::Value o = json::Value::object();
    o.set("when", h.when);
    o.set("title", h.title);
    o.set("url", h.url);
    o.set("path", h.path);
    std::string line = json::dump(o) + "\n";
    HANDLE f = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, line.data(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(f);
}

std::vector<HistoryItem> loadHistory(size_t max) {
    std::lock_guard<std::mutex> lk(gHistMu);
    std::vector<HistoryItem> r;
    std::string t;
    if (!readFile(joinPath(dataDir(), L"history.jsonl"), t)) return r;
    auto lines = split(t, '\n');
    for (size_t i = lines.size(); i-- > 0 && r.size() < max;) {
        json::Value o;
        if (!json::parse(lines[i], o)) continue;
        r.push_back({o["when"].wstr(), o["title"].wstr(), o["url"].wstr(), o["path"].wstr()});
    }
    return r;
}

static std::mutex gLogMu;
static std::deque<std::wstring> gLogTail;

void logLine(const std::wstring& line) {
    std::lock_guard<std::mutex> lk(gLogMu);
    static bool trimmed = false;
    std::wstring p = joinPath(dataDir(), L"encodeall.log");
    if (!trimmed) {
        trimmed = true;
        if (fileSize(p) > (1u << 20)) {
            std::string t;
            if (readFile(p, t)) writeFileAtomic(p, t.substr(t.size() - (256u << 10)));
        }
    }
    std::wstring full = nowStamp() + L"  " + line;
    gLogTail.push_back(full);
    while (gLogTail.size() > 2000) gLogTail.pop_front();
    std::string u = narrow(full) + "\r\n";
    HANDLE f = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
}

std::vector<std::wstring> recentLog(size_t max) {
    std::lock_guard<std::mutex> lk(gLogMu);
    size_t n = std::min(max, gLogTail.size());
    return std::vector<std::wstring>(gLogTail.end() - n, gLogTail.end());
}

}  // namespace ea
