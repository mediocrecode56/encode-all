// Real-media checks through the app's own code paths (Deps, Queue, runners). Needs network on the
// first run (tools install into build\data, portable). Run: build\integration_tests.exe [--no-net]
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/app/deps.h"
#include "../src/app/queue.h"
#include "../src/app/runners.h"
#include "../src/app/settings.h"
#include "../src/core/process.h"
#include "../src/core/util.h"
#include "../src/media/hwenc.h"
#include "../src/media/probe.h"

using namespace ea;

static int g_fail = 0, g_pass = 0;
#define CHECK(c, what)                                       \
    do {                                                     \
        if (c) {                                             \
            g_pass++;                                        \
            printf("  ok    %s\n", what);                    \
        } else {                                             \
            g_fail++;                                        \
            printf("  FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__); \
        }                                                    \
        fflush(stdout);                                      \
    } while (0)

static Deps gDeps;
static std::wstring gWork;

static std::string ff(const std::vector<std::wstring>& args, int* code = nullptr) {
    auto c = runCapture(gDeps.ffmpeg(), args, {}, 600000);
    if (code) *code = c.code;
    if (c.code != 0) printf("    ffmpeg failed: %s\n", c.err.substr(0, 400).c_str());
    return c.out;
}

// Hash of each stream's packets (bit-for-bit identity of copied streams).
static std::string streamHash(const std::wstring& file, const std::wstring& map) {
    return ff({L"-v", L"error", L"-i", file, L"-map", map, L"-c", L"copy", L"-f", L"streamhash", L"-hash", L"md5", L"-"});
}
// streamhash v2 prints header lines (#tb, #software...) that differ by container; keep only the
// per-stream "index, size, hash" lines, which cover every packet byte.
static std::string hashOnly(const std::string& s) {
    std::string r;
    for (auto& l : split(s, '\n')) {
        std::string t = trim(l);
        if (!t.empty() && t[0] != '#') r += t + "\n";
    }
    return r;
}
// Hash of decoded frames (lossless encodes keep these identical).
static std::string decodedVideo(const std::wstring& file) {
    return ff({L"-v", L"error", L"-i", file, L"-map", L"0:v:0", L"-f", L"md5", L"-c:v", L"rawvideo", L"-"});
}
static std::string decodedAudio(const std::wstring& file, const wchar_t* fmt = L"s32") {
    return ff({L"-v", L"error", L"-i", file, L"-map", L"0:a:0", L"-af", std::wstring(L"aformat=sample_fmts=") + fmt, L"-c:a",
               std::wstring(fmt) == L"flt" ? L"pcm_f32le" : L"pcm_s32le", L"-f", L"md5", L"-"});
}

static Settings gSettings;

static bool runTask(Queue& q, TaskKind k, const json::Value& spec, TaskView& out, int timeoutSec = 600) {
    uint64_t id = q.add(k, L"test", spec);
    for (int i = 0; i < timeoutSec * 10; i++) {
        Sleep(100);
        for (auto& t : q.snapshot()) {
            if (t.id != id) continue;
            if (t.state == TaskState::Done || t.state == TaskState::Failed || t.state == TaskState::Canceled) {
                out = t;
                if (t.state != TaskState::Done) {
                    printf("    task failed: %s | %s\n", narrow(t.error).c_str(), narrow(t.hint).c_str());
                    for (auto& l : q.taskLog(id)) printf("      %s\n", narrow(l).substr(0, 300).c_str());
                }
                return t.state == TaskState::Done;
            }
        }
    }
    printf("    task timed out\n");
    return false;
}

static bool logHas(Queue& q, uint64_t id, const std::wstring& what) {
    for (auto& l : q.taskLog(id))
        if (l.find(what) != std::wstring::npos) return true;
    return false;
}

static json::Value convertSpec(const std::wstring& in, const ConvertSpec& cs) {
    json::Value v = json::Value::object();
    v.set("input", in);
    v.set("outDir", joinPath(gWork, L"out"));
    v.set("convert", cs.toJson());
    return v;
}

static json::Value remuxSpec(const std::wstring& in, Container c) {
    RemuxSpec r;
    r.container = c;
    json::Value v = json::Value::object();
    v.set("input", in);
    v.set("outDir", joinPath(gWork, L"out"));
    v.set("remux", r.toJson());
    return v;
}

// ---------- the conversion matrix ----------
// Edge-case sources (odd sizes, rotation tags, 10-bit 4:2:2, interlaced, surround, 192 kHz, several
// audio and subtitle tracks, covers) through every Convert target, audio mode and filter, and every
// Remux mode. Each output must finish through the app's own verification, decode from start to end
// without a single error, and match the requested parameters in an independent probe.

static bool decodesClean(const std::wstring& f, std::string& err) {
    auto c = runCapture(gDeps.ffmpeg(), {L"-v", L"error", L"-i", f, L"-map", L"0:v?", L"-map", L"0:a?", L"-f", L"null", L"-"}, {}, 600000);
    err = trim(c.err);
    return c.code == 0 && err.empty();
}

static const StreamInfo* nth(const MediaInfo& m, const char* type, int n) {
    for (auto& s : m.streams)
        if (s.type == type && !s.attachedPic && n-- == 0) return &s;
    return nullptr;
}

struct Want {
    std::wstring ext;
    std::string vcodec;
    int w = 0, h = 0, depth = 0;
    std::string chroma;
    double fps = 0;
    std::vector<std::string> acodecs;  // every audio track, in order
    int ach = 0, asr = 0;              // first audio track
    int subs = -1, chapters = -1, covers = -1;
    QualityClass cls = QualityClass::Copy;
    bool anyClass = false;
    bool rotationKept = false;
};

static int gMatrixRun = 0, gMatrixOk = 0;

static void matrixCheck(Queue& q, const std::string& name, TaskKind k, const json::Value& spec, const Want& w) {
    gMatrixRun++;
    TaskView t;
    bool done = runTask(q, k, spec, t);
    std::vector<std::string> bad;
    MediaInfo m;
    if (done) {
        if (!probeFile(gDeps.ffprobe(), t.output, m)) bad.push_back("does not probe");
        std::string derr;
        if (!decodesClean(t.output, derr)) bad.push_back("decode errors: " + derr.substr(0, 200));
        if (!w.ext.empty() && lower(extension(t.output)) != w.ext) bad.push_back("extension " + narrow(extension(t.output)));
        if (!w.anyClass && t.cls != w.cls) bad.push_back(std::string("class ") + narrow(className(t.cls)));
        const StreamInfo* v = m.video();
        if (!w.vcodec.empty()) {
            if (!v) bad.push_back("no video");
            else {
                if (v->codec != w.vcodec) bad.push_back("video codec " + v->codec);
                if (w.w && (v->width != w.w || v->height != w.h)) bad.push_back("size " + std::to_string(v->width) + "x" + std::to_string(v->height));
                if (w.depth && pixFmtDepth(v->pixFmt) != w.depth) bad.push_back("pix " + v->pixFmt);
                if (!w.chroma.empty() && pixFmtChroma(v->pixFmt) != w.chroma) bad.push_back("chroma " + v->pixFmt);
                if (w.fps > 0 && std::fabs(v->fps - w.fps) > 0.05) bad.push_back("fps " + std::to_string(v->fps));
                if (v->rotation && !w.rotationKept) bad.push_back("rotation tag left: " + std::to_string(v->rotation));
            }
        } else if (v) {
            bad.push_back("unexpected video " + v->codec);
        }
        int na = m.count("audio");
        if ((int)w.acodecs.size() != na) bad.push_back("audio tracks " + std::to_string(na));
        for (int i = 0; i < na && i < (int)w.acodecs.size(); i++) {
            const StreamInfo* a = nth(m, "audio", i);
            if (a->codec != w.acodecs[i] && !(startsWith(a->codec, "pcm_") && startsWith(w.acodecs[i], "pcm_") &&
                                              a->codec.substr(0, a->codec.size() - 2) == w.acodecs[i].substr(0, w.acodecs[i].size() - 2)))
                bad.push_back("audio " + std::to_string(i) + " codec " + a->codec);
        }
        if (const StreamInfo* a = nth(m, "audio", 0)) {
            if (w.ach && a->channels != w.ach) bad.push_back("channels " + std::to_string(a->channels));
            if (w.asr && a->sampleRate != w.asr) bad.push_back("rate " + std::to_string(a->sampleRate));
        }
        if (w.subs >= 0 && m.count("subtitle") != w.subs) bad.push_back("subtitles " + std::to_string(m.count("subtitle")));
        if (w.chapters >= 0 && m.chapters != w.chapters) bad.push_back("chapters " + std::to_string(m.chapters));
        int covers = 0;
        for (auto& s : m.streams) covers += s.attachedPic;
        if (w.covers >= 0 && covers != w.covers) bad.push_back("covers " + std::to_string(covers));
        // Every stream lasts as long as the source (no truncated track, no drift).
        for (auto& s : m.streams)
            if ((s.isVideo() || s.isAudio()) && s.duration > 0 && m.duration > 0 && std::fabs(s.duration - m.duration) > 0.25)
                bad.push_back(s.type + " lasts " + std::to_string(s.duration) + " of " + std::to_string(m.duration));
    } else {
        bad.push_back("task failed: " + narrow(t.error));
    }
    if (bad.empty()) gMatrixOk++;
    std::string what = "matrix: " + name;
    if (!bad.empty()) {
        what += "  [";
        for (size_t i = 0; i < bad.size(); i++) what += (i ? "; " : "") + bad[i];
        what += "]";
    }
    CHECK(bad.empty(), what.c_str());
}

static void convertCase(Queue& q, const std::string& name, const std::wstring& in, const ConvertSpec& cs, const Want& w) {
    matrixCheck(q, name, TaskKind::Convert, convertSpec(in, cs), w);
}

static void remuxCase(Queue& q, const std::string& name, const std::wstring& in, Container c, RemuxAudio audio, int bitsOrKbps, const Want& w) {
    RemuxSpec r;
    r.container = c;
    r.audio = audio;
    if (audio == RemuxAudio::PCM) r.pcmBits = bitsOrKbps;
    if (audio == RemuxAudio::AAC) r.aacKbps = bitsOrKbps;
    json::Value v = json::Value::object();
    v.set("input", in);
    v.set("outDir", joinPath(gWork, L"out"));
    v.set("remux", r.toJson());
    matrixCheck(q, name, TaskKind::Remux, v, w);
}

static ConvertSpec video(VCodec c, Container k = Container::MP4) {
    ConvertSpec s;
    s.vcodec = c;
    s.container = k;
    s.quality = 26;
    s.speed = "veryfast";
    return s;
}

static Want vw(const wchar_t* ext, const char* vc, int w, int h, int depth, const char* chroma, std::vector<std::string> ac, QualityClass cls = QualityClass::Lossy) {
    Want x;
    x.ext = ext;
    x.vcodec = vc;
    x.w = w;
    x.h = h;
    x.depth = depth;
    x.chroma = chroma;
    x.acodecs = std::move(ac);
    x.cls = cls;
    return x;
}

static void runMatrix(Queue& q) {
    printf("[matrix: fixtures]\n");
    std::wstring dir = joinPath(gWork, L"mx");
    makeDirs(dir);
    auto F = [&](const wchar_t* n) { return joinPath(dir, n); };
    std::wstring odd = F(L"odd 321x241.mkv"), rotTmp = F(L"rot-tmp.mp4"), rot = F(L"rotated.mp4"), p422 = F(L"10bit 422 pcm96k 5.1.mov"),
                 inter = F(L"interlaced.mkv"), multi = F(L"hevc10 multi.mkv"), vp9 = F(L"vp9.webm"), hiFlac = F(L"192k 5.1.flac"), mono = F(L"mono 22k.mp3"),
                 alac = F(L"alac cover.m4a"), cover = F(L"cover.png"), ass = F(L"s.ass"), srt2 = F(L"s2.srt"), chap = F(L"chap2.txt");
    writeFileAtomic(ass, "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                         "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
                         "MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,28,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n\n"
                         "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
                         "Dialogue: 0,0:00:00.50,0:00:01.50,Default,,0,0,0,,{\\b1}styled{\\b0} line\n");
    writeFileAtomic(srt2, "1\n00:00:00,200 --> 00:00:01,800\nsecond track\n");
    writeFileAtomic(chap, ";FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1500\ntitle=A\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=1500\nEND=3000\ntitle=B\n");
    const wchar_t* six = L"aevalsrc=exprs=0.3*sin(440*2*PI*t)|0.3*sin(550*2*PI*t)|0.3*sin(660*2*PI*t)|0.2*sin(60*2*PI*t)|0.3*sin(770*2*PI*t)|0.3*sin(880*2*PI*t)";
    int code = 0;
    auto mk = [&](const char* what, std::vector<std::wstring> args) {
        std::vector<std::wstring> a = {L"-v", L"error", L"-y"};
        a.insert(a.end(), args.begin(), args.end());
        ff(a, &code);
        CHECK(code == 0, what);
    };
    mk("odd 321x241 FFV1 4:2:0 + MP3 stereo 44.1 kHz in MKV",
       {L"-f", L"lavfi", L"-i", L"testsrc=s=321x241:r=25:d=3", L"-f", L"lavfi", L"-i", L"sine=f=440:d=3:sample_rate=44100", L"-ac", L"2", L"-c:v", L"ffv1",
        L"-pix_fmt", L"yuv420p", L"-c:a", L"libmp3lame", L"-b:a", L"192k", L"-shortest", odd});
    mk("640x360 H.264 + AAC with a 90 degree rotation tag",
       {L"-f", L"lavfi", L"-i", L"testsrc2=s=640x360:r=30:d=3", L"-f", L"lavfi", L"-i", L"sine=f=440:d=3:sample_rate=48000", L"-c:v", L"libx264", L"-preset",
        L"veryfast", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac", L"-shortest", rotTmp});
    mk("rotation tag applied", {L"-display_rotation", L"90", L"-i", rotTmp, L"-c", L"copy", rot});
    mk("10-bit 4:2:2 H.264 + PCM 24-bit 96 kHz 5.1(side) in MOV",
       {L"-f", L"lavfi", L"-i", L"testsrc2=s=640x360:r=24000/1001:d=3", L"-f", L"lavfi", L"-i", std::wstring(six) + L":s=96000:c=5.1(side):d=3", L"-c:v", L"libx264",
        L"-preset", L"veryfast", L"-pix_fmt", L"yuv422p10le", L"-c:a", L"pcm_s24le", L"-shortest", p422});
    mk("interlaced MPEG-2 720x576 TFF + AC-3 5.1 in MKV",
       {L"-f", L"lavfi", L"-i", L"testsrc2=s=720x576:r=25:d=3", L"-f", L"lavfi", L"-i", std::wstring(six) + L":s=48000:c=5.1(side):d=3", L"-c:v", L"mpeg2video",
        L"-vf", L"setfield=tff", L"-flags", L"+ilme+ildct", L"-field_order", L"tt", L"-b:v", L"4M", L"-c:a", L"ac3", L"-b:a", L"384k", L"-shortest", inter});
    mk("10-bit HEVC + Opus 5.1 + FLAC 192 kHz + ASS + SRT + chapters in MKV",
       {L"-f", L"lavfi", L"-i", L"testsrc2=s=640x360:r=30:d=3", L"-f", L"lavfi", L"-i", std::wstring(six) + L":s=48000:c=5.1:d=3", L"-f", L"lavfi", L"-i",
        L"sine=f=300:d=3:sample_rate=192000", L"-i", ass, L"-i", srt2, L"-f", L"ffmetadata", L"-i", chap, L"-map", L"0", L"-map", L"1", L"-map", L"2", L"-map", L"3",
        L"-map", L"4", L"-map_chapters", L"5", L"-c:v", L"libx265", L"-preset", L"ultrafast", L"-pix_fmt", L"yuv420p10le", L"-x265-params", L"log-level=error",
        L"-c:a:0", L"libopus", L"-b:a:0", L"256k", L"-c:a:1", L"flac", L"-ac:a:1", L"2", L"-sample_fmt:a:1", L"s32", L"-bits_per_raw_sample:a:1", L"24",
        L"-c:s:0", L"ass", L"-c:s:1", L"srt", L"-shortest", multi});
    mk("VP9 + Vorbis in WebM",
       {L"-f", L"lavfi", L"-i", L"testsrc2=s=426x240:r=30:d=3", L"-f", L"lavfi", L"-i", L"sine=f=500:d=3:sample_rate=48000", L"-ac", L"2", L"-c:v", L"libvpx-vp9",
        L"-deadline", L"realtime", L"-cpu-used", L"8", L"-b:v", L"400k", L"-c:a", L"libvorbis", L"-shortest", vp9});
    mk("FLAC 24-bit 192 kHz 5.1",
       {L"-f", L"lavfi", L"-i", std::wstring(six) + L":s=192000:c=5.1(side):d=3", L"-c:a", L"flac", L"-sample_fmt", L"s32", L"-bits_per_raw_sample", L"24", hiFlac});
    mk("MP3 mono 22.05 kHz", {L"-f", L"lavfi", L"-i", L"sine=f=700:d=3:sample_rate=22050", L"-c:a", L"libmp3lame", L"-b:a", L"64k", mono});
    mk("cover PNG", {L"-f", L"lavfi", L"-i", L"testsrc2=s=300x300:d=1", L"-frames:v", L"1", cover});
    mk("ALAC stereo with a PNG cover in M4A",
       {L"-f", L"lavfi", L"-i", L"sine=f=600:d=3:sample_rate=44100", L"-i", cover, L"-map", L"0", L"-map", L"1", L"-ac", L"2", L"-c:a", L"alac", L"-c:v", L"copy",
        L"-disposition:v", L"attached_pic", alac});
    MediaInfo pm;
    probeFile(gDeps.ffprobe(), odd, pm);
    CHECK(pm.video() && pm.video()->width == 321 && pm.video()->height == 241, "odd fixture really is 321x241");
    probeFile(gDeps.ffprobe(), inter, pm);
    CHECK(pm.video() && (pm.video()->fieldOrder == "tt" || pm.video()->fieldOrder == "tb"), "interlaced fixture probes as top field first");
    probeFile(gDeps.ffprobe(), rot, pm);
    CHECK(pm.video() && std::abs(pm.video()->rotation) == 90, "rotated fixture carries the tag");

    printf("[matrix: convert]\n");
    using QC = QualityClass;
    // Odd sizes: 4:2:0 encoders crop to even; 4:2:2 crops the width only; MP3 audio fits MP4.
    convertCase(q, "odd FFV1 -> H.264 MP4 (cropped to 320x240, MP3 copied)", odd, video(VCodec::H264), vw(L".mp4", "h264", 320, 240, 8, "420", {"mp3"}));
    {
        ConvertSpec s = video(VCodec::H265);
        s.rate = RateMode::Lossless;
        convertCase(q, "odd FFV1 -> H.265 lossless (crop makes it LOSSY)", odd, s, vw(L".mp4", "hevc", 320, 240, 8, "420", {"mp3"}));
    }
    convertCase(q, "odd FFV1 -> DNxHR HQ MOV (DNxHR needs even sides: cropped)", odd, video(VCodec::DNxHR_HQ, Container::MOV), vw(L".mov", "dnxhd", 320, 240, 8, "422", {"mp3"}));
    {
        ConvertSpec s = video(VCodec::H264);
        s.pixFmt = "yuv422p";
        convertCase(q, "odd FFV1 -> H.264 8-bit 4:2:2 (cropped to even sides)", odd, s, vw(L".mp4", "h264", 320, 240, 8, "422", {"mp3"}));
        s.pixFmt = "yuv444p";
        convertCase(q, "odd FFV1 -> H.264 4:4:4 (odd size kept)", odd, s, vw(L".mp4", "h264", 321, 241, 8, "444", {"mp3"}));
    }
    {
        ConvertSpec s = video(VCodec::H264, Container::MOV);
        s.acodec = ACodec::PCM;
        s.pcmBits = 16;
        Want w = vw(L".mov", "h264", 320, 240, 8, "420", {"pcm_s16le"});
        w.asr = 44100;
        convertCase(q, "odd FFV1 -> H.264 MOV + PCM 16-bit", odd, s, w);
        s.vcodec = VCodec::Copy;
        s.container = Container::MP4;
        Plan p;
        MediaInfo om;
        probeFile(gDeps.ffprobe(), odd, om);
        p = planConvert(om, s, EncoderCaps());
        CHECK(!p.ok && p.error.find(L"cannot be copied") != std::wstring::npos, "matrix: FFV1 copy into MP4 is refused before it starts");
    }
    // Rotation: the tag is applied; sizes follow the turned picture.
    {
        ConvertSpec s = video(VCodec::H264);
        s.height = 720;
        convertCase(q, "rotated -> H.264 720p (upright 406x720)", rot, s, vw(L".mp4", "h264", 406, 720, 8, "420", {"aac"}));
        s = video(VCodec::H265);
        s.rotate = 1;
        convertCase(q, "rotated -> H.265 + rotate 90 cw (back to 640x360)", rot, s, vw(L".mp4", "hevc", 640, 360, 8, "420", {"aac"}));
    }
    // 10-bit 4:2:2 with 96 kHz 5.1 PCM.
    {
        Want w = vw(L".mp4", "hevc", 640, 360, 10, "422", {"pcm_s24le"});
        w.ach = 6;
        w.asr = 96000;
        convertCase(q, "10-bit 4:2:2 -> H.265 MP4 (4:2:2 10-bit kept, PCM copied)", p422, video(VCodec::H265), w);
        ConvertSpec s = video(VCodec::H264);
        s.acodec = ACodec::AAC;
        s.aacKbps = 256;
        w = vw(L".mp4", "h264", 640, 360, 10, "422", {"aac"});
        w.ach = 6;
        w.asr = 96000;
        convertCase(q, "10-bit 4:2:2 -> H.264 MP4 + AAC 256 (5.1 at 96 kHz)", p422, s, w);
        convertCase(q, "10-bit 4:2:2 -> DNxHR HQX MOV", p422, video(VCodec::DNxHR_HQX, Container::MOV), vw(L".mov", "dnxhd", 640, 360, 10, "422", {"pcm_s24le"}));
        convertCase(q, "10-bit 4:2:2 -> DNxHR 444 MOV", p422, video(VCodec::DNxHR_444, Container::MOV), vw(L".mov", "dnxhd", 640, 360, 10, "444", {"pcm_s24le"}));
        convertCase(q, "10-bit 4:2:2 -> DNxHR LB MOV (8-bit 4:2:2)", p422, video(VCodec::DNxHR_LB, Container::MOV), vw(L".mov", "dnxhd", 640, 360, 8, "422", {"pcm_s24le"}));
        s = video(VCodec::H264, Container::MOV);
        s.rate = RateMode::Lossless;
        TaskView t;
        CHECK(runTask(q, TaskKind::Convert, convertSpec(p422, s), t) && t.cls == QC::Lossless && decodedVideo(t.output) == decodedVideo(p422) &&
                  decodedAudio(t.output) == decodedAudio(p422),
              "matrix: 10-bit 4:2:2 -> H.264 lossless MOV: frames and samples identical");
        s = video(VCodec::H265);
        s.pixFmt = "yuv420p";
        s.width = 1280;
        s.height = 720;
        s.fps = 30;
        w = vw(L".mp4", "hevc", 1280, 720, 8, "420", {"pcm_s24le"});
        w.fps = 30;
        convertCase(q, "10-bit 4:2:2 -> H.265 8-bit 4:2:0 720p 30 fps", p422, s, w);
        s = video(VCodec::H264);
        s.channels = 2;
        s.loudness = 1;
        w = vw(L".mp4", "h264", 640, 360, 10, "422", {"aac"});
        w.ach = 2;
        w.asr = 96000;
        convertCase(q, "10-bit 4:2:2 -> H.264 + stereo downmix + loudness -23 (AAC)", p422, s, w);
    }
    // Interlaced MPEG-2 with AC-3 5.1.
    {
        ConvertSpec s = video(VCodec::H264);
        s.deinterlace = 1;
        Want w = vw(L".mp4", "h264", 720, 576, 8, "420", {"ac3"});
        w.fps = 25;
        w.ach = 6;
        convertCase(q, "interlaced -> H.264 + deinterlace auto (AC-3 copied)", inter, s, w);
        s.deinterlace = 3;
        s.vcodec = VCodec::H265;
        w.vcodec = "hevc";
        w.fps = 50;
        convertCase(q, "interlaced -> H.265 + deinterlace double rate (50 fps)", inter, s, w);
        s = video(VCodec::H264);
        s.denoise = 2;
        s.sharpen = 1;
        s.rotate = 3;
        s.channels = 2;
        w = vw(L".mp4", "h264", 720, 576, 8, "420", {"aac"});
        w.ach = 2;
        w.asr = 48000;
        convertCase(q, "interlaced -> H.264 + denoise + sharpen + 180 + stereo", inter, s, w);
    }
    // Several tracks: Opus 5.1, FLAC 192 kHz, ASS, SRT, chapters.
    {
        Want w = vw(L".mp4", "h264", 640, 360, 10, "420", {"opus", "flac"});
        w.subs = 2;
        w.chapters = 2;
        convertCase(q, "multi -> H.264 MP4 (Opus and FLAC copied, ASS and SRT as mov_text, chapters)", multi, video(VCodec::H264), w);
        ConvertSpec s = video(VCodec::H265, Container::MOV);
        w = vw(L".mov", "hevc", 640, 360, 10, "420", {"pcm_f32le", "pcm_s24le"});
        w.subs = 2;
        w.ach = 6;
        convertCase(q, "multi -> H.265 MOV (Opus and FLAC to exact PCM)", multi, s, w);
        s = video(VCodec::Copy);
        s.acodec = ACodec::AAC;
        s.aacKbps = 192;
        s.keepSubs = false;
        w = vw(L".mp4", "hevc", 640, 360, 10, "420", {"aac", "aac"});
        w.ach = 6;
        w.subs = 0;
        convertCase(q, "multi -> copy video MP4 + AAC 192 (5.1 and 192 kHz stereo), no subtitles", multi, s, w);
    }
    // VP9 + Vorbis.
    {
        Want w = vw(L".mp4", "h264", 426, 240, 8, "420", {"pcm_f32le"});
        convertCase(q, "VP9 -> H.264 MP4 (Vorbis cannot be copied: exact PCM)", vp9, video(VCodec::H264), w);
        ConvertSpec s = video(VCodec::Copy);
        s.acodec = ACodec::AAC;
        s.aacKbps = 128;
        w = vw(L".mp4", "vp9", 426, 240, 8, "420", {"aac"});
        w.ach = 2;
        convertCase(q, "VP9 -> copy video MP4 + AAC 128", vp9, s, w);
        s = video(VCodec::H265);
        s.rate = RateMode::Bitrate;
        s.mbps = 1;
        w = vw(L".mp4", "hevc", 426, 240, 8, "420", {"pcm_f32le"});
        convertCase(q, "VP9 -> H.265 1 Mb/s", vp9, s, w);
    }
    // Audio targets.
    {
        auto aw = [](const wchar_t* ext, const char* c, int ch, int sr, QualityClass cls, int covers = 0) {
            Want w;
            w.ext = ext;
            w.acodecs = {c};
            w.ach = ch;
            w.asr = sr;
            w.cls = cls;
            w.covers = covers;
            return w;
        };
        ConvertSpec a;
        a.audio = AudioTarget::MP3;
        convertCase(q, "192 kHz 5.1 FLAC -> MP3 320 (stereo, 48 kHz)", hiFlac, a, aw(L".mp3", "mp3", 2, 48000, QC::Lossy));
        a.mp3Vbr = true;
        a.mp3Quality = 2;
        convertCase(q, "192 kHz 5.1 FLAC -> MP3 VBR V2", hiFlac, a, aw(L".mp3", "mp3", 2, 48000, QC::Lossy));
        a = ConvertSpec();
        a.audio = AudioTarget::WAV;
        TaskView t;
        CHECK(runTask(q, TaskKind::Convert, convertSpec(hiFlac, a), t) && t.cls == QC::Lossless && decodedAudio(t.output) == decodedAudio(hiFlac),
              "matrix: 192 kHz 5.1 FLAC -> WAV: samples identical");
        a.audio = AudioTarget::FLAC;
        a.flacLevel = 5;
        convertCase(q, "192 kHz 5.1 FLAC -> FLAC 5 (exact)", hiFlac, a, aw(L".flac", "flac", 6, 192000, QC::Lossless));
        a.audio = AudioTarget::WAV;
        a.loudness = 3;
        convertCase(q, "192 kHz 5.1 FLAC -> WAV + loudness -14 (keeps 192 kHz)", hiFlac, a, aw(L".wav", "pcm_s24le", 6, 192000, QC::Lossy));
        a = ConvertSpec();
        a.audio = AudioTarget::MP3;
        convertCase(q, "mono MP3 -> MP3 (copied)", mono, a, aw(L".mp3", "mp3", 1, 22050, QC::Copy));
        a.audio = AudioTarget::FLAC;
        a.sampleRate = 48000;
        convertCase(q, "mono MP3 -> FLAC 48 kHz (resampled)", mono, a, aw(L".flac", "flac", 1, 48000, QC::Lossy));
        a = ConvertSpec();
        a.audio = AudioTarget::MP3;
        convertCase(q, "ALAC with cover -> MP3 (cover kept)", alac, a, aw(L".mp3", "mp3", 2, 44100, QC::Lossy, 1));
        a.audio = AudioTarget::FLAC;
        convertCase(q, "ALAC with cover -> FLAC (exact, cover kept)", alac, a, aw(L".flac", "flac", 2, 44100, QC::Lossless, 1));
        a.audio = AudioTarget::WAV;
        convertCase(q, "ALAC with cover -> WAV (cover dropped)", alac, a, aw(L".wav", "pcm_s16le", 2, 44100, QC::Lossless, 0));
        a = ConvertSpec();
        a.audio = AudioTarget::WAV;
        convertCase(q, "multi -> WAV (default track: Opus 5.1 as float)", multi, a, aw(L".wav", "pcm_f32le", 6, 48000, QC::Lossless));
    }

    printf("[matrix: fastest encoder]\n");
    {
        // Auto takes the graphics encoder when there is one: sizes and codecs must hold there too.
        gSettings.encoder = Accel::Auto;
        auto any = [](const char* vc, int w, int h, std::vector<std::string> ac) {
            Want x = vw(L".mp4", vc, w, h, 0, "", std::move(ac));
            x.anyClass = true;
            return x;
        };
        convertCase(q, "fastest: 10-bit 4:2:2 -> H.265", p422, video(VCodec::H265), any("hevc", 640, 360, {"pcm_s24le"}));
        convertCase(q, "fastest: odd FFV1 -> H.264 (cropped)", odd, video(VCodec::H264), any("h264", 320, 240, {"mp3"}));
        ConvertSpec s = video(VCodec::H265);
        s.height = 720;
        convertCase(q, "fastest: rotated -> H.265 720p", rot, s, any("hevc", 406, 720, {"aac"}));
        s = video(VCodec::H264);
        s.deinterlace = 1;
        s.rate = RateMode::Bitrate;
        s.mbps = 3;
        convertCase(q, "fastest: interlaced -> H.264 3 Mb/s, deinterlaced", inter, s, any("h264", 720, 576, {"ac3"}));
        gSettings.encoder = Accel::Software;
    }

    printf("[matrix: remux]\n");
    {
        Want w = vw(L".mp4", "hevc", 640, 360, 10, "420", {"opus", "flac"}, QC::Copy);
        w.subs = 2;
        w.chapters = 2;
        w.anyClass = true;  // the subtitles become mov_text (text conversion), the streams are copied
        remuxCase(q, "multi -> MP4 original", multi, Container::MP4, RemuxAudio::Original, 0, w);
        w = vw(L".mov", "hevc", 640, 360, 10, "420", {"pcm_f32le", "pcm_s24le"}, QC::Lossless);
        w.subs = 2;
        w.chapters = 2;
        remuxCase(q, "multi -> MOV original (Opus, FLAC to exact PCM)", multi, Container::MOV, RemuxAudio::Original, 0, w);
        w = vw(L".mp4", "hevc", 640, 360, 10, "420", {"aac", "aac"});
        w.ach = 6;
        w.subs = 2;
        remuxCase(q, "multi -> MP4 AAC 192 (Opus 5.1 and FLAC 192 kHz)", multi, Container::MP4, RemuxAudio::AAC, 192, w);
        w = vw(L".mp4", "hevc", 640, 360, 10, "420", {"pcm_s16le", "pcm_s16le"});
        w.subs = 2;
        remuxCase(q, "multi -> MP4 PCM 16-bit", multi, Container::MP4, RemuxAudio::PCM, 16, w);
        w = vw(L".mp4", "h264", 640, 360, 10, "422", {"pcm_s24le"}, QC::Copy);
        w.ach = 6;
        w.asr = 96000;
        remuxCase(q, "10-bit 4:2:2 MOV -> MP4 original (PCM copied)", p422, Container::MP4, RemuxAudio::Original, 0, w);
        w = vw(L".mp4", "h264", 640, 360, 10, "422", {"aac"});
        w.ach = 6;
        w.asr = 96000;
        remuxCase(q, "10-bit 4:2:2 MOV -> MP4 AAC 320 (5.1)", p422, Container::MP4, RemuxAudio::AAC, 320, w);
        w = vw(L".mp4", "mpeg2video", 720, 576, 8, "420", {"ac3"}, QC::Copy);
        w.ach = 6;
        remuxCase(q, "interlaced MKV -> MP4 original", inter, Container::MP4, RemuxAudio::Original, 0, w);
        w = vw(L".mov", "mpeg2video", 720, 576, 8, "420", {"aac"});
        remuxCase(q, "interlaced MKV -> MOV AAC 256 (AC-3 to AAC)", inter, Container::MOV, RemuxAudio::AAC, 256, w);
        w = vw(L".mp4", "h264", 0, 0, 8, "420", {"aac"}, QC::Copy);
        w.rotationKept = true;  // copied: the stored 640x360 keeps its rotation tag
        remuxCase(q, "rotated -> MP4 AAC (AAC already: copied, rotation tag kept)", rot, Container::MP4, RemuxAudio::AAC, 256, w);
        Want a;
        a.ext = L".mp4";
        a.acodecs = {"aac"};
        a.covers = 1;
        a.cls = QC::Lossy;
        remuxCase(q, "ALAC with cover -> MP4 AAC (cover kept)", alac, Container::MP4, RemuxAudio::AAC, 256, a);
    }
    printf("  matrix: %d of %d outputs correct\n", gMatrixOk, gMatrixRun);
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool net = true;
    for (int i = 1; i < argc; i++)
        if (std::wstring(argv[i]) == L"--no-net") net = false;
    // Own data folder: the app beside this exe must never see test tasks, history or log lines.
    setDataDir(joinPath(dirName(exePath()), L"test-data"));
    gWork = joinPath(dataDir(), L"itest");
    removeTree(gWork);
    makeDirs(joinPath(gWork, L"out"));
    printf("data: %s\n", narrow(dataDir()).c_str());

    printf("[tools]\n");
    gDeps.init(dataDir(), nullptr);
    gDeps.startMaintenance(false, false);
    std::atomic<bool> never{false};
    ULONGLONG t0 = GetTickCount64();
    for (Dep d : {Dep::YtDlp, Dep::Deno, Dep::FFmpeg}) {
        while (!gDeps.ready(d) && GetTickCount64() - t0 < 900000) {
            DepView v = gDeps.view(d);
            if (!v.error.empty() && !v.busy) break;
            Sleep(500);
        }
        DepView v = gDeps.view(d);
        std::string what = narrow(depName(d)) + " installed and self-tested (" + narrow(v.version) + ")" + (v.error.empty() ? "" : " err=" + narrow(v.error));
        CHECK(v.present, what.c_str());
    }
    if (!gDeps.ready(Dep::FFmpeg)) {
        printf("FFmpeg missing: cannot continue\n");
        return 1;
    }
    printf("  install time %.1f s\n", (GetTickCount64() - t0) / 1000.0);
    EncoderCaps caps = detectEncoders(gDeps.ffmpeg());
    printf("  encoders: nvenc=%d/%d qsv=%d/%d amf=%d/%d\n", caps.has("h264_nvenc"), caps.has("hevc_nvenc"), caps.has("h264_qsv"), caps.has("hevc_qsv"),
           caps.has("h264_amf"), caps.has("hevc_amf"));
    CHECK(caps.probed && caps.has("libx264") && caps.has("libx265"), "software encoders present");

    printf("[fixtures]\n");
    std::wstring a = joinPath(gWork, L"src a.mp4"), b = joinPath(gWork, L"src b ünï.mkv"), fl = joinPath(gWork, L"src c.flac");
    std::wstring srt = joinPath(gWork, L"s.srt"), chap = joinPath(gWork, L"chap.txt"), font = joinPath(gWork, L"font.ttf");
    writeFileAtomic(srt, "1\n00:00:00,500 --> 00:00:02,000\nhello\n\n2\n00:00:02,500 --> 00:00:04,000\nworld\n");
    writeFileAtomic(chap, ";FFMETADATA1\ntitle=Fixture\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=2000\ntitle=One\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=2000\nEND=5000\ntitle=Two\n");
    writeFileAtomic(font, std::string(256, 'F'));
    int code = 0;
    ff({L"-v", L"error", L"-y", L"-f", L"lavfi", L"-i", L"testsrc2=s=640x360:r=30000/1001:d=5", L"-f", L"lavfi", L"-i", L"sine=f=440:d=5:sample_rate=48000",
        L"-c:v", L"libx264", L"-preset", L"veryfast", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac", L"-b:a", L"160k", L"-shortest", a}, &code);
    CHECK(code == 0, "fixture A: H.264 + AAC in MP4");
    ff({L"-v", L"error", L"-y", L"-f", L"lavfi", L"-i", L"testsrc2=s=640x360:r=24:d=5", L"-f", L"lavfi", L"-i", L"sine=f=330:d=5:sample_rate=48000", L"-f", L"lavfi",
        L"-i", L"sine=f=550:d=5:sample_rate=44100", L"-i", srt, L"-f", L"ffmetadata", L"-i", chap, L"-map", L"0", L"-map", L"1", L"-map", L"2", L"-map", L"3",
        L"-map_chapters", L"4", L"-c:v", L"libx264", L"-preset", L"veryfast", L"-pix_fmt", L"yuv420p10le", L"-c:a:0", L"flac", L"-sample_fmt:a:0", L"s32",
        L"-bits_per_raw_sample:a:0", L"24", L"-c:a:1", L"libopus", L"-b:a:1", L"96k", L"-c:s", L"srt", L"-metadata:s:a:0", L"language=eng",
        L"-metadata:s:a:1", L"language=spa", L"-attach", font, L"-metadata:s:t", L"mimetype=font/ttf", L"-shortest", b}, &code);
    CHECK(code == 0, "fixture B: 10-bit H.264 + FLAC 24 + Opus + SRT + chapters + attachment in MKV (Unicode name)");
    ff({L"-v", L"error", L"-y", L"-f", L"lavfi", L"-i", L"sine=f=1000:d=4:sample_rate=96000", L"-ac", L"2", L"-c:a", L"flac", L"-sample_fmt", L"s32",
        L"-bits_per_raw_sample", L"24", L"-metadata", L"title=Tone", fl}, &code);
    CHECK(code == 0, "fixture C: FLAC 24-bit 96 kHz");

    Queue q;
    gSettings.maxEncodes = 2;
    gSettings.maxRemux = 2;
    gSettings.encoder = Accel::Software;  // deterministic encodes; the hardware loop and live downloads switch it
    gSettings.dlDir = joinPath(gWork, L"dl");
    q.setRunner(TaskKind::Download, runDownload);
    q.setRunner(TaskKind::Convert, runConvert);
    q.setRunner(TaskKind::Remux, runRemux);
    q.init(&gDeps, [] { return gSettings; }, [&] { return caps; }, nullptr);
    TaskView t;
    MediaInfo m;

    printf("[remux]\n");
    CHECK(runTask(q, TaskKind::Remux, remuxSpec(a, Container::MKV), t), "A -> MKV remux finished");
    CHECK(hashOnly(streamHash(t.output, L"0:v")) == hashOnly(streamHash(a, L"0:v")) && hashOnly(streamHash(t.output, L"0:a")) == hashOnly(streamHash(a, L"0:a")),
          "A -> MKV: video and audio packets bit-identical");
    CHECK(t.cls == QualityClass::Copy, "A -> MKV classed as stream copy");
    CHECK(runTask(q, TaskKind::Remux, remuxSpec(b, Container::MP4), t), "B -> MP4 remux finished");
    probeFile(gDeps.ffprobe(), t.output, m);
    CHECK(m.count("audio") == 2 && m.count("subtitle") == 1 && m.chapters == 2, "B -> MP4: 2 audio, subtitle as mov_text, chapters kept");
    CHECK(m.count("attachment") == 0, "B -> MP4: attachment left out (listed in the plan)");
    CHECK(hashOnly(streamHash(t.output, L"0:a:0")) == hashOnly(streamHash(b, L"0:a:0")), "B -> MP4: FLAC copied bit-for-bit");
    CHECK(hashOnly(streamHash(t.output, L"0:v")) == hashOnly(streamHash(b, L"0:v")), "B -> MP4: 10-bit video copied bit-for-bit");
    CHECK(runTask(q, TaskKind::Remux, remuxSpec(b, Container::MOV), t), "B -> MOV remux finished");
    probeFile(gDeps.ffprobe(), t.output, m);
    CHECK(m.streams.size() >= 3 && m.streams[1].codec == "pcm_s24le", "B -> MOV: FLAC became PCM 24-bit (MOV cannot hold FLAC)");
    CHECK(decodedAudio(t.output) == decodedAudio(b), "B -> MOV: PCM decode identical to the FLAC (lossless)");
    CHECK(m.streams.size() >= 3 && m.streams[2].codec == "pcm_f32le", "B -> MOV: Opus decoded to 32-bit float (exact)");
    CHECK(runTask(q, TaskKind::Remux, remuxSpec(b, Container::MKV), t), "B -> MKV remux finished");
    probeFile(gDeps.ffprobe(), t.output, m);
    CHECK(m.count("attachment") == 1 && m.count("subtitle") == 1 && m.chapters == 2 && t.cls == QualityClass::Copy, "B -> MKV: everything kept, pure copy");
    CHECK(fileExists(joinPath(joinPath(gWork, L"out"), L"src b ünï.mkv")), "Unicode output name written");
    CHECK(runTask(q, TaskKind::Remux, remuxSpec(a, Container::MKV), t) && t.output.find(L"(1)") != std::wstring::npos, "second remux of A gets a new name (never overwrites)");

    printf("[convert]\n");
    ConvertSpec cs;
    applyPreset(cs, Preset::Lossless);
    CHECK(runTask(q, TaskKind::Convert, convertSpec(a, cs), t), "A -> H.264 lossless finished");
    CHECK(decodedVideo(t.output) == decodedVideo(a), "A -> H.264 lossless: decoded frames identical");
    CHECK(hashOnly(streamHash(t.output, L"0:a")) == hashOnly(streamHash(a, L"0:a")), "A -> lossless: AAC copied bit-for-bit");
    CHECK(t.cls == QualityClass::Lossless, "classed as lossless encode");
    applyPreset(cs, Preset::Lossless);
    CHECK(runTask(q, TaskKind::Convert, convertSpec(b, cs), t), "B (10-bit) -> H.264 lossless finished");
    CHECK(decodedVideo(t.output) == decodedVideo(b), "B -> lossless: 10-bit frames identical");
    cs = ConvertSpec();
    applyPreset(cs, Preset::Balanced);
    CHECK(runTask(q, TaskKind::Convert, convertSpec(a, cs), t), "A -> H.265 balanced (x265) finished");
    probeFile(gDeps.ffprobe(), t.output, m);
    CHECK(m.video() && m.video()->codec == "hevc" && t.cls == QualityClass::Lossy, "H.265 output, classed lossy");
    cs = ConvertSpec();
    setlocale(LC_ALL, "C");
    cs.vcodec = VCodec::DNxHR_HQX;
    cs.container = Container::MOV;
    CHECK(runTask(q, TaskKind::Convert, convertSpec(b, cs), t), "B -> DNxHR HQX finished");
    probeFile(gDeps.ffprobe(), t.output, m);
    CHECK(m.video() && m.video()->codec == "dnxhd" && m.video()->pixFmt == "yuv422p10le" && extension(t.output) == L".mov", "DNxHR HQX: 10-bit 4:2:2 in MOV");
    for (const char* hw : {"hevc_nvenc", "hevc_qsv", "hevc_amf"}) {
        if (!caps.has(hw)) continue;
        cs = ConvertSpec();
        cs.vcodec = VCodec::H265;
        gSettings.encoder = std::string(hw) == "hevc_nvenc" ? Accel::NVENC : std::string(hw) == "hevc_qsv" ? Accel::QSV : Accel::AMF;
        std::string what = std::string("A -> H.265 with ") + hw;
        CHECK(runTask(q, TaskKind::Convert, convertSpec(a, cs), t) && logHas(q, t.id, L"(" + widen(hw) + L")"), what.c_str());
    }
    gSettings.encoder = Accel::Auto;
    cs = ConvertSpec();
    cs.vcodec = VCodec::H264;
    {
        std::string fastest = pickEncoder(VCodec::H264, Accel::Auto, RateMode::Quality, caps);
        std::string what = "A -> H.264 with the encoder setting on auto uses the fastest encoder (" + fastest + ")";
        CHECK(runTask(q, TaskKind::Convert, convertSpec(a, cs), t) && logHas(q, t.id, L"(" + widen(fastest) + L")"), what.c_str());
    }
    gSettings.encoder = Accel::Software;

    printf("[audio]\n");
    ConvertSpec au;
    au.audio = AudioTarget::WAV;
    CHECK(runTask(q, TaskKind::Convert, convertSpec(fl, au), t), "FLAC -> WAV finished");
    CHECK(decodedAudio(t.output) == decodedAudio(fl) && t.cls == QualityClass::Lossless, "FLAC -> WAV: samples identical, classed lossless");
    au.audio = AudioTarget::FLAC;
    au.flacLevel = 12;
    std::wstring wav = t.output;
    CHECK(runTask(q, TaskKind::Convert, convertSpec(wav, au), t), "WAV -> FLAC 12 finished");
    CHECK(decodedAudio(t.output) == decodedAudio(fl), "WAV -> FLAC: samples identical");
    au.audio = AudioTarget::MP3;
    au.mp3Vbr = false;
    au.mp3Kbps = 320;
    CHECK(runTask(q, TaskKind::Convert, convertSpec(fl, au), t), "FLAC -> MP3 320 finished");
    probeFile(gDeps.ffprobe(), t.output, m);
    CHECK(m.audio() && m.audio()->codec == "mp3" && t.cls == QualityClass::Lossy, "MP3 output, classed lossy");
    au.audio = AudioTarget::WAV;
    CHECK(runTask(q, TaskKind::Convert, convertSpec(b, au), t), "MKV FLAC track -> WAV finished");
    CHECK(decodedAudio(t.output) == decodedAudio(b), "MKV FLAC -> WAV identical");

    runMatrix(q);

    printf("[queue control]\n");
    {
        std::wstring big = joinPath(gWork, L"big.mkv");
        ff({L"-v", L"error", L"-y", L"-f", L"lavfi", L"-i", L"testsrc2=s=1920x1080:r=30:d=40", L"-c:v", L"libx264", L"-preset", L"ultrafast", L"-qp", L"0", big}, &code);
        ConvertSpec slow;
        slow.vcodec = VCodec::H265;
        slow.accel = Accel::Software;
        slow.speed = "slow";
        slow.quality = 18;
        uint64_t id = q.add(TaskKind::Convert, L"slow", convertSpec(big, slow));
        auto find = [&]() {
            for (auto& v : q.snapshot())
                if (v.id == id) return v;
            return TaskView();
        };
        TaskView v;
        for (int i = 0; i < 300 && !((v = find()).state == TaskState::Running && v.progress > 0.01); i++) Sleep(100);
        CHECK(v.state == TaskState::Running && v.progress > 0, "slow encode running with progress");
        q.pause(id);
        Sleep(400);
        double p1 = find().progress;
        Sleep(2500);
        TaskView pv = find();
        CHECK(pv.state == TaskState::Paused && pv.progress == p1, "pause suspends the encoder (progress frozen)");
        q.resume(id);
        Sleep(2500);
        TaskView rv = find();
        CHECK(rv.state == TaskState::Running && rv.progress > p1, "resume continues from where it paused");
        std::wstring out = rv.output;
        q.cancel(id);
        for (int i = 0; i < 100 && find().state != TaskState::Canceled; i++) Sleep(100);
        CHECK(find().state == TaskState::Canceled, "cancel stops the task");
        Sleep(300);
        CHECK(!out.empty() && !fileExists(out) && !fileExists(out + L".tmp"), "cancel leaves no partial or final file");
        q.retry(id);
        for (int i = 0; i < 100 && find().state != TaskState::Running; i++) Sleep(100);
        CHECK(find().state == TaskState::Running, "retry restarts a canceled task");
        q.remove(id);
        bool gone = false;
        for (int i = 0; i < 100 && !gone; i++) {
            gone = true;
            for (auto& x : q.snapshot()) gone = gone && x.id != id;
            if (!gone) Sleep(100);
        }
        CHECK(gone, "remove on a running task cancels and drops it");
    }

    if (net) {
        printf("[download]\n");
        gSettings.encoder = Accel::Auto;  // downloads convert with the default: the fastest encoder here
        std::wstring url = L"https://www.youtube.com/watch?v=jNQXAC9IVRw";  // "Me at the zoo", 19 s
        auto spec = makeDownloadSpec(gSettings, url, L"", L"", L"");
        CHECK(runTask(q, TaskKind::Download, spec, t, 900), "YouTube video -> MP4 download finished");
        std::wstring got = t.output;
        printf("    %s\n    %s\n", narrow(got).c_str(), narrow(t.result).c_str());
        probeFile(gDeps.ffprobe(), got, m);
        CHECK(m.video() && m.audio(), "download has video and audio");
        // Bit-for-bit: fetch the same format IDs raw (no merge) and compare packet hashes.
        std::wstring raw = joinPath(gWork, L"raw");
        makeDirs(raw);
        std::string fmt = narrow(t.result);
        auto info = runCapture(gDeps.ytdlp(), {L"--ignore-config", L"-J", L"--no-playlist", L"--js-runtimes", L"deno:" + gDeps.deno(), url}, {}, 300000);
        YtResult yr = parseYtJson(info.out);
        DlPrefs prefs;
        prefs.preferH264 = true;
        Selection sel = selectFormats(yr.info, prefs);
        CHECK(sel.ok, "format selection on live data");
        bool same = true;
        for (auto id : {sel.videoId, sel.audioId}) {
            if (id.empty()) continue;
            std::wstring out = joinPath(raw, widen(id) + L".bin");
            auto c = runCapture(gDeps.ytdlp(), {L"--ignore-config", L"-f", widen(id), L"--js-runtimes", L"deno:" + gDeps.deno(), L"--ffmpeg-location",
                                                 gDeps.ffmpegDir(), L"-o", out, L"--no-part", L"--fixup", L"never", url},
                                {}, 300000);
            if (c.code != 0) {
                printf("    raw fetch failed: %s\n", c.err.substr(0, 300).c_str());
                same = false;
                continue;
            }
            bool isVideo = id == sel.videoId;
            std::string h1 = hashOnly(streamHash(out, isVideo ? L"0:v:0" : L"0:a:0"));
            std::string h2 = hashOnly(streamHash(got, isVideo ? L"0:v:0" : L"0:a:0"));
            printf("    %s %s raw=%s merged=%s\n", id.c_str(), isVideo ? "video" : "audio", h1.c_str(), h2.c_str());
            same = same && !h1.empty() && h1 == h2;
        }
        CHECK(same, "downloaded streams are bit-identical to the source formats");
        gSettings.dlOut = DlOut::AudioOriginal;
        CHECK(runTask(q, TaskKind::Download, makeDownloadSpec(gSettings, url, L"", L"", L""), t, 600), "audio original download finished");
        probeFile(gDeps.ffprobe(), t.output, m);
        CHECK(m.audio() && !m.video() && m.audio()->codec == normalizeYtCodec(sel.a.acodec), "audio original keeps the source codec");
        gSettings.dlOut = DlOut::AudioMP3;
        CHECK(runTask(q, TaskKind::Download, makeDownloadSpec(gSettings, url, L"", L"", L""), t, 600), "MP3 download finished");
        CHECK(extension(t.output) == L".mp3" && t.cls == QualityClass::Lossy, "MP3 output labelled lossy");
        gSettings.dlOut = DlOut::MOV;
        CHECK(runTask(q, TaskKind::Download, makeDownloadSpec(gSettings, url, L"", L"", L""), t, 600), "MOV lossless download finished");
        CHECK(extension(t.output) == L".mov" && t.cls == QualityClass::Copy, "H.264/AAC source goes into MOV bit-for-bit");
        CHECK(hashOnly(streamHash(t.output, L"0:v:0")) == hashOnly(streamHash(got, L"0:v:0")), "MOV video packets identical to the MP4 download");
        // A VP9/AV1 source (Big Buck Bunny, CC-BY, Blender Foundation): MP4 converts to H.264 CRF 16 + AAC.
        gSettings.dlOut = DlOut::MP4;
        std::wstring bbb = L"https://www.youtube.com/watch?v=aqz-KE-bpKQ";
        CHECK(runTask(q, TaskKind::Download, makeDownloadSpec(gSettings, bbb, L"", L"0:20", L"0:24"), t, 1200), "4K VP9/AV1 segment -> MP4 finished");
        probeFile(gDeps.ffprobe(), t.output, m);
        printf("    %s\n    %s\n", narrow(t.output).c_str(), narrow(t.result).c_str());
        CHECK(m.video() && m.video()->codec == "h264" && m.audio() && m.audio()->codec == "aac" && extension(t.output) == L".mp4", "MP4 output is H.264 + AAC");
        CHECK(m.video() && m.video()->height >= 1080, "resolution kept (no downscale)");
        CHECK(t.cls == QualityClass::Lossy, "re-encode labelled LOSSY");
        CHECK(!fileExists(joinPath(dirName(t.output), stem(t.output) + L".src.webm")) && !fileExists(joinPath(dirName(t.output), stem(t.output) + L".src.mkv")),
              "intermediate .src download removed");
        CHECK(runTask(q, TaskKind::Download, makeDownloadSpec(gSettings, url, L"", L"0:05", L"0:12"), t, 600), "segment 0:05-0:12 download finished");
        probeFile(gDeps.ffprobe(), t.output, m);
        printf("    segment duration %.2f s\n", m.duration);
        CHECK(m.duration > 5 && m.duration < 11, "segment length roughly 7 s (cut at keyframes, no re-encode)");
        auto pl = runCapture(gDeps.ytdlp(), buildInfoArgs(L"https://www.youtube.com/playlist?list=PLFgquLnL59alCl_2TQvOiD5Vgm1hCaGSI", true, gDeps.deno(), nullptr), {}, 300000);
        YtResult pr = parseYtJson(pl.out);
        CHECK(pr.ok && pr.playlist && pr.entries.size() > 5 && !pr.entries[0].thumb.empty(), "playlist analysis lists entries with thumbnails");
    }
    q.shutdown();

    printf("[restart]\n");
    gSettings.encoder = Accel::Software;  // slow enough to stop mid-encode
    {
        std::wstring big = joinPath(gWork, L"big.mkv");
        ConvertSpec slow;
        slow.vcodec = VCodec::H265;
        slow.accel = Accel::Software;
        slow.speed = "slow";
        uint64_t id;
        {
            Queue q3;
            q3.setRunner(TaskKind::Convert, runConvert);
            q3.init(&gDeps, [] { return gSettings; }, [&] { return caps; }, nullptr);
            id = q3.add(TaskKind::Convert, L"restart-me", convertSpec(big, slow));
            bool running = false;
            for (int i = 0; i < 200 && !running; i++) {
                for (auto& v : q3.snapshot()) running = running || (v.id == id && v.state == TaskState::Running && v.progress > 0);
                if (!running) Sleep(100);
            }
            CHECK(running, "task running before the app closes");
            q3.shutdown();  // what closing the window does
        }
        Queue q4;
        q4.setRunner(TaskKind::Convert, [](RunCtx&) { RunResult r; r.error = L"held for the test"; return r; });
        Settings held = gSettings;
        held.maxEncodes = 0;  // keep it waiting
        q4.init(&gDeps, [held] { return held; }, [&] { return caps; }, nullptr);
        bool found = false;
        for (auto& v : q4.snapshot()) found = found || (v.title == L"restart-me" && v.state == TaskState::Waiting);
        CHECK(found, "an interrupted task is back as waiting after a restart (not dropped as canceled)");
        for (auto& v : q4.snapshot())
            if (v.title == L"restart-me") q4.remove(v.id);
        q4.shutdown();
    }
    gDeps.shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
