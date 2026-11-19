#include "encode.h"

#include <algorithm>
#include <cmath>

#include "../core/util.h"

namespace ea {

// ---------- names, presets, json ----------

const wchar_t* presetName(Preset p) {
    switch (p) {
        case Preset::Lossless: return L"lossless";
        case Preset::MaxQuality: return L"max quality";
        case Preset::Balanced: return L"balanced";
        case Preset::Compact: return L"compact";
        case Preset::Custom: return L"custom";
    }
    return L"";
}

const wchar_t* vcodecName(VCodec c) {
    switch (c) {
        case VCodec::Copy: return L"copy";
        case VCodec::H264: return L"H.264";
        case VCodec::H265: return L"H.265";
        case VCodec::DNxHR_LB: return L"DNxHR LB";
        case VCodec::DNxHR_SQ: return L"DNxHR SQ";
        case VCodec::DNxHR_HQ: return L"DNxHR HQ";
        case VCodec::DNxHR_HQX: return L"DNxHR HQX";
        case VCodec::DNxHR_444: return L"DNxHR 444";
    }
    return L"";
}

const wchar_t* accelName(Accel a) {
    switch (a) {
        case Accel::Auto: return L"auto";
        case Accel::Software: return L"software";
        case Accel::NVENC: return L"NVENC";
        case Accel::QSV: return L"Quick Sync";
        case Accel::AMF: return L"AMF";
    }
    return L"";
}

bool isDnxhr(VCodec c) { return c >= VCodec::DNxHR_LB; }

const wchar_t* className(QualityClass c) {
    switch (c) {
        case QualityClass::Copy: return L"STREAM COPY - bit-for-bit, no re-encode";
        case QualityClass::Lossless: return L"LOSSLESS ENCODE - mathematically identical pixels/samples";
        case QualityClass::Lossy: return L"LOSSY ENCODE - smaller, measurably different";
    }
    return L"";
}

void applyPreset(ConvertSpec& s, Preset p) {
    if (s.audio != AudioTarget::None) {
        switch (p) {
            case Preset::Lossless: s.audio = AudioTarget::FLAC; s.flacLevel = 8; s.bitDepth = 0; s.sampleRate = 0; s.channels = 0; break;
            case Preset::MaxQuality: s.audio = AudioTarget::MP3; s.mp3Vbr = false; s.mp3Kbps = 320; break;
            case Preset::Balanced: s.audio = AudioTarget::MP3; s.mp3Vbr = true; s.mp3Quality = 0; break;
            case Preset::Compact: s.audio = AudioTarget::MP3; s.mp3Vbr = true; s.mp3Quality = 4; break;
            case Preset::Custom: break;
        }
        return;
    }
    switch (p) {
        case Preset::Lossless:
            s.vcodec = VCodec::H264; s.rate = RateMode::Lossless; s.accel = Accel::Software; s.speed = "veryfast";
            s.acodec = ACodec::Copy; s.width = s.height = 0; s.fps = 0; s.pixFmt.clear();
            break;
        case Preset::MaxQuality:
            s.vcodec = VCodec::H265; s.rate = RateMode::Quality; s.quality = 16; s.accel = Accel::Software; s.speed = "slow";
            s.acodec = ACodec::Copy; s.aacKbps = 320;
            break;
        case Preset::Balanced:
            s.vcodec = VCodec::H265; s.rate = RateMode::Quality; s.quality = 22; s.accel = Accel::Auto; s.speed = "medium";
            s.acodec = ACodec::Copy; s.aacKbps = 192;
            break;
        case Preset::Compact:
            s.vcodec = VCodec::H265; s.rate = RateMode::Quality; s.quality = 28; s.accel = Accel::Auto; s.speed = "medium";
            s.acodec = ACodec::AAC; s.aacKbps = 128;
            break;
        case Preset::Custom: break;
    }
}

json::Value ConvertSpec::toJson() const {
    json::Value v = json::Value::object();
    v.set("audio", (int)audio);
    v.set("container", (int)container);
    v.set("vcodec", (int)vcodec);
    v.set("accel", (int)accel);
    v.set("rate", (int)rate);
    v.set("quality", quality);
    v.set("mbps", mbps);
    v.set("speed", speed);
    v.set("width", width);
    v.set("height", height);
    v.set("fps", fps);
    v.set("pixFmt", pixFmt);
    v.set("acodec", (int)acodec);
    v.set("aacKbps", aacKbps);
    v.set("pcmBits", pcmBits);
    v.set("flacLevel", flacLevel);
    v.set("mp3Vbr", mp3Vbr);
    v.set("mp3Kbps", mp3Kbps);
    v.set("mp3Quality", mp3Quality);
    v.set("bitDepth", bitDepth);
    v.set("sampleRate", sampleRate);
    v.set("channels", channels);
    v.set("keepSubs", keepSubs);
    v.set("faststart", faststart);
    return v;
}

ConvertSpec ConvertSpec::fromJson(const json::Value& v) {
    ConvertSpec s;
    if (!v.isObj()) return s;
    auto clampEnum = [](double x, int max) { return std::clamp((int)x, 0, max); };
    s.audio = (AudioTarget)clampEnum(v["audio"].num(0), 3);
    s.container = (Container)clampEnum(v["container"].num(0), (int)Container::WEBM);
    s.vcodec = (VCodec)clampEnum(v["vcodec"].num(2), (int)VCodec::DNxHR_444);
    s.accel = (Accel)clampEnum(v["accel"].num(0), 4);
    s.rate = (RateMode)clampEnum(v["rate"].num(0), 2);
    s.quality = std::clamp((int)v["quality"].num(22), 0, 51);
    s.mbps = std::clamp(v["mbps"].num(8), 0.1, 2000.0);
    s.speed = v["speed"].str("medium");
    s.width = std::clamp((int)v["width"].num(0), 0, 16384);
    s.height = std::clamp((int)v["height"].num(0), 0, 16384);
    s.fps = std::clamp(v["fps"].num(0), 0.0, 1000.0);
    s.pixFmt = v["pixFmt"].str();
    s.acodec = (ACodec)clampEnum(v["acodec"].num(0), 2);
    s.aacKbps = std::clamp((int)v["aacKbps"].num(256), 32, 512);
    s.pcmBits = (int)v["pcmBits"].num(0);
    s.flacLevel = std::clamp((int)v["flacLevel"].num(8), 0, 12);
    s.mp3Vbr = v["mp3Vbr"].boolean(false);
    s.mp3Kbps = std::clamp((int)v["mp3Kbps"].num(320), 32, 320);
    s.mp3Quality = std::clamp((int)v["mp3Quality"].num(0), 0, 9);
    s.bitDepth = (int)v["bitDepth"].num(0);
    s.sampleRate = (int)v["sampleRate"].num(0);
    s.channels = (int)v["channels"].num(0);
    s.keepSubs = v["keepSubs"].boolean(true);
    s.faststart = v["faststart"].boolean(true);
    return s;
}

json::Value RemuxSpec::toJson() const {
    json::Value v = json::Value::object();
    v.set("container", (int)container);
    v.set("aacKbps", aacKbps);
    v.set("chapters", chapters);
    v.set("faststart", faststart);
    json::Value a = json::Value::object();
    for (auto& kv : actions) a.set(std::to_string(kv.first), (int)kv.second);
    v.set("actions", a);
    return v;
}

RemuxSpec RemuxSpec::fromJson(const json::Value& v) {
    RemuxSpec s;
    s.container = (Container)std::clamp((int)v["container"].num(0), 0, (int)Container::WEBM);
    s.aacKbps = std::clamp((int)v["aacKbps"].num(256), 32, 512);
    s.chapters = v["chapters"].boolean(true);
    s.faststart = v["faststart"].boolean(true);
    for (auto& kv : v["actions"].o) s.actions[atoi(kv.first.c_str())] = (TrackAction)std::clamp((int)kv.second.num(0), 0, 4);
    return s;
}

// ---------- encoder capabilities ----------

static bool isHw(const std::string& e) {
    return contains(e, "_nvenc") || contains(e, "_qsv") || contains(e, "_amf");
}

bool EncoderCaps::has(const std::string& enc) const {
    auto it = works.find(enc);
    if (it != works.end()) return it->second;
    return !isHw(enc);  // software encoders ship in every build we install
}

static const std::vector<std::string>& fallbackPix(const std::string& enc) {
    static const std::map<std::string, std::vector<std::string>> t = {
        {"libx264", {"yuv420p", "yuvj420p", "yuv422p", "yuvj422p", "yuv444p", "yuvj444p", "nv12", "nv16", "yuv420p10le", "yuv422p10le", "yuv444p10le", "gray", "gray10le"}},
        {"libx265", {"yuv420p", "yuvj420p", "yuv422p", "yuvj422p", "yuv444p", "yuvj444p", "gbrp", "yuv420p10le", "yuv422p10le", "yuv444p10le", "gbrp10le", "yuv420p12le", "yuv422p12le", "yuv444p12le", "gray", "gray10le", "gray12le"}},
        {"h264_nvenc", {"yuv420p", "nv12", "yuv444p"}},
        {"hevc_nvenc", {"yuv420p", "nv12", "p010le", "yuv444p", "p016le", "yuv444p16le"}},
        {"h264_qsv", {"nv12"}},
        {"hevc_qsv", {"nv12", "p010le"}},
        {"h264_amf", {"nv12", "yuv420p"}},
        {"hevc_amf", {"nv12", "yuv420p", "p010le"}},
        {"dnxhd", {"yuv422p", "yuv422p10le", "yuv444p10le", "gbrp10le"}},
    };
    static const std::vector<std::string> none;
    auto it = t.find(enc);
    return it == t.end() ? none : it->second;
}

bool EncoderCaps::supportsPix(const std::string& enc, const std::string& pix) const {
    // H.264 hardware encoders list 10-bit formats that only the newest GPUs accept; keep them 8-bit.
    if ((enc == "h264_nvenc" || enc == "h264_qsv" || enc == "h264_amf") && pixFmtDepth(pix) > 8) return false;
    auto it = pixFmts.find(enc);
    const auto& l = (it != pixFmts.end() && !it->second.empty()) ? it->second : fallbackPix(enc);
    if (l.empty()) return true;
    return std::find(l.begin(), l.end(), pix) != l.end();
}

json::Value EncoderCaps::toJson() const {
    json::Value v = json::Value::object();
    v.set("probed", probed);
    json::Value w = json::Value::object();
    for (auto& kv : works) w.set(kv.first, kv.second);
    v.set("works", w);
    json::Value p = json::Value::object();
    for (auto& kv : pixFmts) {
        json::Value a = json::Value::array();
        for (auto& f : kv.second) a.push(f);
        p.set(kv.first, a);
    }
    v.set("pixFmts", p);
    return v;
}

EncoderCaps EncoderCaps::fromJson(const json::Value& v) {
    EncoderCaps c;
    c.probed = v["probed"].boolean(false);
    for (auto& kv : v["works"].o) c.works[kv.first] = kv.second.boolean(false);
    for (auto& kv : v["pixFmts"].o)
        for (auto& f : kv.second.a) c.pixFmts[kv.first].push_back(f.str());
    return c;
}

std::string pickEncoder(VCodec c, Accel a, RateMode r, const EncoderCaps& caps, std::wstring* why) {
    if (c == VCodec::Copy) return "copy";
    if (isDnxhr(c)) return "dnxhd";
    bool h265 = c == VCodec::H265;
    std::string sw = h265 ? "libx265" : "libx264";
    std::string nv = h265 ? "hevc_nvenc" : "h264_nvenc";
    std::string qs = h265 ? "hevc_qsv" : "h264_qsv";
    std::string am = h265 ? "hevc_amf" : "h264_amf";
    auto note = [&](const std::wstring& t) {
        if (why) *why = t;
    };
    if (r == RateMode::Lossless) {
        if (a == Accel::NVENC && caps.has(nv)) return nv;
        if (a != Accel::Software && a != Accel::Auto) note(L"lossless needs the software encoder (or NVENC); using " + widen(sw));
        return sw;
    }
    switch (a) {
        case Accel::Software: return sw;
        case Accel::NVENC: if (caps.has(nv)) return nv; note(L"NVENC not available here; using " + widen(sw)); return sw;
        case Accel::QSV: if (caps.has(qs)) return qs; note(L"Quick Sync not available here; using " + widen(sw)); return sw;
        case Accel::AMF: if (caps.has(am)) return am; note(L"AMF not available here; using " + widen(sw)); return sw;
        case Accel::Auto:
            if (caps.has(nv)) return nv;
            if (caps.has(qs)) return qs;
            if (caps.has(am)) return am;
            return sw;
    }
    return sw;
}

// Nearest pixel format the encoder accepts, preferring the same bit depth, then the same chroma.
static std::string nearestPix(const std::string& enc, const std::string& want, const EncoderCaps& caps) {
    if (want.empty() || caps.supportsPix(enc, want)) return want;
    int depth = pixFmtDepth(want);
    std::string chroma = pixFmtChroma(want);
    // Same data, different layout: no loss.
    static const std::map<std::string, std::string> repack = {{"yuv420p10le", "p010le"}, {"yuv420p", "nv12"}, {"nv12", "yuv420p"},
                                                              {"p010le", "yuv420p10le"}, {"yuvj420p", "yuv420p"}, {"yuvj422p", "yuv422p"},
                                                              {"yuvj444p", "yuv444p"}, {"yuv444p10le", "yuv444p16le"}};
    auto r = repack.find(want);
    if (r != repack.end() && caps.supportsPix(enc, r->second)) return r->second;
    std::vector<std::string> order;
    auto add = [&](const char* p) { order.push_back(p); };
    if (depth > 8) {
        if (chroma == "444") { add("yuv444p10le"); add("yuv444p16le"); }
        if (chroma == "422" || chroma == "444") { add("yuv422p10le"); }
        add("yuv420p10le"); add("p010le");
    }
    if (chroma == "444") add("yuv444p");
    if (chroma == "422" || chroma == "444") add("yuv422p");
    add("yuv420p"); add("nv12");
    for (auto& p : order)
        if (caps.supportsPix(enc, p)) return p;
    return "yuv420p";
}

double codecEfficiency(const std::string& c) {
    if (c == "hevc") return 0.6;
    if (c == "av1") return 0.5;
    if (c == "vp9") return 0.65;
    if (c == "h264") return 1.0;
    if (c == "mpeg4" || c == "msmpeg4v3" || c == "vc1" || c == "wmv3") return 1.4;
    if (c == "mpeg2video" || c == "mpeg1video") return 2.0;
    if (c == "prores" || c == "dnxhd" || c == "mjpeg" || c == "cfhd" || c == "dvvideo") return 6.0;
    if (c == "rawvideo" || c == "ffv1" || c == "utvideo" || c == "huffyuv") return 20.0;
    return 1.2;
}

static double dnxhrMbps1080p30(VCodec c) {
    switch (c) {
        case VCodec::DNxHR_LB: return 45;
        case VCodec::DNxHR_SQ: return 145;
        case VCodec::DNxHR_HQ: return 220;
        case VCodec::DNxHR_HQX: return 220;
        case VCodec::DNxHR_444: return 440;
        default: return 0;
    }
}

static const char* dnxhrProfile(VCodec c) {
    switch (c) {
        case VCodec::DNxHR_LB: return "dnxhr_lb";
        case VCodec::DNxHR_SQ: return "dnxhr_sq";
        case VCodec::DNxHR_HQ: return "dnxhr_hq";
        case VCodec::DNxHR_HQX: return "dnxhr_hqx";
        case VCodec::DNxHR_444: return "dnxhr_444";
        default: return "";
    }
}

static std::wstring W(const std::string& s) { return widen(s); }
static std::wstring I(int n) { return std::to_wstring(n); }

static std::string pcmCodecFor(const StreamInfo& a, int bits) {
    if (bits == 16) return "pcm_s16le";
    if (bits == 24) return "pcm_s24le";
    if (bits == 32) return "pcm_f32le";
    // Exact: integer sources keep their depth; float decoders (every lossy codec) keep float.
    if (a.lossyAudio() || a.sampleFmt == "flt" || a.sampleFmt == "fltp" || a.sampleFmt == "dbl" || a.sampleFmt == "dblp") return "pcm_f32le";
    int d = a.bitsPerSample;
    if (d == 0) d = (a.sampleFmt == "s16" || a.sampleFmt == "s16p") ? 16 : 24;
    if (d <= 16) return "pcm_s16le";
    if (d <= 24) return "pcm_s24le";
    return "pcm_s32le";
}

static double pcmBitrate(const StreamInfo& a, const std::string& codec, int sr, int ch) {
    int bits = contains(codec, "16") ? 16 : contains(codec, "24") ? 24 : 32;
    return (double)(sr ? sr : a.sampleRate ? a.sampleRate : 48000) * (ch ? ch : a.channels ? a.channels : 2) * bits;
}

static void colorArgs(std::vector<std::wstring>& args, int o, const StreamInfo& v) {
    auto put = [&](const char* opt, const std::string& val) {
        if (val.empty() || val == "unknown" || val == "reserved") return;
        args.push_back(W(std::string("-") + opt + ":" + std::to_string(o)));
        args.push_back(W(val));
    };
    put("color_primaries", v.colorPrimaries);
    put("color_trc", v.colorTransfer);
    put("colorspace", v.colorSpace);
    put("color_range", v.colorRange);
}

static std::string nvencPreset(const std::string& s) {
    if (s == "veryslow" || s == "slower" || s == "placebo") return "p7";
    if (s == "slow") return "p6";
    if (s == "medium") return "p5";
    if (s == "fast") return "p4";
    if (s == "faster") return "p3";
    if (s == "veryfast") return "p2";
    return "p1";
}

// ---------- convert ----------

static Plan planAudioTarget(const MediaInfo& m, const ConvertSpec& s) {
    Plan p;
    int ai = -1;
    for (auto& st : m.streams)
        if (st.isAudio() && (ai < 0 || (st.isDefault && !m.streams[ai].isDefault))) ai = (int)(&st - m.streams.data());
    if (ai < 0) {
        p.error = L"No audio stream in this file";
        return p;
    }
    const StreamInfo& a = m.streams[ai];
    int naudio = m.count("audio");
    if (naudio > 1)
        p.warnings.push_back(L"The file has " + I(naudio) + L" audio tracks; " +
                             (s.audio == AudioTarget::FLAC ? L"FLAC" : s.audio == AudioTarget::MP3 ? L"MP3" : L"WAV") +
                             L" holds one, so only " + describeStream(a) + L" is converted. Use Remux to MKV to keep all.");
    Container c = s.audio == AudioTarget::FLAC ? Container::FLAC : s.audio == AudioTarget::MP3 ? Container::MP3 : Container::WAV;
    p.ext = containerExt(c);
    p.args = {L"-map", L"0:" + I(a.index)};
    int o = 0;
    bool resample = s.sampleRate && s.sampleRate != a.sampleRate;
    bool remix = s.channels && s.channels != a.channels;
    double dur = m.duration > 0 ? m.duration : a.duration;
    double bps = 0;
    bool lossless = !resample && !remix;
    std::wstring line;
    if (s.audio == AudioTarget::MP3) {
        if (a.codec == "mp3" && !resample && !remix) {
            p.args.insert(p.args.end(), {L"-c:0", L"copy"});
            line = L"audio: MP3 copied bit-for-bit (already MP3)";
            p.cls = QualityClass::Copy;
            bps = a.bitrate;
            p.copiedSourceStreams.push_back(a.index);
        } else {
            p.encoder = L"libmp3lame";
            p.args.insert(p.args.end(), {L"-c:0", L"libmp3lame"});
            if (s.mp3Vbr) {
                p.args.insert(p.args.end(), {L"-q:0", I(s.mp3Quality)});
                static const int vbrKbps[] = {245, 225, 190, 175, 165, 130, 115, 100, 85, 65};
                bps = vbrKbps[std::clamp(s.mp3Quality, 0, 9)] * 1000.0;
                p.estVariable = true;
                line = L"audio: MP3 VBR V" + I(s.mp3Quality) + L" (~" + I((int)(bps / 1000)) + L" kb/s)";
            } else {
                p.args.insert(p.args.end(), {L"-b:0", I(s.mp3Kbps) + L"k"});
                bps = s.mp3Kbps * 1000.0;
                line = L"audio: MP3 CBR " + I(s.mp3Kbps) + L" kb/s";
            }
            p.cls = QualityClass::Lossy;
            p.warnings.push_back(L"MP3 is always lossy: no bitrate makes it bit-exact. 320 kb/s CBR is the MP3 maximum. Pick WAV or FLAC for a lossless copy.");
            if (a.lossyAudio() && a.bitrate > 0 && bps > a.bitrate * 1.6)
                p.warnings.push_back(L"The source is lossy at " + fmtBitrate(a.bitrate) + L"; a higher MP3 bitrate cannot restore what it removed.");
            p.args.insert(p.args.end(), {L"-id3v2_version", L"3"});
        }
    } else if (s.audio == AudioTarget::FLAC) {
        p.encoder = L"flac";
        bool srcFloat = a.lossyAudio() || a.sampleFmt == "flt" || a.sampleFmt == "fltp" || a.sampleFmt == "dbl" || a.sampleFmt == "dblp";
        int depth = s.bitDepth ? s.bitDepth : (srcFloat ? 24 : (a.bitsPerSample ? a.bitsPerSample : 16));
        if (depth > 24) depth = 24;
        p.args.insert(p.args.end(), {L"-c:0", L"flac", L"-compression_level:0", I(s.flacLevel)});
        if (depth <= 16) p.args.insert(p.args.end(), {L"-sample_fmt:0", L"s16"});
        else p.args.insert(p.args.end(), {L"-sample_fmt:0", L"s32", L"-bits_per_raw_sample:0", I(depth)});
        bps = (double)(s.sampleRate ? s.sampleRate : a.sampleRate) * (s.channels ? s.channels : a.channels) * depth * 0.6;
        p.estVariable = true;
        line = L"audio: FLAC level " + I(s.flacLevel) + L", " + I(depth) + L"-bit";
        if (a.codec == "flac" && !s.bitDepth && !resample && !remix) {
            line += L" (re-packed losslessly; Copy via Remux is identical and faster)";
        }
        if (srcFloat) {
            lossless = false;
            p.warnings.push_back(L"The source is lossy (decoded as floating point). FLAC stores integers, so samples are rounded to " + I(depth) +
                                 L"-bit: inaudible but not bit-exact. WAV (32-bit float) keeps the decode exact.");
        } else if (a.bitsPerSample && depth < a.bitsPerSample) {
            lossless = false;
            p.warnings.push_back(L"Bit depth reduced from " + I(a.bitsPerSample) + L" to " + I(depth) + L" bits.");
        }
        p.cls = lossless ? QualityClass::Lossless : QualityClass::Lossy;
    } else {
        std::string codec = pcmCodecFor(a, s.bitDepth);
        p.encoder = W(codec);
        p.args.insert(p.args.end(), {L"-c:0", W(codec)});
        bps = pcmBitrate(a, codec, s.sampleRate, s.channels);
        line = L"audio: WAV " + W(codec);
        bool srcFloat = a.lossyAudio() || a.sampleFmt == "flt" || a.sampleFmt == "fltp";
        if (s.bitDepth && s.bitDepth != 32 && (srcFloat || (a.bitsPerSample && s.bitDepth < a.bitsPerSample))) {
            lossless = false;
            p.warnings.push_back(L"Requested " + I(s.bitDepth) + L"-bit rounds the source samples; leave bit depth on 'source' for an exact decode.");
        }
        if (a.lossyAudio()) p.warnings.push_back(L"The source is lossy: WAV is an exact decode, larger than the source, with no quality gain.");
        if (bps * dur / 8 > 4.0e9) p.warnings.push_back(L"WAV is limited to 4 GiB; FFmpeg writes RF64 beyond that, which some players reject.");
        p.cls = lossless ? QualityClass::Lossless : QualityClass::Lossy;
        p.args.insert(p.args.end(), {L"-rf64", L"auto"});
    }
    if (resample) {
        p.args.insert(p.args.end(), {L"-ar:0", I(s.sampleRate)});
        line += L", resampled to " + I(s.sampleRate) + L" Hz";
        p.warnings.push_back(L"Resampling changes every sample (not lossless).");
    }
    if (remix) {
        p.args.insert(p.args.end(), {L"-ac:0", I(s.channels)});
        line += L", " + I(s.channels) + L" channels";
        p.warnings.push_back(L"Channel downmix/upmix is not lossless.");
    }
    if (p.cls == QualityClass::Lossless && !lossless) p.cls = QualityClass::Lossy;
    p.lines.push_back(line);
    o = 1;
    // Cover art: MP3 and FLAC hold one picture.
    for (auto& st : m.streams) {
        if (st.type == "video" && st.attachedPic) {
            if (c != Container::WAV && fitStream(c, st) == Fit::Copy) {
                p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index), L"-c:" + I(o), L"copy", L"-disposition:" + I(o), L"attached_pic"});
                p.lines.push_back(L"cover: copied");
                o++;
            } else {
                p.lines.push_back(L"cover: dropped (" + std::wstring(containerName(c)) + L" cannot hold this picture)");
            }
            break;
        }
    }
    p.args.insert(p.args.end(), {L"-map_metadata", L"0"});
    p.args.insert(p.args.end(), {L"-f", W(containerMuxer(c))});
    p.outStreams = o;
    p.estBytes = bps * dur / 8;
    p.estText = p.estBytes > 0 ? (p.estVariable ? L"~" : L"") + fmtBytes(p.estBytes) + (p.estVariable ? L" (variable bitrate estimate)" : L"") : L"unknown";
    p.ok = true;
    return p;
}

Plan planConvert(const MediaInfo& m, const ConvertSpec& s, const EncoderCaps& caps) {
    if (!m.ok) {
        Plan p;
        p.error = m.error.empty() ? L"File not analysed" : m.error;
        return p;
    }
    if (s.audio != AudioTarget::None) return planAudioTarget(m, s);
    Plan p;
    Container c = s.container;
    if (isDnxhr(s.vcodec) && c != Container::MOV && c != Container::MKV) {
        c = Container::MOV;
        p.warnings.push_back(L"DNxHR goes in MOV (MP4 cannot hold it): output is .mov.");
    }
    p.ext = containerExt(c);
    std::wstring why;
    std::string enc = pickEncoder(s.vcodec, s.accel, s.rate, caps, &why);
    if (!why.empty()) p.warnings.push_back(why);
    double dur = m.duration;
    double bps = 0;  // estimated output bits per second
    bool anyLossy = false, anyEncode = false;
    int o = 0;
    bool haveVideo = false;
    for (const auto& st : m.streams) {
        std::wstring idx = I(o);
        if (st.isVideo()) {
            haveVideo = true;
            if (s.vcodec == VCodec::Copy) {
                if (fitStream(c, st) != Fit::Copy) {
                    p.error = describeStream(st) + L" cannot be copied into " + containerName(c) + L". Pick a codec or use Remux to MKV.";
                    return p;
                }
                p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index), L"-c:" + idx, L"copy"});
                if (st.codec == "hevc" && (c == Container::MP4 || c == Container::MOV)) p.args.insert(p.args.end(), {L"-tag:" + idx, L"hvc1"});
                p.lines.push_back(L"video #" + I(st.index) + L": copied bit-for-bit (" + W(st.codec) + L")");
                p.copiedSourceStreams.push_back(st.index);
                bps += st.bitrate > 0 ? st.bitrate : m.videoBitrate();
                o++;
                continue;
            }
            anyEncode = true;
            p.encoder = W(enc);
            p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index), L"-c:" + idx, W(enc)});
            // Target size and rate.
            int tw = st.width, th = st.height;
            std::vector<std::string> filters;
            if (s.width || s.height) {
                int w2 = s.width, h2 = s.height;
                if (!w2) w2 = st.height ? (int)std::lround(st.width * (double)h2 / st.height / 2) * 2 : -2;
                if (!h2) h2 = st.width ? (int)std::lround(st.height * (double)w2 / st.width / 2) * 2 : -2;
                if (w2 != st.width || h2 != st.height) {
                    filters.push_back("scale=" + std::to_string(w2) + ":" + std::to_string(h2) + ":flags=lanczos");
                    if (w2 > st.width || h2 > st.height) p.warnings.push_back(L"Upscaling adds pixels, not detail; the file grows with no quality gain.");
                    tw = w2;
                    th = h2;
                }
            }
            double tfps = st.fps;
            if (s.fps > 0 && std::fabs(s.fps - st.fps) > 0.01) {
                char b[32];
                snprintf(b, sizeof b, "fps=%.6g", s.fps);
                filters.push_back(b);
                tfps = s.fps;
                if (s.fps > st.fps) p.warnings.push_back(L"Raising the frame rate duplicates frames; motion does not get smoother.");
                else p.warnings.push_back(L"Lowering the frame rate drops frames.");
            }
            if (!filters.empty()) {
                std::string f;
                for (auto& x : filters) f += (f.empty() ? "" : ",") + x;
                p.args.insert(p.args.end(), {L"-filter:" + idx, W(f)});
            }
            // Pixel format.
            std::string wantPix = s.pixFmt.empty() ? st.pixFmt : s.pixFmt;
            if (s.vcodec == VCodec::DNxHR_HQX) wantPix = pixFmtChroma(st.pixFmt) == "444" && s.pixFmt.empty() ? "yuv422p10le" : (s.pixFmt.empty() ? "yuv422p10le" : s.pixFmt);
            else if (s.vcodec == VCodec::DNxHR_444) wantPix = "yuv444p10le";
            else if (isDnxhr(s.vcodec)) wantPix = "yuv422p";
            std::string pix = nearestPix(enc, wantPix, caps);
            if (pix.empty()) pix = "yuv420p";
            int srcDepth = st.bitDepth ? st.bitDepth : pixFmtDepth(st.pixFmt);
            int outDepth = pixFmtDepth(pix);
            std::string srcCh = pixFmtChroma(st.pixFmt), outCh = pixFmtChroma(pix);
            if (srcDepth > outDepth)
                p.warnings.push_back(L"Bit depth drops from " + I(srcDepth) + L" to " + I(outDepth) + L" bits (" + W(enc) + L" + " + W(pix) +
                                     L"). " + (isDnxhr(s.vcodec) ? L"DNxHR HQX or 444 keep 10-bit." : L"x264, x265 or NVENC H.265 keep 10-bit."));
            auto chRank = [](const std::string& ch) { return ch == "444" ? 3 : ch == "422" ? 2 : ch == "420" ? 1 : 0; };
            if (chRank(srcCh) > chRank(outCh) && chRank(outCh) > 0)
                p.warnings.push_back(L"Chroma is subsampled from 4:" + W(srcCh.substr(1)) + L" to 4:" + W(outCh.substr(1)) + L".");
            if (st.hdr() && outDepth < 10) p.warnings.push_back(L"HDR needs 10-bit: this output will look wrong. Use H.265 10-bit.");
            if (st.hdr() && s.vcodec == VCodec::H264) p.warnings.push_back(L"H.264 HDR plays on very few devices; H.265 is the HDR codec.");
            if (pix != st.pixFmt || !filters.empty()) p.args.insert(p.args.end(), {L"-pix_fmt:" + idx, W(pix)});
            // Rate control.
            std::wstring rateText;
            double vbps = 0;
            bool lossless = s.rate == RateMode::Lossless && !isDnxhr(s.vcodec);
            if (isDnxhr(s.vcodec)) {
                p.args.insert(p.args.end(), {L"-profile:" + idx, W(dnxhrProfile(s.vcodec))});
                vbps = dnxhrMbps1080p30(s.vcodec) * 1e6 * ((double)tw * th * (tfps > 0 ? tfps : 30)) / (1920.0 * 1080 * 29.97);
                rateText = L"fixed rate ~" + fmtBitrate(vbps);
                anyLossy = true;
                if (s.rate == RateMode::Lossless) p.warnings.push_back(L"DNxHR is an intermediate codec: visually lossless, not mathematically lossless.");
            } else if (lossless) {
                if (enc == "libx264") p.args.insert(p.args.end(), {L"-preset:" + idx, W(s.speed), L"-qp:" + idx, L"0"});
                else if (enc == "libx265") {}  // lossless goes in x265-params below
                else if (contains(enc, "nvenc")) p.args.insert(p.args.end(), {L"-preset:" + idx, L"p7", L"-tune:" + idx, L"lossless"});
                double raw = (double)tw * th * (tfps > 0 ? tfps : 30) * outDepth * (outCh == "444" ? 3 : outCh == "422" ? 2 : 1.5);
                vbps = raw / 3.0;  // lossless H.264/H.265 is typically 2-4x smaller than raw
                rateText = L"lossless (qp 0)";
                if (st.codec != "rawvideo" && codecEfficiency(st.codec) < 3 && m.videoBitrate() > 0)
                    p.warnings.push_back(L"The source is already compressed (" + W(st.codec) +
                                         L"): lossless output keeps every decoded pixel exactly but is many times larger, with no quality gain. Remux copies it bit-for-bit instead.");
            } else if (s.rate == RateMode::Bitrate) {
                vbps = s.mbps * 1e6;
                std::wstring b = fmtFixed(s.mbps * 1000, 0) + L"k";
                p.args.insert(p.args.end(), {L"-b:" + idx, b, L"-maxrate:" + idx, fmtFixed(s.mbps * 1500, 0) + L"k", L"-bufsize:" + idx, fmtFixed(s.mbps * 2000, 0) + L"k"});
                if (contains(enc, "nvenc")) p.args.insert(p.args.end(), {L"-rc:" + idx, L"vbr"});
                rateText = L"average " + fmtBitrate(vbps);
                anyLossy = true;
                double src = m.videoBitrate();
                if (src > 0) {
                    double outEff = codecEfficiency(s.vcodec == VCodec::H265 ? "hevc" : "h264");
                    double srcEq = src / codecEfficiency(st.codec) * outEff;
                    if (vbps > srcEq * 1.15 && codecEfficiency(st.codec) < 3)
                        p.warnings.push_back(L"Output bitrate " + fmtBitrate(vbps) + L" exceeds what the source needs (" + fmtBitrate(src) + L" " + W(st.codec) +
                                             L" is about " + fmtBitrate(srcEq) + L" in " + vcodecName(s.vcodec) + L"): bigger file, no quality benefit. Quality mode sizes it automatically.");
                }
            } else {
                int q = s.quality;
                if (enc == "libx264" || enc == "libx265") {
                    p.args.insert(p.args.end(), {L"-preset:" + idx, W(s.speed), L"-crf:" + idx, I(q)});
                    rateText = L"CRF " + I(q);
                } else if (contains(enc, "nvenc")) {
                    p.args.insert(p.args.end(), {L"-preset:" + idx, W(nvencPreset(s.speed)), L"-tune:" + idx, L"hq", L"-rc:" + idx, L"vbr", L"-cq:" + idx, I(q + 1), L"-b:" + idx, L"0"});
                    rateText = L"CQ " + I(q + 1);
                } else if (contains(enc, "qsv")) {
                    p.args.insert(p.args.end(), {L"-preset:" + idx, W(s.speed == "ultrafast" || s.speed == "superfast" ? std::string("veryfast") : s.speed), L"-global_quality:" + idx, I(q + 2)});
                    rateText = L"ICQ " + I(q + 2);
                } else if (contains(enc, "amf")) {
                    std::string qual = (s.speed == "slow" || s.speed == "slower" || s.speed == "veryslow") ? "quality" : (s.speed == "medium" ? "balanced" : "speed");
                    p.args.insert(p.args.end(), {L"-quality:" + idx, W(qual), L"-rc:" + idx, L"cqp", L"-qp_i:" + idx, I(q + 2), L"-qp_p:" + idx, I(q + 2), L"-qp_b:" + idx, I(q + 4)});
                    rateText = L"QP " + I(q + 2);
                }
                double bpp = 0.14 * std::pow(2.0, (18.0 - q) / 6.0) * (s.vcodec == VCodec::H265 ? 0.55 : 1.0);
                if (outDepth > 8) bpp *= 1.1;
                vbps = bpp * tw * th * (tfps > 0 ? tfps : 30);
                double src = m.videoBitrate();
                if (src > 0 && codecEfficiency(st.codec) < 3) {
                    double srcEq = src / codecEfficiency(st.codec) * (s.vcodec == VCodec::H265 ? 0.6 : 1.0) * ((double)tw * th / std::max(1, st.width * st.height));
                    vbps = std::clamp(vbps, srcEq * 0.25, srcEq * 1.5);
                }
                p.estVariable = true;
                anyLossy = true;
            }
            if (enc == "libx265") {
                std::string xp = "log-level=error";
                if (lossless) xp += ":lossless=1";
                if (st.hdr() && outDepth >= 10) {
                    xp += ":hdr10=1:repeat-headers=1";
                    if (!st.masterDisplay.empty()) xp += ":master-display=" + st.masterDisplay;
                    if (!st.maxCll.empty()) xp += ":max-cll=" + st.maxCll;
                }
                p.args.insert(p.args.end(), {L"-x265-params:" + idx, W(xp)});
                if (lossless) p.args.insert(p.args.end(), {L"-preset:" + idx, W(s.speed)});
                if (c == Container::MP4 || c == Container::MOV) p.args.insert(p.args.end(), {L"-tag:" + idx, L"hvc1"});
            } else if (contains(enc, "hevc") && (c == Container::MP4 || c == Container::MOV)) {
                p.args.insert(p.args.end(), {L"-tag:" + idx, L"hvc1"});
            }
            if (st.hdr() && isHw(enc)) p.warnings.push_back(L"HDR with a hardware encoder: colour tags are kept; mastering-display metadata depends on the driver. Software x265 writes it explicitly.");
            colorArgs(p.args, o, st);
            if (st.fieldOrder == "tt" || st.fieldOrder == "bb" || st.fieldOrder == "tb" || st.fieldOrder == "bt")
                p.warnings.push_back(L"Interlaced source: fields are kept as woven frames (no deinterlacing).");
            p.lines.push_back(L"video #" + I(st.index) + L": " + W(st.codec) + L" " + I(st.width) + L"x" + I(st.height) + L" -> " + vcodecName(s.vcodec) + L" (" + W(enc) +
                              L") " + I(tw) + L"x" + I(th) + L" " + fmtFps(tfps) + L"fps " + W(pix) + L", " + rateText);
            bps += vbps;
            o++;
        } else if (st.type == "video" && st.attachedPic) {
            if (fitStream(c, st) == Fit::Copy) {
                p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index), L"-c:" + idx, L"copy", L"-disposition:" + idx, L"attached_pic"});
                p.lines.push_back(L"cover #" + I(st.index) + L": copied");
                o++;
            } else {
                p.lines.push_back(L"cover #" + I(st.index) + L": not included (" + containerName(c) + L" cannot hold " + W(st.codec) + L" covers)");
            }
        } else if (st.isAudio()) {
            Fit fit = fitStream(c, st);
            ACodec act = s.acodec;
            std::wstring reason;
            if (act == ACodec::Copy && fit != Fit::Copy) {
                act = ACodec::PCM;
                reason = L" (" + std::wstring(containerName(c)) + L" cannot copy " + W(st.codec) + L"; PCM is the exact decode, AAC would be smaller but lossy)";
            }
            if (act == ACodec::PCM && c == Container::MKV) {}  // MKV holds PCM too
            p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index)});
            if (act == ACodec::Copy) {
                p.args.insert(p.args.end(), {L"-c:" + idx, L"copy"});
                p.lines.push_back(L"audio #" + I(st.index) + L": copied bit-for-bit (" + W(st.codec) + L")");
                p.copiedSourceStreams.push_back(st.index);
                bps += st.bitrate > 0 ? st.bitrate : 192000;
            } else if (act == ACodec::AAC) {
                int ch = st.channels > 8 ? 8 : st.channels;
                p.args.insert(p.args.end(), {L"-c:" + idx, L"aac", L"-b:" + idx, I(s.aacKbps) + L"k"});
                if (ch != st.channels) p.args.insert(p.args.end(), {L"-ac:" + idx, I(ch)});
                p.lines.push_back(L"audio #" + I(st.index) + L": " + W(st.codec) + L" -> AAC " + I(s.aacKbps) + L" kb/s" + reason);
                anyLossy = true;
                anyEncode = true;
                if (st.lossyAudio() && st.bitrate > 0 && s.aacKbps * 1000.0 > st.bitrate * 1.3 && st.codec != "ac3" && st.codec != "dts")
                    p.warnings.push_back(L"Audio #" + I(st.index) + L": AAC " + I(s.aacKbps) + L" kb/s is above the lossy source's " + fmtBitrate(st.bitrate) +
                                         L"; no quality gain." + (fit == Fit::Copy ? L" Copy keeps it bit-for-bit." : L""));
                else if (st.lossyAudio() && fit == Fit::Copy)
                    p.warnings.push_back(L"Audio #" + I(st.index) + L" is already lossy (" + W(st.codec) + L"); AAC adds generation loss. Copy keeps it bit-for-bit.");
                bps += s.aacKbps * 1000.0;
            } else {
                std::string pc = pcmCodecFor(st, s.pcmBits);
                p.args.insert(p.args.end(), {L"-c:" + idx, W(pc)});
                p.lines.push_back(L"audio #" + I(st.index) + L": " + W(st.codec) + L" -> " + W(pc) + reason);
                anyEncode = true;
                bool exact = s.pcmBits == 0 || s.pcmBits == 32 || (!st.lossyAudio() && st.bitsPerSample && s.pcmBits >= st.bitsPerSample);
                if (!exact) {
                    anyLossy = true;
                    p.warnings.push_back(L"Audio #" + I(st.index) + L": " + I(s.pcmBits) + L"-bit PCM rounds the samples; 'exact' keeps them identical.");
                }
                bps += pcmBitrate(st, pc, 0, 0);
            }
            o++;
        } else if (st.isSub()) {
            if (!s.keepSubs) {
                p.lines.push_back(L"subtitle #" + I(st.index) + L": excluded (subtitles off)");
                continue;
            }
            Fit fit = fitStream(c, st);
            if (fit == Fit::No) {
                p.lines.push_back(L"subtitle #" + I(st.index) + L": NOT included");
                p.warnings.push_back(L"Subtitle #" + I(st.index) + L" (" + W(st.codec) + L", image-based) cannot go in " + containerName(c) +
                                     L". Remux to MKV keeps it with no compromise.");
                continue;
            }
            p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index)});
            if (fit == Fit::Copy) {
                p.args.insert(p.args.end(), {L"-c:" + idx, L"copy"});
                p.lines.push_back(L"subtitle #" + I(st.index) + L": copied (" + W(st.codec) + L")");
            } else {
                const wchar_t* to = c == Container::MKV ? L"srt" : L"mov_text";
                p.args.insert(p.args.end(), {L"-c:" + idx, to});
                p.lines.push_back(L"subtitle #" + I(st.index) + L": " + W(st.codec) + L" -> " + to + L" (text kept, styling lost)");
            }
            o++;
        } else if (st.type == "attachment") {
            if (attachmentsFit(c)) {
                p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index)});
                p.lines.push_back(L"attachment #" + I(st.index) + L": copied");
                o++;
            } else {
                p.lines.push_back(L"attachment #" + I(st.index) + L" (" + W(st.filename) + L"): NOT included (" + containerName(c) + L" has no attachments)");
                p.warnings.push_back(L"Attachment " + W(st.filename) + L" (usually a subtitle font) is left out: only MKV holds attachments.");
            }
        } else if (st.codec == "bin_data" && m.chapters) {
            p.lines.push_back(L"data #" + I(st.index) + L": chapter text track (the chapters themselves are carried below)");
        } else {
            p.lines.push_back(W(st.type) + L" #" + I(st.index) + L" (" + W(st.codec) + L"): NOT included (data streams are not carried)");
        }
    }
    if (!haveVideo) {
        p.error = L"No video stream: pick an audio output (FLAC, MP3 or WAV).";
        return p;
    }
    p.args.insert(p.args.end(), {L"-map_metadata", L"0"});
    if (m.chapters) {
        if (chaptersFit(c)) {
            p.args.insert(p.args.end(), {L"-map_chapters", L"0"});
            p.lines.push_back(I(m.chapters) + L" chapters: kept");
        } else {
            p.lines.push_back(I(m.chapters) + L" chapters: NOT included");
        }
    }
    if ((c == Container::MP4 || c == Container::MOV) && s.faststart) p.args.insert(p.args.end(), {L"-movflags", L"+faststart"});
    p.args.insert(p.args.end(), {L"-max_muxing_queue_size", L"4096", L"-f", W(containerMuxer(c))});
    p.outStreams = o;
    p.cls = !anyEncode ? QualityClass::Copy : anyLossy ? QualityClass::Lossy : QualityClass::Lossless;
    p.estBytes = bps * dur / 8 * 1.01;
    if (p.estBytes > 0) {
        if (s.rate == RateMode::Lossless && !isDnxhr(s.vcodec)) p.estText = L"~" + fmtBytes(p.estBytes) + L" (lossless: very rough, content-dependent)";
        else if (p.estVariable) p.estText = L"~" + fmtBytes(p.estBytes) + L" (variable bitrate: may differ by +/-50%)";
        else p.estText = L"~" + fmtBytes(p.estBytes);
    } else {
        p.estText = L"unknown";
    }
    p.ok = true;
    return p;
}

// ---------- remux ----------

const wchar_t* trackActionName(TrackAction a) {
    switch (a) {
        case TrackAction::Copy: return L"COPY";
        case TrackAction::ToAAC: return L"AAC";
        case TrackAction::ToPCM: return L"PCM";
        case TrackAction::ToText: return L"TEXT";
        case TrackAction::Exclude: return L"EXCLUDE";
    }
    return L"";
}

TrackAction defaultAction(Container c, const StreamInfo& s) {
    Fit f = fitStream(c, s);
    if (f == Fit::Copy) return TrackAction::Copy;
    if (f == Fit::Convert) {
        if (s.isAudio()) return TrackAction::ToPCM;
        if (s.isSub()) return TrackAction::ToText;
    }
    return TrackAction::Exclude;
}

std::vector<TrackAction> allowedActions(Container c, const StreamInfo& s) {
    std::vector<TrackAction> r;
    if (fitStream(c, s) == Fit::Copy) r.push_back(TrackAction::Copy);
    if (s.isAudio()) {
        r.push_back(TrackAction::ToPCM);
        r.push_back(TrackAction::ToAAC);
    }
    if (s.isSub() && s.textSub() && fitStream(c, s) != Fit::Copy) r.push_back(TrackAction::ToText);
    r.push_back(TrackAction::Exclude);
    return r;
}

Plan planRemux(const MediaInfo& m, const RemuxSpec& s) {
    Plan p;
    if (!m.ok) {
        p.error = m.error.empty() ? L"File not analysed" : m.error;
        return p;
    }
    Container c = s.container;
    p.ext = containerExt(c);
    int o = 0;
    bool anyLossy = false, anyEncode = false;
    double bps = 0;
    for (const auto& st : m.streams) {
        auto it = s.actions.find(st.index);
        TrackAction a = it != s.actions.end() ? it->second : defaultAction(c, st);
        auto allowed = allowedActions(c, st);
        if (std::find(allowed.begin(), allowed.end(), a) == allowed.end()) a = defaultAction(c, st);
        std::wstring idx = I(o), d = describeStream(st);
        if (a == TrackAction::Exclude && st.codec == "bin_data" && m.chapters) {
            p.lines.push_back(d + L"  ->  chapter text track (chapters are carried separately)");
            continue;
        }
        if (a == TrackAction::Exclude) {
            Fit f = fitStream(c, st);
            p.lines.push_back(d + L"  ->  EXCLUDED" + (f == Fit::No ? std::wstring(L" (") + containerName(c) + L" cannot hold it)" : L" (your choice)"));
            if (f == Fit::No && st.type != "data") p.warnings.push_back(d + L" is left out: " + containerName(c) + L" cannot hold it. MKV keeps it bit-for-bit.");
            continue;
        }
        p.args.insert(p.args.end(), {L"-map", L"0:" + I(st.index)});
        switch (a) {
            case TrackAction::Copy:
                if (st.type != "attachment") p.args.insert(p.args.end(), {L"-c:" + idx, L"copy"});
                if (st.isVideo() && st.codec == "hevc" && (c == Container::MP4 || c == Container::MOV)) p.args.insert(p.args.end(), {L"-tag:" + idx, L"hvc1"});
                if (st.type == "video" && st.attachedPic) p.args.insert(p.args.end(), {L"-disposition:" + idx, L"attached_pic"});
                p.lines.push_back(d + L"  ->  COPY (bit-for-bit)");
                if (st.isVideo() || st.isAudio()) p.copiedSourceStreams.push_back(st.index);
                bps += st.bitrate > 0 ? st.bitrate : (st.isVideo() ? m.videoBitrate() : st.isAudio() ? 192000 : 0);
                break;
            case TrackAction::ToAAC: {
                p.args.insert(p.args.end(), {L"-c:" + idx, L"aac", L"-b:" + idx, I(s.aacKbps) + L"k"});
                if (st.channels > 8) p.args.insert(p.args.end(), {L"-ac:" + idx, L"8"});
                p.lines.push_back(d + L"  ->  AAC " + I(s.aacKbps) + L" kb/s (lossy)");
                anyLossy = anyEncode = true;
                bps += s.aacKbps * 1000.0;
                break;
            }
            case TrackAction::ToPCM: {
                std::string pc = pcmCodecFor(st, 0);
                p.args.insert(p.args.end(), {L"-c:" + idx, W(pc)});
                p.lines.push_back(d + L"  ->  " + W(pc) + L" (exact decode, lossless)");
                anyEncode = true;
                bps += pcmBitrate(st, pc, 0, 0);
                break;
            }
            case TrackAction::ToText: {
                const wchar_t* to = c == Container::MKV ? L"srt" : L"mov_text";
                p.args.insert(p.args.end(), {L"-c:" + idx, to});
                p.lines.push_back(d + L"  ->  " + to + L" (text kept, styling lost)");
                if (st.codec == "ass" || st.codec == "ssa") p.warnings.push_back(d + L": ASS styling, fonts and positions are lost in " + to + L". MKV keeps them.");
                break;
            }
            default: break;
        }
        o++;
    }
    if (o == 0) {
        p.error = L"Every track is excluded: nothing to write.";
        return p;
    }
    p.args.insert(p.args.end(), {L"-map_metadata", L"0"});
    if (m.chapters) {
        if (s.chapters && chaptersFit(c)) {
            p.args.insert(p.args.end(), {L"-map_chapters", L"0"});
            p.lines.push_back(I(m.chapters) + L" chapters  ->  kept");
        } else {
            p.args.insert(p.args.end(), {L"-map_chapters", L"-1"});
            p.lines.push_back(I(m.chapters) + L" chapters  ->  EXCLUDED");
        }
    }
    if ((c == Container::MP4 || c == Container::MOV) && s.faststart) p.args.insert(p.args.end(), {L"-movflags", L"+faststart"});
    p.args.insert(p.args.end(), {L"-max_muxing_queue_size", L"4096", L"-f", W(containerMuxer(c))});
    p.outStreams = o;
    p.cls = !anyEncode ? QualityClass::Copy : anyLossy ? QualityClass::Lossy : QualityClass::Lossless;
    p.estBytes = bps > 0 ? bps * m.duration / 8 * 1.005 : (double)m.size;
    p.estText = L"~" + fmtBytes(p.estBytes);
    p.ok = true;
    return p;
}

Advice adviseConvert(const MediaInfo& m, const ConvertSpec& s) {
    Advice a;
    if (!m.ok) return a;
    const StreamInfo* v = m.video();
    const StreamInfo* au = m.audio();
    if (s.audio != AudioTarget::None) {
        if (!au) {
            a.text = L"No audio stream.";
            return a;
        }
        bool same = (s.audio == AudioTarget::MP3 && au->codec == "mp3") || (s.audio == AudioTarget::FLAC && au->codec == "flac") ||
                    (s.audio == AudioTarget::WAV && startsWith(au->codec, "pcm_"));
        if (same && !s.sampleRate && !s.channels && !s.bitDepth) {
            a.mode = Advice::Copy;
            a.text = L"COPY recommended: the audio is already " + widen(au->codec) + L". Remux copies it bit-for-bit.";
        } else if (au->lossyAudio() && s.audio != AudioTarget::MP3) {
            a.text = L"Source audio is lossy (" + widen(au->codec) + L"): lossless output preserves the decode exactly but cannot add quality.";
        } else if (!au->lossyAudio() && s.audio == AudioTarget::MP3) {
            a.text = L"Source audio is lossless: FLAC keeps it exactly at roughly half the WAV size; MP3 is lossy.";
        } else {
            a.text = L"Convert.";
        }
        return a;
    }
    if (!v) {
        a.text = L"No video stream: choose an audio output.";
        return a;
    }
    bool sameCodec = (s.vcodec == VCodec::H264 && v->codec == "h264") || (s.vcodec == VCodec::H265 && v->codec == "hevc") ||
                     (isDnxhr(s.vcodec) && v->codec == "dnxhd") || s.vcodec == VCodec::Copy;
    bool noChange = !s.width && !s.height && s.fps <= 0 && (s.pixFmt.empty() || s.pixFmt == v->pixFmt);
    int fitAll = 0, total = 0;
    for (auto& st : m.streams) {
        if (st.type == "data") continue;
        total++;
        if (fitStream(s.container, st) == Fit::Copy) fitAll++;
    }
    if (sameCodec && noChange && s.rate != RateMode::Lossless) {
        a.mode = fitAll == total ? Advice::Copy : Advice::Remux;
        a.text = L"REMUX recommended: the video is already " + widen(v->codec) +
                 L". Copying keeps it bit-for-bit, is near-instant, and re-encoding can only lose quality.";
        return a;
    }
    a.mode = Advice::Convert;
    if (fitStream(s.container, *v) == Fit::Copy && noChange)
        a.text = std::wstring(L"CONVERT. (If you only need a different container, Remux copies ") + widen(v->codec) + L" bit-for-bit.)";
    else
        a.text = L"CONVERT: the target codec differs from the source.";
    return a;
}

}  // namespace ea
