#include "compat.h"

#include "../core/util.h"

namespace ea {

const wchar_t* containerName(Container c) {
    switch (c) {
        case Container::MP4: return L"MP4";
        case Container::MOV: return L"MOV";
        case Container::MKV: return L"MKV";
        case Container::M4A: return L"M4A";
        case Container::MP3: return L"MP3";
        case Container::FLAC: return L"FLAC";
        case Container::WAV: return L"WAV";
        case Container::OPUS: return L"OPUS";
        case Container::WEBM: return L"WEBM";
    }
    return L"?";
}

const wchar_t* containerExt(Container c) {
    switch (c) {
        case Container::MP4: return L".mp4";
        case Container::MOV: return L".mov";
        case Container::MKV: return L".mkv";
        case Container::M4A: return L".m4a";
        case Container::MP3: return L".mp3";
        case Container::FLAC: return L".flac";
        case Container::WAV: return L".wav";
        case Container::OPUS: return L".opus";
        case Container::WEBM: return L".webm";
    }
    return L"";
}

const char* containerMuxer(Container c) {
    switch (c) {
        case Container::MP4: return "mp4";
        case Container::MOV: return "mov";
        case Container::MKV: return "matroska";
        case Container::M4A: return "ipod";
        case Container::MP3: return "mp3";
        case Container::FLAC: return "flac";
        case Container::WAV: return "wav";
        case Container::OPUS: return "ogg";
        case Container::WEBM: return "webm";
    }
    return "";
}

static bool in(const std::string& c, std::initializer_list<const char*> l) {
    for (auto x : l)
        if (c == x) return true;
    return false;
}

Fit fitStream(Container c, const StreamInfo& s) {
    const std::string& k = s.codec;
    bool pcm = startsWith(k, "pcm_");
    if (s.type == "video" && s.attachedPic) {
        // Cover art: MP4/MOV/M4A/MP3/FLAC hold JPEG/PNG covers, MKV keeps it as an attachment-like stream.
        if (in(k, {"mjpeg", "png"})) return (c == Container::WAV || c == Container::OPUS) ? Fit::No : Fit::Copy;
        return c == Container::MKV ? Fit::Copy : Fit::No;
    }
    switch (c) {
        case Container::MKV:
            if (s.isVideo() || s.isAudio()) return Fit::Copy;
            if (s.isSub()) return k == "mov_text" ? Fit::Convert : Fit::Copy;
            if (s.type == "attachment") return Fit::Copy;
            return Fit::No;  // data streams (tmcd, gpmd) have no Matroska mapping
        case Container::WEBM:
            if (s.isVideo()) return in(k, {"vp8", "vp9", "av1"}) ? Fit::Copy : Fit::No;
            if (s.isAudio()) return in(k, {"opus", "vorbis"}) ? Fit::Copy : Fit::No;
            if (s.isSub()) return k == "webvtt" ? Fit::Copy : (s.textSub() ? Fit::Convert : Fit::No);
            return Fit::No;
        case Container::MP4:
        case Container::MOV: {
            bool mov = c == Container::MOV;
            if (s.isVideo()) {
                if (in(k, {"h264", "hevc", "mpeg4", "mpeg2video", "mpeg1video", "mjpeg"})) return Fit::Copy;
                if (in(k, {"av1", "vp9"})) return mov ? Fit::No : Fit::Copy;
                if (in(k, {"prores", "dnxhd", "qtrle", "rawvideo", "png", "v210", "cineform", "cfhd", "dvvideo"})) return mov ? Fit::Copy : Fit::No;
                return Fit::No;
            }
            if (s.isAudio()) {
                if (in(k, {"aac", "mp3", "mp2", "ac3", "eac3", "alac"})) return Fit::Copy;
                if (pcm) return in(k, {"pcm_s16le", "pcm_s16be", "pcm_s24le", "pcm_s24be", "pcm_s32le", "pcm_s32be", "pcm_f32le", "pcm_f32be", "pcm_f64le"}) ? Fit::Copy : Fit::Convert;
                if (in(k, {"opus", "flac"})) return mov ? Fit::Convert : Fit::Copy;
                if (k == "dts") return Fit::Copy;
                // TrueHD in MP4 is experimental in FFmpeg; Vorbis in MP4 plays almost nowhere.
                // Decoding to PCM is mathematically lossless (exact decoder output).
                return Fit::Convert;
            }
            if (s.isSub()) return s.textSub() ? (k == "mov_text" ? Fit::Copy : Fit::Convert) : Fit::No;
            return Fit::No;
        }
        case Container::M4A:
            if (s.isAudio()) return in(k, {"aac", "alac", "mp3", "ac3", "eac3", "opus", "flac"}) ? Fit::Copy : Fit::Convert;
            return Fit::No;
        case Container::MP3:
            if (s.isAudio()) return k == "mp3" ? Fit::Copy : Fit::Convert;
            return Fit::No;
        case Container::FLAC:
            if (s.isAudio()) return k == "flac" ? Fit::Copy : (s.lossyAudio() ? Fit::Convert : Fit::Convert);
            return Fit::No;
        case Container::WAV:
            if (s.isAudio()) return pcm ? Fit::Copy : Fit::Convert;
            return Fit::No;
        case Container::OPUS:
            if (s.isAudio()) return in(k, {"opus", "vorbis", "flac"}) ? Fit::Copy : Fit::Convert;
            return Fit::No;
    }
    return Fit::No;
}

std::string normalizeYtCodec(const std::string& raw) {
    std::string c = lower(raw);
    if (c.empty() || c == "none" || c == "null") return "";
    if (startsWith(c, "avc") || startsWith(c, "h264")) return "h264";
    if (startsWith(c, "hvc") || startsWith(c, "hev") || startsWith(c, "h265")) return "hevc";
    if (startsWith(c, "av01") || c == "av1") return "av1";
    if (startsWith(c, "vp09") || startsWith(c, "vp9")) return "vp9";
    if (startsWith(c, "vp8")) return "vp8";
    if (startsWith(c, "mp4a") || c == "aac") return "aac";
    if (c == "opus") return "opus";
    if (c == "vorbis") return "vorbis";
    if (c == "mp3" || c == "mp4a.40.34" || c == "mp4a.6b") return "mp3";
    if (startsWith(c, "ac-3") || c == "ac3") return "ac3";
    if (startsWith(c, "ec-3") || c == "eac3") return "eac3";
    if (c == "flac" || c == "alac") return c;
    return c;
}

bool ytCodecFitsMp4(const std::string& vcodec, const std::string& acodec) {
    std::string v = normalizeYtCodec(vcodec), a = normalizeYtCodec(acodec);
    bool vok = v.empty() || in(v, {"h264", "hevc", "av1", "vp9"});
    bool aok = a.empty() || in(a, {"aac", "opus", "mp3", "ac3", "eac3", "flac", "alac"});
    return vok && aok;
}

bool chaptersFit(Container c) { return c == Container::MP4 || c == Container::MOV || c == Container::MKV || c == Container::M4A || c == Container::WEBM; }
bool attachmentsFit(Container c) { return c == Container::MKV; }

ContainerAdvice adviseContainer(Container wanted, const MediaInfo& m) {
    ContainerAdvice a;
    a.best = wanted;
    for (auto& s : m.streams) {
        // Data streams (MP4 chapter text, timecode, telemetry) are carried by none of these
        // containers, so they never decide the container. Chapters travel separately.
        if (s.type == "data") {
            if (!(s.codec == "bin_data" && m.chapters)) a.notes.push_back(describeStream(s) + L": data stream, not carried by any of these containers");
            continue;
        }
        Fit f = fitStream(wanted, s);
        if (f == Fit::Copy) a.copyable++;
        else if (f == Fit::Convert) a.convertible++;
        else a.impossible++;
        if (f == Fit::Copy) continue;
        std::wstring what = describeStream(s);
        if (f == Fit::Convert) {
            if (s.isSub())
                a.notes.push_back(what + L": " + containerName(wanted) + L" needs it converted to mov_text (styling and positioning are lost)");
            else if (s.isAudio())
                a.notes.push_back(what + L": " + containerName(wanted) + L" cannot copy it; it is decoded to PCM (exact, larger)");
            else
                a.notes.push_back(what + L": needs conversion for " + containerName(wanted));
        } else {
            a.notes.push_back(what + L": " + containerName(wanted) + L" cannot hold it");
        }
    }
    if (m.chapters && !chaptersFit(wanted)) a.notes.push_back(std::to_wstring(m.chapters) + L" chapters: " + containerName(wanted) + L" cannot hold chapters");
    if (a.convertible || a.impossible) {
        // Try the other Remux container for a bit-for-bit fit; with none, the wanted one stays.
        for (Container c : {Container::MP4, Container::MOV}) {
            if (c == wanted) continue;
            bool all = true;
            for (auto& s : m.streams)
                if (fitStream(c, s) != Fit::Copy && s.type != "data") all = false;
            if (all) {
                a.best = c;
                break;
            }
        }
    }
    return a;
}

}  // namespace ea
