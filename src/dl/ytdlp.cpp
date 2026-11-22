#include "ytdlp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../core/json.h"
#include "../core/util.h"

namespace ea {

static bool known(const std::string& c) { return !c.empty() && c != "none" && c != "null"; }

bool YtFormat::hasVideo() const { return known(vcodec) && vcodec != "images"; }
bool YtFormat::hasAudio() const { return known(acodec); }
bool YtFormat::drc() const { return contains(lower(id), "-drc") || contains(note, "DRC"); }

double YtFormat::bytes(double duration) const {
    if (filesize > 0) return filesize;
    if (tbr > 0 && duration > 0) return tbr * 1000 / 8 * duration;
    return 0;
}

std::string pickThumb(const std::vector<std::pair<std::string, int>>& t) {
    // YouTube: the plain mqdefault.jpg is 320x180 JPEG. Its sized "sqp=" variants may arrive as WebP,
    // which Windows without the WebP extension cannot decode.
    for (auto& [url, w] : t) {
        size_t p = url.find("i.ytimg.com/vi");
        if (p == std::string::npos) continue;
        size_t idStart = url.find('/', url.find("/vi", p) + 1);
        if (idStart == std::string::npos) continue;
        size_t idEnd = url.find('/', idStart + 1);
        if (idEnd == std::string::npos) continue;
        return "https://i.ytimg.com/vi/" + url.substr(idStart + 1, idEnd - idStart - 1) + "/mqdefault.jpg";
    }
    std::string best;
    int bestScore = 1 << 30;
    for (auto& [url, w] : t) {
        if (url.empty()) continue;
        bool webp = contains(url, ".webp") || contains(url, "vi_webp");
        int width = w > 0 ? w : 480;  // YouTube's unsized ones are hq (480) or larger
        if (contains(url, "maxres") || contains(url, "hq720") || contains(url, "sddefault")) width = w > 0 ? w : 1280;
        if (contains(url, "mqdefault")) width = 320;
        if (contains(url, "default.jpg") && !contains(url, "hq") && !contains(url, "mq") && !contains(url, "sd")) width = 120;
        int score = std::abs(width - 320) + (webp ? 10000 : 0) + (width < 160 ? 500 : 0);
        if (score < bestScore) {
            bestScore = score;
            best = url;
        }
    }
    return best;
}

static std::string thumbOf(const json::Value& v) {
    std::vector<std::pair<std::string, int>> t;
    for (auto& th : v["thumbnails"].a) t.push_back({th["url"].str(), (int)th["width"].num(0)});
    std::string p = pickThumb(t);
    if (p.empty()) p = v["thumbnail"].str();
    return p;
}

YtResult parseYtJson(const std::string& text) {
    YtResult r;
    json::Value v;
    std::string err;
    if (!json::parse(text, v, &err)) {
        r.error = L"yt-dlp returned unreadable data (" + widen(err) + L")";
        return r;
    }
    if (v["_type"].str() == "playlist" || v.has("entries")) {
        r.playlist = true;
        r.playlistTitle = v["title"].str(v["id"].str());
        int i = 0;
        for (auto& e : v["entries"].a) {
            i++;
            if (e.isNull()) continue;  // deleted/private entries come back as null
            YtEntry en;
            en.index = (int)e["playlist_index"].num(i);
            if (en.index < 1) en.index = i;  // rows, the range box and segments key on it
            en.id = e["id"].str();
            en.url = e["url"].str();
            if (en.url.empty()) en.url = e["webpage_url"].str();
            if (en.url.empty() && e["ie_key"].str() == "Youtube" && !en.id.empty()) en.url = "https://www.youtube.com/watch?v=" + en.id;
            en.title = e["title"].str(en.id);
            en.duration = e["duration"].num(0);
            en.thumb = thumbOf(e);
            if (en.thumb.empty() && e["ie_key"].str() == "Youtube" && !en.id.empty()) en.thumb = "https://i.ytimg.com/vi/" + en.id + "/mqdefault.jpg";
            if (!en.url.empty()) r.entries.push_back(en);
        }
        r.ok = true;
        if (r.entries.empty()) {
            r.ok = false;
            r.error = L"The playlist has no available entries.";
        }
        return r;
    }
    YtInfo& in = r.info;
    in.id = v["id"].str();
    in.title = v["title"].str(in.id);
    in.uploader = v["uploader"].str(v["channel"].str());
    in.webpageUrl = v["webpage_url"].str(v["original_url"].str());
    in.extractor = v["extractor_key"].str(v["extractor"].str());
    in.liveStatus = v["live_status"].str();
    in.duration = v["duration"].num(0);
    in.chapters = (int)v["chapters"].size();
    in.hasSubs = v["subtitles"].size() > 0;
    in.thumb = thumbOf(v);
    const json::Value& fl = v["formats"].isArr() ? v["formats"] : json::Value();
    auto addFmt = [&](const json::Value& f) {
        YtFormat x;
        x.id = f["format_id"].str();
        x.ext = f["ext"].str();
        x.vcodec = f["vcodec"].str();
        x.acodec = f["acodec"].str();
        x.protocol = f["protocol"].str();
        x.note = f["format_note"].str();
        x.lang = f["language"].str();
        x.dynamicRange = f["dynamic_range"].str();
        x.width = (int)f["width"].num(0);
        x.height = (int)f["height"].num(0);
        x.channels = (int)f["audio_channels"].num(0);
        x.asr = (int)f["asr"].num(0);
        x.langPref = (int)f["language_preference"].num(-1);
        x.fps = f["fps"].num(0);
        x.tbr = f["tbr"].num(0);
        x.vbr = f["vbr"].num(0);
        x.abr = f["abr"].num(0);
        x.filesize = f["filesize"].num(f["filesize_approx"].num(0));
        x.quality = f["quality"].num(0);
        x.drm = f["has_drm"].boolean(false);
        // Some extractors leave codecs unset on progressive files: assume both are present.
        if (!f.has("vcodec") && !f.has("acodec") && (x.width || x.height)) x.vcodec = x.acodec = "unknown";
        if (x.ext == "mhtml" || contains(x.note, "storyboard")) return;
        in.formats.push_back(x);
    };
    for (auto& f : fl.a) addFmt(f);
    if (in.formats.empty() && v.has("format_id")) addFmt(v);  // single-format sites
    r.ok = true;
    if (in.liveStatus == "is_live" || in.liveStatus == "is_upcoming") {
        r.ok = false;
        r.error = in.liveStatus == "is_live" ? L"This is a live stream: wait until it ends, then download the recording."
                                             : L"This premiere or live event has not started yet.";
    }
    return r;
}

const wchar_t* dlOutName(DlOut o) {
    switch (o) {
        case DlOut::MP4: return L"MP4";
        case DlOut::MOV: return L"MOV";
        case DlOut::Original: return L"original";
        case DlOut::AudioMP3: return L"MP3 320";
        case DlOut::AudioWAV: return L"WAV";
        case DlOut::AudioOriginal: return L"audio original";
    }
    return L"";
}

bool dlOutIsAudio(DlOut o) { return o == DlOut::AudioMP3 || o == DlOut::AudioWAV || o == DlOut::AudioOriginal; }

static int vcodecRank(const std::string& raw) {
    std::string c = normalizeYtCodec(raw);
    std::string l = lower(raw);
    if (c == "av1") return 6;
    if (c == "vp9") return startsWith(l, "vp09.02") ? 5 : 4;  // VP9 profile 2 carries HDR/10-bit
    if (c == "hevc") return 3;
    if (c == "h264") return 2;
    if (c == "vp8") return 1;
    return 0;
}

static int acodecRank(const std::string& raw) {
    std::string c = normalizeYtCodec(raw);
    if (c == "flac" || c == "alac") return 7;
    if (startsWith(c, "pcm") || c == "wav") return 6;
    if (c == "opus") return 5;
    if (c == "vorbis") return 4;
    if (c == "aac") return 3;
    if (c == "mp3") return 2;
    if (c == "eac3" || c == "ac3") return 1;
    return 0;
}

static int protoRank(const std::string& p) {
    if (p == "https" || p == "http") return 3;
    if (contains(p, "dash")) return 2;
    if (contains(p, "m3u8")) return 1;
    return 0;
}

static bool hdr(const YtFormat& f) { return !f.dynamicRange.empty() && f.dynamicRange != "SDR"; }

static std::wstring W(const std::string& s) { return widen(s); }

static std::wstring describeV(const YtFormat& f) {
    std::wstring r = f.height ? std::to_wstring(f.height) + L"p" : L"?";
    if (f.fps > 0) r += fmtFps(f.fps);
    if (hdr(f)) r += L" " + W(f.dynamicRange);
    r += L" " + W(normalizeYtCodec(f.vcodec));
    double br = f.vbr > 0 ? f.vbr : f.tbr;
    if (br > 0) r += L" " + fmtBitrate(br * 1000);
    return r;
}

static std::wstring describeA(const YtFormat& f) {
    std::wstring r = W(normalizeYtCodec(f.acodec));
    double br = f.abr > 0 ? f.abr : f.tbr;
    if (br > 0) r += L" " + fmtBitrate(br * 1000);
    if (f.channels > 2) r += L" " + std::to_wstring(f.channels) + L"ch";
    if (!f.lang.empty()) r += L" [" + W(f.lang) + L"]";
    return r;
}

std::string Selection::formatSpec() const { return audioId.empty() ? videoId : videoId + "+" + audioId; }

// The channel count a CHANNELS choice changes the audio to; an unknown count counts as a change (the
// conversion after the download sees the real one and copies when nothing changes).
static int remixChoice(int channels, int want) { return want >= 1 && want <= 2 && channels != want ? want : 0; }

Selection selectFormats(const YtInfo& info, const DlPrefs& prefs) {
    Selection s;
    std::vector<const YtFormat*> vids, auds, combos;
    for (auto& f : info.formats) {
        if (f.drm) continue;
        if (f.hasVideo() && f.hasAudio()) combos.push_back(&f);
        else if (f.hasVideo()) vids.push_back(&f);
        else if (f.hasAudio()) auds.push_back(&f);
    }
    bool audioOnly = dlOutIsAudio(prefs.out);
    // Audio: the original-language track, without dynamic range compression, then the most data.
    const YtFormat* bestA = nullptr;
    if (!auds.empty()) {
        int maxPref = -1000;
        bool anyNonDrc = false;
        for (auto f : auds) {
            maxPref = std::max(maxPref, f->langPref);
            anyNonDrc = anyNonDrc || !f->drc();
        }
        auto better = [&](const YtFormat* a, const YtFormat* b) {  // is a better than b
            if (a->channels != b->channels) return a->channels > b->channels;
            double ab = a->abr > 0 ? a->abr : a->tbr, bb = b->abr > 0 ? b->abr : b->tbr;
            if (std::fabs(ab - bb) > std::max(ab, bb) * 0.02) return ab > bb;
            bool aAac = normalizeYtCodec(a->acodec) == "aac", bAac = normalizeYtCodec(b->acodec) == "aac";
            if (prefs.preferH264 && aAac != bAac) return aAac;
            if (acodecRank(a->acodec) != acodecRank(b->acodec)) return acodecRank(a->acodec) > acodecRank(b->acodec);
            return protoRank(a->protocol) > protoRank(b->protocol);
        };
        for (auto f : auds) {
            if (f->langPref < maxPref) continue;
            if (anyNonDrc && f->drc()) continue;
            if (!bestA || better(f, bestA)) bestA = f;
        }
    }
    const YtFormat* bestV = nullptr;
    if (!audioOnly && !vids.empty()) {
        auto better = [&](const YtFormat* a, const YtFormat* b) {
            long long pa = (long long)a->width * a->height, pb = (long long)b->width * b->height;
            if (pa != pb) return pa > pb;
            if (std::fabs(a->fps - b->fps) > 0.5) return a->fps > b->fps;
            if (hdr(*a) != hdr(*b)) return hdr(*a);
            bool a264 = normalizeYtCodec(a->vcodec) == "h264", b264 = normalizeYtCodec(b->vcodec) == "h264";
            if (prefs.preferH264 && a264 != b264) return a264;
            if (vcodecRank(a->vcodec) != vcodecRank(b->vcodec)) return vcodecRank(a->vcodec) > vcodecRank(b->vcodec);
            if (protoRank(a->protocol) != protoRank(b->protocol)) return protoRank(a->protocol) > protoRank(b->protocol);
            double ab = a->vbr > 0 ? a->vbr : a->tbr, bb = b->vbr > 0 ? b->vbr : b->tbr;
            return ab > bb;
        };
        for (auto f : vids)
            if (!bestV || better(f, bestV)) bestV = f;
    }
    // A progressive (combined) format wins only if it beats the separate streams.
    const YtFormat* bestC = nullptr;
    for (auto f : combos) {
        long long pc = (long long)f->width * f->height;
        if (!bestC || pc > (long long)bestC->width * bestC->height || (pc == (long long)bestC->width * bestC->height && f->tbr > bestC->tbr)) bestC = f;
    }
    if (audioOnly) {
        if (bestA) {
            s.a = *bestA;
            s.audioId = "";
            s.videoId = bestA->id;  // formatSpec() returns it alone
        } else if (bestC) {
            s.a = *bestC;
            s.videoId = bestC->id;
            s.notes.push_back(L"No separate audio stream: the audio is taken from a combined file, still bit-for-bit.");
        } else {
            s.error = L"No audio format is available.";
            return s;
        }
        std::string ac = normalizeYtCodec(s.a.acodec);
        s.ext = ac == "aac" ? L".m4a" : ac == "opus" ? L".opus" : ac == "vorbis" ? L".ogg" : ac == "mp3" ? L".mp3" : ac == "flac" ? L".flac" : L".mka";
        s.container = ac == "aac" ? Container::M4A : ac == "opus" ? Container::OPUS : ac == "mp3" ? Container::MP3 : ac == "flac" ? Container::FLAC : Container::MKV;
        s.finalExt = prefs.out == DlOut::AudioMP3 ? L".mp3" : prefs.out == DlOut::AudioWAV ? L".wav" : s.ext;
        s.needsConvert = prefs.out != DlOut::AudioOriginal;
        s.summary = L"audio only: " + describeA(s.a) + L" -> " +
                    (prefs.out == DlOut::AudioOriginal ? s.ext.substr(1) + L" (as it comes, bit-for-bit)"
                     : prefs.out == DlOut::AudioMP3   ? std::wstring(L"MP3 320 kb/s (lossy)")
                                                      : std::wstring(L"WAV (exact decode)"));
        std::wstring layout = s.a.channels > 0 ? L" " + channelsLabel(s.a.channels) : L"";
        // MP3 holds stereo at most; a CHANNELS choice applies to MP3 and WAV (Original stays as it comes).
        int to = prefs.out == DlOut::AudioOriginal ? 0 : remixChoice(s.a.channels, prefs.channels);
        if (prefs.out == DlOut::AudioMP3 && !to && s.a.channels > 2) to = 2;
        s.brief = codecLabel(ac) + layout + L" \u2192 " +
                  (prefs.out == DlOut::AudioOriginal ? std::wstring(containerName(s.container)) + L" copy"
                   : prefs.out == DlOut::AudioMP3    ? std::wstring(L"MP3 320k")
                                                     : std::wstring(L"WAV")) +
                  (to ? L" " + channelsLabel(to) : L"");
        s.estBytes = s.a.bytes(info.duration);
        s.ok = true;
        return s;
    }
    bool useCombo = bestC && (!bestV || (long long)bestC->width * bestC->height > (long long)bestV->width * bestV->height || !bestA);
    if (useCombo) {
        s.v = *bestC;
        s.a = *bestC;
        s.videoId = bestC->id;
    } else if (bestV) {
        s.v = *bestV;
        s.videoId = bestV->id;
        if (bestA) {
            s.a = *bestA;
            s.audioId = bestA->id;
        } else {
            s.notes.push_back(L"This video has no audio track.");
        }
    } else if (bestA) {
        s.error = L"Only audio is available for this item: choose an audio output.";
        return s;
    } else {
        s.error = L"No downloadable format (DRM-protected or unsupported).";
        return s;
    }
    std::string vc = normalizeYtCodec(s.v.vcodec), ac = s.audioId.empty() && !useCombo ? "" : normalizeYtCodec(s.a.acodec);
    // The download is always the best streams, bit-for-bit, in their native container
    // (MKV when video and audio come from different families). MP4 and MOV convert afterwards.
    std::string ve = lower(s.v.ext), ae = lower(s.a.ext);
    if (useCombo || s.audioId.empty()) s.container = ve == "webm" ? Container::WEBM : ve == "mp4" ? Container::MP4 : Container::MKV;
    else if (ve == "mp4" && (ae == "m4a" || ae == "mp4")) s.container = Container::MP4;
    else if (ve == "webm" && ae == "webm") s.container = Container::WEBM;
    else s.container = Container::MKV;
    if (useCombo && ve != "mp4" && ve != "webm") s.ext = L"." + W(ve);
    if (s.ext.empty()) s.ext = containerExt(s.container);
    std::wstring streams = describeV(s.v) + (s.audioId.empty() ? L"" : L" + " + describeA(s.a));
    s.brief = (s.v.height ? resolutionLabel(s.v.width ? s.v.width : s.v.height * 16 / 9, s.v.height, s.v.fps) + L" " : L"") + codecLabel(vc) +
              (hdr(s.v) ? L" HDR" : L"") + (ac.empty() ? L"" : L" + " + codecLabel(ac) + (s.a.channels > 0 ? L" " + channelsLabel(s.a.channels) : L""));
    if (prefs.out == DlOut::MP4 || prefs.out == DlOut::MOV) {
        bool mov = prefs.out == DlOut::MOV;
        s.finalExt = mov ? L".mov" : L".mp4";
        std::wstring codec = prefs.hevc ? L"H.265" : L"H.264";
        bool vCopy = vc == (prefs.hevc ? "hevc" : "h264");
        int to = ac.empty() ? 0 : remixChoice(s.a.channels, prefs.channels);
        bool aCopy = !to && (ac.empty() || ac == "aac" || (mov && (ac == "mp3" || ac == "ac3" || ac == "eac3" || ac == "alac")));
        s.needsConvert = !vCopy || !aCopy || mov || s.container != Container::MP4;
        std::wstring crf = codec + L" CRF " + std::to_wstring(prefs.crf);
        std::wstring aTo = std::wstring(mov ? L"PCM" : L"AAC 320k") + (to ? L" " + channelsLabel(to) : mov ? L" exact" : L"");
        std::wstring what = (vCopy ? codec + L" copied" : crf) + (ac.empty() ? L"" : aCopy ? L" + " + W(ac) + L" copied" : L" + " + aTo);
        s.summary = streams + (mov ? L" -> MOV (" : L" -> MP4 (") + what + L")";
        std::wstring aShort = std::wstring(mov ? L"PCM" : L"AAC") + (to ? L" " + channelsLabel(to) : L"");
        std::wstring target = (vCopy ? std::wstring() : crf) + (ac.empty() || aCopy ? L"" : (vCopy ? L"" : L" + ") + aShort);
        s.brief += (mov ? L" \u2192 MOV " : L" \u2192 MP4 ") + (target.empty() ? std::wstring(L"copy") : target);
        if (!vCopy || !aCopy)
            s.notes.push_back(L"Converted after download: " + std::wstring(vCopy ? L"" : L"video " + W(vc) + L" -> " + crf) + (!vCopy && !aCopy ? L", " : L"") +
                              (aCopy ? L"" : L"audio " + W(ac) + L" -> " + aTo) + L". Original keeps the source bit-for-bit.");
        else
            s.notes.push_back(L"Already " + codec + L": copied bit-for-bit, nothing re-encoded.");
    } else {
        s.finalExt = s.ext;
        s.summary = streams + L" -> " + std::wstring(containerName(s.container)) + L" (as it comes, bit-for-bit)";
        s.brief += L" \u2192 " + std::wstring(containerName(s.container)) + L" copy";
    }
    if (useCombo && bestV && bestA) s.notes.push_back(L"A combined stream has the highest resolution here.");
    s.estBytes = s.v.bytes(info.duration) + (s.audioId.empty() ? 0 : s.a.bytes(info.duration));
    s.ok = true;
    return s;
}

static void authArgs(std::vector<std::wstring>& a, const DlOptions& o) {
    if (!o.cookiesFile.empty()) a.insert(a.end(), {L"--cookies", o.cookiesFile});
    else if (!o.cookiesBrowser.empty()) a.insert(a.end(), {L"--cookies-from-browser", o.cookiesBrowser});
}

std::vector<std::wstring> buildInfoArgs(const std::wstring& url, bool flat, const std::wstring& deno, const DlOptions* auth) {
    std::vector<std::wstring> a = {L"--ignore-config", L"--color", L"never", L"-J", L"--no-warnings"};
    if (flat) a.push_back(L"--flat-playlist");
    else a.push_back(L"--no-playlist");
    if (!deno.empty()) a.insert(a.end(), {L"--js-runtimes", L"deno:" + deno});
    if (auth) authArgs(a, *auth);
    a.insert(a.end(), {L"--", url});
    return a;
}

std::vector<std::wstring> buildDownloadArgs(const DlOptions& o) {
    std::vector<std::wstring> a = {L"--ignore-config", L"--color", L"never", L"--newline", L"--progress", L"--no-playlist"};
    a.insert(a.end(), {L"--progress-template",
                       L"download:EAP|%(progress.status)s|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|"
                       L"%(progress.speed)s|%(progress.eta)s|%(info.format_id)s",
                       L"--progress-template", L"postprocess:EAPP|%(progress.status)s|%(progress.postprocessor)s"});
    a.insert(a.end(), {L"-f", widen(o.sel.formatSpec())});
    if (o.audioExtract) {
        a.push_back(L"-x");  // audio format "best" = stream copy into its native container
    } else if (!o.sel.audioId.empty()) {
        a.insert(a.end(), {L"--merge-output-format", std::wstring(containerExt(o.sel.container)).substr(1)});
    } else {
        std::wstring ext = o.sel.ext.substr(1);
        if (ext == L"mp4" || ext == L"mkv" || ext == L"webm" || ext == L"mov") a.insert(a.end(), {L"--remux-video", ext});
    }
    a.insert(a.end(), {L"-P", o.outDir, L"-o", o.outStem + L".%(ext)s"});
    if (!o.pathFile.empty()) a.insert(a.end(), {L"--print-to-file", L"after_move:%(filepath)s", o.pathFile});
    if (!o.ffmpegDir.empty()) a.insert(a.end(), {L"--ffmpeg-location", o.ffmpegDir});
    if (!o.deno.empty()) a.insert(a.end(), {L"--js-runtimes", L"deno:" + o.deno});
    a.insert(a.end(), {L"-N", std::to_wstring(std::clamp(o.fragments, 1, 32)), L"--retries", L"15", L"--fragment-retries", L"15", L"--retry-sleep",
                       L"fragment:exp=1:20", L"--file-access-retries", L"5", L"--continue", L"--part", L"--no-mtime", L"--no-overwrites"});
    if (o.metadata) a.push_back(L"--embed-metadata");
    if (o.chapters && !o.audioExtract) a.push_back(L"--embed-chapters");
    if (o.thumbnail) a.push_back(L"--embed-thumbnail");
    if (o.subs && !o.audioExtract) a.insert(a.end(), {L"--embed-subs", L"--sub-langs", L"all,-live_chat"});
    if (!o.sectionStart.empty() || !o.sectionEnd.empty()) {
        std::wstring from = o.sectionStart.empty() ? L"0" : o.sectionStart, to = o.sectionEnd.empty() ? L"inf" : o.sectionEnd;
        a.insert(a.end(), {L"--download-sections", L"*" + from + L"-" + to});
    }
    authArgs(a, o);
    if (!o.infoJsonPath.empty()) a.insert(a.end(), {L"--load-info-json", o.infoJsonPath});
    else a.insert(a.end(), {L"--", o.url});
    return a;
}

static double numOrNeg(const std::string& s) {
    if (s.empty() || s == "NA" || s == "None") return -1;
    char* e = nullptr;
    double v = strtod(s.c_str(), &e);
    return (e && *e == 0 && std::isfinite(v)) ? v : -1;
}

bool parseProgressLine(const std::string& line, DlProgress& p) {
    if (startsWith(line, "EAP|")) {
        auto f = split(line, '|');
        if (f.size() < 8) return false;
        p = DlProgress();
        p.isDownload = true;
        p.status = f[1];
        p.done = numOrNeg(f[2]);
        p.total = numOrNeg(f[3]);
        if (p.total <= 0) p.total = numOrNeg(f[4]);
        p.speed = numOrNeg(f[5]);
        p.eta = numOrNeg(f[6]);
        p.formatId = f[7];
        return true;
    }
    if (startsWith(line, "EAPP|")) {
        auto f = split(line, '|');
        if (f.size() < 3) return false;
        p = DlProgress();
        p.isPost = true;
        p.status = f[1];
        p.postprocessor = f[2];
        return true;
    }
    return false;
}

DlError classifyError(const std::string& t, std::wstring& msg, std::wstring& hint) {
    std::string l = lower(t);
    // The last ERROR: line is the most specific one.
    std::string last;
    for (auto& line : split(t, '\n'))
        if (startsWith(trim(line), "ERROR:")) last = trim(line).substr(6);
    msg = widen(trim(last.empty() ? (t.size() > 300 ? t.substr(t.size() - 300) : t) : last));
    auto has = [&](const char* s) { return l.find(s) != std::string::npos; };
    if (has("no space left") || has("errno 28") || has("disk full")) {
        hint = L"The disk is full. Free space or change the output folder, then Retry.";
        return DlError::Disk;
    }
    if (has("unsupported url") || has("is not a valid url")) {
        hint = L"yt-dlp does not recognise this address. Check the link.";
        return DlError::Url;
    }
    if (has("sign in to confirm") || has("login required") || has("private video") || has("members-only") || has("join this channel") ||
        has("age-restricted") || has("confirm your age") || has("use --cookies") || (has("cookies") && has("authentication"))) {
        hint = L"This needs a signed-in account. In Settings pick the browser you are signed in with (cookies), then Retry.";
        return DlError::Auth;
    }
    if (has("not available in your country") || has("geo restrict") || has("geo-restrict")) {
        hint = L"Blocked in your region. A VPN may help; nothing in the app can bypass it.";
        return DlError::Geo;
    }
    if (has("is live") || has("live event will begin") || has("premieres in")) {
        hint = L"Live or upcoming: retry after it ends.";
        return DlError::Live;
    }
    if (has("video unavailable") || has("has been removed") || has("this video is not available") || has("http error 404") || has("does not exist")) {
        hint = L"The video is gone or private.";
        return DlError::Unavailable;
    }
    if (has("requested format is not available")) {
        hint = L"The chosen format vanished between analysis and download. Retry re-analyses it.";
        return DlError::Format;
    }
    if (has("unable to extract") || has("nsig") || has("signature extraction") || has("failed to extract") || has("http error 403") ||
        has("precondition check failed") || has("please report this issue") || has("player response") || has("unable to download api page") ||
        has("challenge") || has("po token")) {
        hint = L"The site changed and yt-dlp could not read it. Encode All updates yt-dlp and retries automatically.";
        return DlError::Extraction;
    }
    if (has("timed out") || has("getaddrinfo") || has("connection reset") || has("network is unreachable") || has("http error 5") || has("unable to download") ||
        has("connection aborted") || has("ssl") || has("remote end closed")) {
        hint = L"Network problem. Retry resumes where it stopped.";
        return DlError::Network;
    }
    hint = L"See the log for details. Retry resumes where it stopped.";
    return DlError::Unknown;
}

}  // namespace ea
