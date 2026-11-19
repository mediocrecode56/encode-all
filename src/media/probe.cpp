#include "probe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../core/json.h"
#include "../core/process.h"
#include "../core/util.h"

namespace ea {

bool StreamInfo::lossyAudio() const {
    static const char* lossless[] = {"flac", "alac", "truehd", "mlp", "wavpack", "tta", "ape", "shorten", "s302m"};
    if (startsWith(codec, "pcm_")) return false;
    for (auto c : lossless)
        if (codec == c) return false;
    if (codec == "dts" && (profile == "DTS-HD MA" || profile == "DTS-HD MA + DTS:X" || contains(profile, "MA"))) return false;
    return true;
}

bool StreamInfo::textSub() const {
    static const char* text[] = {"subrip", "srt", "ass", "ssa", "webvtt", "mov_text", "text", "microdvd", "subviewer", "jacosub", "sami", "realtext", "stl", "ttml", "mpl2", "pjs", "vplayer"};
    for (auto c : text)
        if (codec == c) return true;
    return false;
}

int pixFmtDepth(const std::string& p) {
    if (p.empty()) return 0;
    if (contains(p, "p16") || contains(p, "64")) return 16;
    if (contains(p, "p14")) return 14;
    if (contains(p, "p12")) return 12;
    if (contains(p, "p10") || p == "p010le" || p == "y210le" || p == "x2rgb10le" || p == "v210") return 10;
    return 8;
}

std::string pixFmtChroma(const std::string& p) {
    if (startsWith(p, "yuv420") || startsWith(p, "yuvj420") || p == "nv12" || p == "p010le" || p == "p016le" || p == "nv21") return "420";
    if (startsWith(p, "yuv422") || startsWith(p, "yuvj422") || p == "y210le" || p == "nv16" || p == "uyvy422" || p == "yuyv422" || p == "v210") return "422";
    if (startsWith(p, "yuv444") || startsWith(p, "yuvj444") || startsWith(p, "gbr") || startsWith(p, "rgb") || startsWith(p, "bgr")) return "444";
    if (startsWith(p, "gray")) return "400";
    return "";
}

static std::string ratioStr(const json::Value& r) {
    // ffprobe gives side data as "num/den" strings or as numbers.
    return r.str();
}

static double fracVal(const std::string& s) { return parseRate(s); }

bool parseProbeJson(const std::string& text, MediaInfo& m) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, &err)) {
        m.error = L"ffprobe output unreadable: " + widen(err);
        return false;
    }
    const auto& f = v["format"];
    m.formatName = f["format_name"].str();
    m.formatLong = f["format_long_name"].str();
    m.duration = f["duration"].num();
    m.bitrate = f["bit_rate"].num();
    m.size = (uint64_t)f["size"].num();
    m.chapters = (int)v["chapters"].size();
    for (const auto& s : v["streams"].a) {
        StreamInfo si;
        si.index = (int)s["index"].num();
        si.type = s["codec_type"].str();
        si.codec = s["codec_name"].str();
        si.codecLong = s["codec_long_name"].str();
        si.profile = s["profile"].str();
        si.width = (int)s["width"].num();
        si.height = (int)s["height"].num();
        si.fpsRaw = s["r_frame_rate"].str();
        double avg = parseRate(s["avg_frame_rate"].str());
        double r = parseRate(si.fpsRaw);
        // r_frame_rate can be a field rate or a timebase artefact (e.g. 1000/1); avg is truer then.
        si.fps = (avg > 0 && (r <= 0 || r > avg * 1.5)) ? avg : r;
        if (avg > 0 && r > avg * 1.5) si.fpsRaw = s["avg_frame_rate"].str();
        si.sar = s["sample_aspect_ratio"].str();
        si.dar = s["display_aspect_ratio"].str();
        si.pixFmt = s["pix_fmt"].str();
        si.colorRange = s["color_range"].str();
        si.colorSpace = s["color_space"].str();
        si.colorTransfer = s["color_transfer"].str();
        si.colorPrimaries = s["color_primaries"].str();
        si.fieldOrder = s["field_order"].str();
        si.bitDepth = (int)s["bits_per_raw_sample"].num();
        if (si.type == "video") {
            int d = pixFmtDepth(si.pixFmt);
            if (d) si.bitDepth = d;
        }
        si.channels = (int)s["channels"].num();
        si.layout = s["channel_layout"].str();
        si.sampleRate = (int)s["sample_rate"].num();
        si.sampleFmt = s["sample_fmt"].str();
        si.bitsPerSample = (int)s["bits_per_raw_sample"].num();
        if (!si.bitsPerSample) si.bitsPerSample = (int)s["bits_per_sample"].num();
        si.bitrate = s["bit_rate"].num();
        si.duration = s["duration"].num();
        const auto& tags = s["tags"];
        if (si.bitrate <= 0) {
            // Matroska writes statistics tags: BPS or BPS-eng.
            for (const auto& kv : tags.o)
                if (lower(kv.first) == "bps" || startsWith(lower(kv.first), "bps-")) si.bitrate = kv.second.num();
        }
        if (si.duration <= 0) {
            for (const auto& kv : tags.o) {
                if (lower(kv.first) == "duration" || startsWith(lower(kv.first), "duration-")) {
                    double t;
                    std::string d = kv.second.str();
                    if (d.size() > 12) d = d.substr(0, 12);
                    if (parseTime(widen(d), t)) si.duration = t;
                }
            }
        }
        for (const auto& kv : tags.o) {
            std::string k = lower(kv.first);
            if (k == "language") si.lang = kv.second.str();
            else if (k == "title") si.title = kv.second.str();
            else if (k == "filename") si.filename = kv.second.str();
            else if (k == "mimetype") si.mimetype = kv.second.str();
            else if (k == "rotate") si.rotation = (int)kv.second.num();
        }
        if (si.lang == "und") si.lang.clear();
        const auto& disp = s["disposition"];
        si.isDefault = disp["default"].num() != 0;
        si.forced = disp["forced"].num() != 0;
        si.attachedPic = disp["attached_pic"].num() != 0;
        for (const auto& sd : s["side_data_list"].a) {
            std::string t = sd["side_data_type"].str();
            if (t == "Display Matrix") {
                si.rotation = (int)std::lround(sd["rotation"].num());
            } else if (t == "Mastering display metadata") {
                // x265 wants G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min) in 0.00002 and 0.0001 units.
                auto c = [&](const char* k, double unit) { return (long long)std::llround(fracVal(ratioStr(sd[k])) / unit); };
                char b[256];
                snprintf(b, sizeof b, "G(%lld,%lld)B(%lld,%lld)R(%lld,%lld)WP(%lld,%lld)L(%lld,%lld)", c("green_x", 0.00002),
                         c("green_y", 0.00002), c("blue_x", 0.00002), c("blue_y", 0.00002), c("red_x", 0.00002), c("red_y", 0.00002),
                         c("white_point_x", 0.00002), c("white_point_y", 0.00002), c("max_luminance", 0.0001), c("min_luminance", 0.0001));
                si.masterDisplay = b;
            } else if (t == "Content light level metadata") {
                si.maxCll = std::to_string((long long)sd["max_content"].num()) + "," + std::to_string((long long)sd["max_average"].num());
            }
        }
        m.streams.push_back(std::move(si));
    }
    if (m.duration <= 0)
        for (auto& s : m.streams) m.duration = std::max(m.duration, s.duration);
    m.ok = true;
    return true;
}

bool probeFile(const std::wstring& ffprobe, const std::wstring& path, MediaInfo& m, const std::atomic<bool>* cancel) {
    m = MediaInfo();
    m.path = path;
    auto c = runCapture(ffprobe, {L"-v", L"error", L"-print_format", L"json", L"-show_format", L"-show_streams", L"-show_chapters", path},
                        {}, 60000, cancel);
    if (!c.started) {
        m.error = c.startError;
        return false;
    }
    if (c.code != 0) {
        std::string e = trim(c.err);
        m.error = L"Not a readable media file" + (e.empty() ? L"" : L": " + widen(e.substr(0, 300)));
        return false;
    }
    if (!parseProbeJson(c.out, m)) return false;
    if (!m.size) m.size = fileSize(path);
    if (m.streams.empty()) {
        m.ok = false;
        m.error = L"No streams found";
        return false;
    }
    // HDR metadata sometimes lives only in the first frame's side data.
    const StreamInfo* v = m.video();
    if (v && v->hdr() && v->masterDisplay.empty()) {
        auto fc = runCapture(ffprobe, {L"-v", L"error", L"-select_streams", L"v:0", L"-read_intervals", L"%+#1", L"-show_frames", L"-show_entries",
                                       L"frame=side_data_list", L"-print_format", L"json", path},
                             {}, 30000, cancel);
        json::Value fv;
        if (fc.code == 0 && json::parse(fc.out, fv)) {
            std::string wrapped = "{\"streams\":[{\"codec_type\":\"video\",\"side_data_list\":" + json::dump(fv["frames"][0]["side_data_list"]) + "}]}";
            MediaInfo tmp;
            if (parseProbeJson(wrapped, tmp) && !tmp.streams.empty()) {
                for (auto& s : m.streams)
                    if (&s == v) {
                        s.masterDisplay = tmp.streams[0].masterDisplay;
                        s.maxCll = tmp.streams[0].maxCll;
                    }
            }
        }
    }
    return true;
}

const StreamInfo* MediaInfo::video() const {
    for (auto& s : streams)
        if (s.isVideo()) return &s;
    return nullptr;
}
const StreamInfo* MediaInfo::audio() const {
    for (auto& s : streams)
        if (s.isAudio()) return &s;
    return nullptr;
}
int MediaInfo::count(const char* type) const {
    int n = 0;
    for (auto& s : streams)
        if (s.type == type && !(s.type == std::string("video") && s.attachedPic)) n++;
    return n;
}

double MediaInfo::videoBitrate() const {
    const StreamInfo* v = video();
    if (!v) return 0;
    if (v->bitrate > 0) return v->bitrate;
    double total = bitrate > 0 ? bitrate : (duration > 0 ? size * 8.0 / duration : 0);
    for (auto& s : streams)
        if (s.isAudio()) total -= s.bitrate > 0 ? s.bitrate : 192000;
    return total > 0 ? total : 0;
}

static std::wstring w(const std::string& s) { return widen(s); }

std::wstring describeStream(const StreamInfo& s) {
    std::wstring r = L"#" + std::to_wstring(s.index) + L" ";
    if (s.isVideo()) {
        r += L"video " + w(s.codec);
        if (!s.profile.empty()) r += L" " + w(s.profile);
        r += L" " + std::to_wstring(s.width) + L"x" + std::to_wstring(s.height);
        if (s.fps > 0) r += L" " + fmtFps(s.fps) + L"fps";
        if (s.bitDepth) r += L" " + std::to_wstring(s.bitDepth) + L"-bit";
        std::string ch = pixFmtChroma(s.pixFmt);
        if (!ch.empty() && ch != "420") r += L" " + w(ch);
        if (s.hdr()) r += s.colorTransfer == "arib-std-b67" ? L" HLG" : L" HDR10";
        else if (s.colorPrimaries == "bt2020") r += L" BT.2020";
        if (s.fieldOrder == "tt" || s.fieldOrder == "bb" || s.fieldOrder == "tb" || s.fieldOrder == "bt") r += L" interlaced";
        if (s.rotation) r += L" rot" + std::to_wstring(s.rotation);
    } else if (s.type == "video" && s.attachedPic) {
        r += L"cover " + w(s.codec);
    } else if (s.isAudio()) {
        r += L"audio " + w(s.codec);
        if (!s.profile.empty() && s.profile != "LC") r += L" " + w(s.profile);
        r += L" " + (s.layout.empty() ? std::to_wstring(s.channels) + L"ch" : w(s.layout));
        if (s.sampleRate) r += L" " + fmtFixed(s.sampleRate / 1000.0, s.sampleRate % 1000 ? 1 : 0) + L"kHz";
        if (!s.lossyAudio() && s.bitsPerSample) r += L" " + std::to_wstring(s.bitsPerSample) + L"-bit";
    } else if (s.isSub()) {
        r += L"subtitle " + w(s.codec) + (s.textSub() ? L" (text)" : L" (image)");
    } else if (s.type == "attachment") {
        r += L"attachment " + (s.filename.empty() ? w(s.codec) : w(s.filename));
    } else {
        r += w(s.type) + L" " + w(s.codec);
    }
    if (s.bitrate > 0 && (s.isVideo() || s.isAudio())) r += L" " + fmtBitrate(s.bitrate);
    if (!s.lang.empty()) r += L" [" + w(s.lang) + L"]";
    if (!s.title.empty()) r += L" \"" + ellipsize(w(s.title), 28) + L"\"";
    if (s.isDefault && !s.isVideo()) r += L" default";
    if (s.forced) r += L" forced";
    return r;
}

std::wstring describeMedia(const MediaInfo& m) {
    std::wstring fmt = w(m.formatName);
    size_t c = fmt.find(L',');
    if (c != std::wstring::npos) fmt = fmt.substr(0, c);
    std::wstring r = fmt + L" | " + fmtDuration(m.duration) + L" | " + fmtBytes((double)m.size);
    if (m.bitrate > 0) r += L" | " + fmtBitrate(m.bitrate);
    if (m.chapters) r += L" | " + std::to_wstring(m.chapters) + L" chapters";
    return r;
}

}  // namespace ea
