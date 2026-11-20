// Real-media checks through the app's own code paths (Deps, Queue, runners). Needs network on the
// first run (tools install into build\data, portable). Run: build\integration_tests.exe [--no-net]
#include <windows.h>

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
