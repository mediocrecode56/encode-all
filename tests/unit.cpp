// Fast logic tests (no processes, no network). Run: build\unit_tests.exe
#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/app/appupdate.h"
#include "../src/app/deps.h"
#include "../src/app/runners.h"
#include "../src/core/json.h"
#include "../src/core/net.h"
#include "../src/core/util.h"
#include "../src/dl/ytdlp.h"
#include "../src/media/encode.h"
#include "../src/media/hwenc.h"
#include "../src/media/probe.h"

using namespace ea;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (c) g_pass++;                                                 \
        else {                                                           \
            g_fail++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static bool hasArg(const std::vector<std::wstring>& a, const std::wstring& x) {
    for (auto& s : a)
        if (s == x) return true;
    return false;
}
static bool hasPair(const std::vector<std::wstring>& a, const std::wstring& k, const std::wstring& v) {
    for (size_t i = 0; i + 1 < a.size(); i++)
        if (a[i] == k && a[i + 1] == v) return true;
    return false;
}
static bool anyContains(const std::vector<std::wstring>& a, const std::wstring& x) {
    for (auto& s : a)
        if (s.find(x) != std::wstring::npos) return true;
    return false;
}

static void testJson() {
    json::Value v;
    CHECK(json::parse(R"({"a":1,"b":[true,null,"x\u00e9\ud83d\ude00"],"c":{"d":-2.5e3},"n":NaN})", v));
    CHECK(v["a"].num() == 1);
    CHECK(v["b"][0].boolean());
    CHECK(v["b"][1].isNull());
    CHECK(v["b"][2].str() == "x\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(v["c"]["d"].num() == -2500);
    CHECK(v["missing"]["deep"].isNull());
    CHECK(v["n"].num(7) == 7);  // NaN reads as the default
    json::Value back;
    CHECK(json::parse(json::dump(v, true), back));
    CHECK(back["b"][2].str() == v["b"][2].str());
    CHECK(!json::parse("{\"a\":}", v));
    CHECK(!json::parse("[1,2", v));
    CHECK(json::Value("123").num() == 123);
}

static void testUtil() {
    double t;
    CHECK(parseTime(L"1:02:03.5", t) && t == 3723.5);
    CHECK(parseTime(L"02:03", t) && t == 123);
    CHECK(parseTime(L"90", t) && t == 90);
    CHECK(!parseTime(L"1:2:3:4", t));
    CHECK(!parseTime(L"abc", t));
    std::vector<int> r;
    CHECK(parseRanges(L"1-3,5, 8-", 10, r) && r.size() == 7 && r[0] == 1 && r[3] == 5 && r.back() == 10);
    CHECK(parseRanges(L"12", 5, r) == false);
    CHECK(quoteArg(L"a b") == L"\"a b\"");
    CHECK(quoteArg(L"C:\\dir\\") == L"C:\\dir\\");
    CHECK(quoteArg(L"C:\\my dir\\") == L"\"C:\\my dir\\\\\"");
    CHECK(quoteArg(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(fmtDuration(3723) == L"1:02:03");
    CHECK(fmtDuration(65) == L"01:05");
    CHECK(fmtFps(23.976023) == L"23.976");
    CHECK(fmtFps(30.0) == L"30");
    CHECK(parseRate("24000/1001") > 23.97 && parseRate("24000/1001") < 23.98);
    CHECK(sanitizeFileName(L"a:b?c*.") == L"a_b_c_");
    CHECK(extension(L"C:\\x\\Movie.MKV") == L".mkv");
    CHECK(stem(L"C:\\x\\Movie.final.mkv") == L"Movie.final");
}

static const char* kProbe = R"({
 "streams":[
  {"index":0,"codec_name":"h264","codec_type":"video","profile":"High 10","width":1920,"height":1080,"pix_fmt":"yuv420p10le",
   "r_frame_rate":"24000/1001","avg_frame_rate":"24000/1001","color_transfer":"smpte2084","color_primaries":"bt2020","color_space":"bt2020nc",
   "disposition":{"default":1,"attached_pic":0},"tags":{"BPS":"8000000"},
   "side_data_list":[{"side_data_type":"Mastering display metadata","red_x":"35400/50000","red_y":"14600/50000","green_x":"8500/50000","green_y":"39850/50000",
     "blue_x":"6550/50000","blue_y":"2300/50000","white_point_x":"15635/50000","white_point_y":"16450/50000","min_luminance":"50/10000","max_luminance":"10000000/10000"},
     {"side_data_type":"Content light level metadata","max_content":1000,"max_average":400}]},
  {"index":1,"codec_name":"truehd","codec_type":"audio","channels":8,"channel_layout":"7.1","sample_rate":"48000","sample_fmt":"s32","bits_per_raw_sample":"24",
   "disposition":{"default":1},"tags":{"language":"eng"}},
  {"index":2,"codec_name":"aac","codec_type":"audio","channels":2,"sample_rate":"48000","sample_fmt":"fltp","bit_rate":"192000","disposition":{"default":0},"tags":{"language":"spa"}},
  {"index":3,"codec_name":"hdmv_pgs_subtitle","codec_type":"subtitle","disposition":{"default":0},"tags":{"language":"eng"}},
  {"index":4,"codec_name":"subrip","codec_type":"subtitle","disposition":{"default":0}},
  {"index":5,"codec_name":"ttf","codec_type":"attachment","tags":{"filename":"font.ttf","mimetype":"font/ttf"}}
 ],
 "chapters":[{"id":0},{"id":1}],
 "format":{"format_name":"matroska,webm","duration":"600.0","size":"700000000","bit_rate":"9333333"}
})";

static void testProbe() {
    MediaInfo m;
    CHECK(parseProbeJson(kProbe, m));
    CHECK(m.streams.size() == 6);
    CHECK(m.chapters == 2);
    const StreamInfo* v = m.video();
    CHECK(v && v->width == 1920 && v->bitDepth == 10 && v->hdr());
    CHECK(v->bitrate == 8000000);
    CHECK(v->masterDisplay == "G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,50)");
    CHECK(v->maxCll == "1000,400");
    CHECK(m.streams[1].bitsPerSample == 24 && !m.streams[1].lossyAudio());
    CHECK(m.streams[2].lossyAudio());
    CHECK(m.streams[4].textSub() && !m.streams[3].textSub());
    CHECK(describeStream(*v).find(L"HDR10") != std::wstring::npos);
}

static void testCompat() {
    StreamInfo s;
    s.type = "audio";
    s.codec = "opus";
    CHECK(fitStream(Container::MP4, s) == Fit::Copy);
    CHECK(fitStream(Container::MOV, s) == Fit::Convert);
    s.codec = "truehd";
    CHECK(fitStream(Container::MP4, s) == Fit::Convert);
    CHECK(fitStream(Container::MKV, s) == Fit::Copy);
    s.codec = "pcm_s24le";
    CHECK(fitStream(Container::MP4, s) == Fit::Copy);
    s.type = "video";
    s.codec = "vp9";
    CHECK(fitStream(Container::MP4, s) == Fit::Copy);
    CHECK(fitStream(Container::MOV, s) == Fit::No);
    s.codec = "prores";
    CHECK(fitStream(Container::MP4, s) == Fit::No);
    CHECK(fitStream(Container::MOV, s) == Fit::Copy);
    s.type = "subtitle";
    s.codec = "hdmv_pgs_subtitle";
    CHECK(fitStream(Container::MP4, s) == Fit::No);
    CHECK(fitStream(Container::MKV, s) == Fit::Copy);
    s.codec = "subrip";
    CHECK(fitStream(Container::MP4, s) == Fit::Convert);
    CHECK(ytCodecFitsMp4("av01.0.08M.08", "opus"));
    CHECK(ytCodecFitsMp4("avc1.640028", "mp4a.40.2"));
    CHECK(!ytCodecFitsMp4("vp9", "vorbis"));
    CHECK(normalizeYtCodec("vp09.02.51.10.01.09.16.09.00") == "vp9");
    MediaInfo m;
    parseProbeJson(kProbe, m);
    ContainerAdvice a = adviseContainer(Container::MP4, m);
    CHECK(a.best == Container::MKV);
    CHECK(a.impossible >= 2);  // PGS + attachment
    MediaInfo chap;
    parseProbeJson(R"({"streams":[{"index":0,"codec_type":"video","codec_name":"h264"},{"index":1,"codec_type":"audio","codec_name":"aac"},
      {"index":2,"codec_type":"data","codec_name":"bin_data"}],"chapters":[{"id":0}],"format":{"duration":"19"}})", chap);
    ContainerAdvice ca = adviseContainer(Container::MP4, chap);
    CHECK(ca.best == Container::MP4 && ca.impossible == 0);  // the MP4 chapter text track does not push to another container
}

static YtFormat F(const char* id, const char* v, const char* a, int w, int h, double fps, double tbr, const char* proto = "https", const char* dr = "SDR") {
    YtFormat f;
    f.id = id;
    f.vcodec = v;
    f.acodec = a;
    f.width = w;
    f.height = h;
    f.fps = fps;
    f.tbr = tbr;
    if (std::string(v) == "none") f.abr = tbr;
    else f.vbr = tbr;
    f.protocol = proto;
    f.dynamicRange = dr;
    f.ext = std::string(v) == "none" ? (std::string(a) == "opus" ? "webm" : "m4a") : (std::string(v).rfind("avc", 0) == 0 || std::string(v).rfind("av01", 0) == 0 ? "mp4" : "webm");
    return f;
}

static void testFormats() {
    YtInfo in;
    in.duration = 100;
    in.formats = {F("140", "none", "mp4a.40.2", 0, 0, 0, 129.8), F("251", "none", "opus", 0, 0, 0, 135.0), F("251-drc", "none", "opus", 0, 0, 0, 140.0),
                  F("137", "avc1.640028", "none", 1920, 1080, 30, 4000), F("248", "vp9", "none", 1920, 1080, 30, 2500),
                  F("399", "av01.0.08M.08", "none", 1920, 1080, 30, 2000), F("313", "vp9", "none", 3840, 2160, 30, 16000),
                  F("18", "avc1.42001E", "mp4a.40.2", 640, 360, 30, 500), F("230", "avc1.4D401F", "none", 1920, 1080, 30, 9000, "m3u8_native")};
    DlPrefs p;
    p.out = DlOut::MP4;
    p.preferH264 = true;
    Selection s = selectFormats(in, p);
    CHECK(s.ok);
    CHECK(s.videoId == "313");  // 4K VP9 beats 1080p H.264 even with the H.264 preference
    CHECK(s.audioId == "251");  // more data, DRC skipped
    CHECK(s.finalExt == L".mp4" && s.needsConvert && s.container == Container::WEBM);  // downloaded as is, then H.264/AAC
    in.formats.erase(in.formats.begin() + 6);  // drop 4K
    s = selectFormats(in, p);
    CHECK(s.videoId == "137");  // equal 1080p30: H.264 preferred, DASH over HLS
    p.preferH264 = true;
    {
        DlPrefs aac = p;
        std::vector<YtFormat> keep = in.formats;
        in.formats.erase(in.formats.begin() + 1);  // drop Opus so AAC is the best audio
        Selection h = selectFormats(in, aac);
        CHECK(h.videoId == "137" && h.audioId == "140" && !h.needsConvert && h.container == Container::MP4);  // already H.264/AAC: pure copy
        aac.hevc = true;
        h = selectFormats(in, aac);
        CHECK(h.needsConvert && h.summary.find(L"H.265 CRF 16") != std::wstring::npos);  // H.264 source re-encoded for an H.265 target
        aac.hevc = false;
        aac.out = DlOut::MOV;
        h = selectFormats(in, aac);
        CHECK(h.needsConvert && h.finalExt == L".mov");
        in.formats = keep;
    }
    p.preferH264 = false;
    s = selectFormats(in, p);
    CHECK(s.videoId == "399");  // best codec when no preference
    in.formats.push_back(F("400", "av01.0.09M.10", "none", 1920, 1080, 30, 3000, "https", "HDR10"));
    p.preferH264 = true;
    s = selectFormats(in, p);
    CHECK(s.videoId == "400");  // HDR keeps more colour information than an SDR H.264 encode
    in.formats.push_back(F("600", "none", "vorbis", 0, 0, 0, 300));
    s = selectFormats(in, p);
    CHECK(s.audioId == "600" && s.needsConvert);  // best audio wins whatever the codec; MP4 converts it to AAC
    p.out = DlOut::Original;
    s = selectFormats(in, p);
    CHECK(!s.needsConvert && s.finalExt == s.ext);  // ORIGINAL: as it comes
    p.out = DlOut::MP4;
    p.out = DlOut::AudioOriginal;
    s = selectFormats(in, p);
    CHECK(s.ok && s.videoId == "600" && s.audioId.empty() && s.ext == L".ogg");
    p.out = DlOut::Original;
    in.formats = {F("22", "avc1.64001F", "mp4a.40.2", 1280, 720, 30, 2000)};
    s = selectFormats(in, p);
    CHECK(s.ok && s.videoId == "22" && s.audioId.empty() && s.container == Container::MP4);
}

static void testYtArgs() {
    DlOptions o;
    o.url = L"https://example.com/v";
    o.outDir = L"C:\\Out";
    o.outStem = L"Title [id]";
    o.sel.videoId = "137";
    o.sel.audioId = "140";
    o.sel.container = Container::MP4;
    o.sel.ext = L".mp4";
    o.sectionStart = L"10";
    o.sectionEnd = L"1:00";
    auto a = buildDownloadArgs(o);
    CHECK(hasPair(a, L"-f", L"137+140"));
    CHECK(hasPair(a, L"--merge-output-format", L"mp4"));
    CHECK(hasPair(a, L"--download-sections", L"*10-1:00"));
    CHECK(!hasArg(a, L"--force-keyframes-at-cuts"));
    CHECK(!hasArg(a, L"--recode-video"));
    CHECK(!hasArg(a, L"--audio-format"));
    CHECK(!hasArg(a, L"--postprocessor-args"));
    o.audioExtract = true;
    o.sel.audioId.clear();
    a = buildDownloadArgs(o);
    CHECK(hasArg(a, L"-x") && !hasArg(a, L"--audio-format"));
    DlProgress p;
    CHECK(parseProgressLine("EAP|downloading|1048576|4194304|NA|524288.5|6|137", p));
    CHECK(p.isDownload && p.done == 1048576 && p.total == 4194304 && p.formatId == "137" && p.eta == 6);
    CHECK(parseProgressLine("EAP|downloading|100|NA|5000|NA|NA|140", p) && p.total == 5000 && p.speed < 0);
    CHECK(parseProgressLine("EAPP|started|Merger", p) && p.isPost && p.postprocessor == "Merger");
    std::wstring m, h;
    CHECK(classifyError("ERROR: [youtube] abc: Sign in to confirm you're not a bot", m, h) == DlError::Auth);
    CHECK(classifyError("ERROR: [youtube] abc: nsig extraction failed", m, h) == DlError::Extraction);
    CHECK(classifyError("ERROR: unable to download video data: HTTP Error 403: Forbidden", m, h) == DlError::Extraction);
    CHECK(classifyError("ERROR: [youtube] x: Video unavailable", m, h) == DlError::Unavailable);
    CHECK(classifyError("ERROR: Unable to download webpage: <urlopen error timed out>", m, h) == DlError::Network);
    CHECK(classifyError("OSError: [Errno 28] No space left on device", m, h) == DlError::Disk);
}

static void testPlans() {
    MediaInfo m;
    parseProbeJson(kProbe, m);
    EncoderCaps caps;  // nothing probed: software only
    ConvertSpec s;
    applyPreset(s, Preset::Balanced);
    Plan p = planConvert(m, s, caps);
    CHECK(p.ok);
    CHECK(hasPair(p.args, L"-c:0", L"libx265"));
    CHECK(hasPair(p.args, L"-crf:0", L"22"));
    CHECK(anyContains(p.args, L"master-display=G(8500,39850)"));
    CHECK(hasPair(p.args, L"-color_trc:0", L"smpte2084"));
    CHECK(hasPair(p.args, L"-c:1", L"pcm_s24le"));  // TrueHD cannot be copied into MP4: exact PCM, not AAC
    CHECK(hasPair(p.args, L"-c:2", L"copy"));       // AAC copied
    CHECK(hasPair(p.args, L"-c:3", L"mov_text"));   // SRT converted, PGS left out with a warning
    CHECK(anyContains(p.warnings, L"image-based"));
    CHECK(anyContains(p.warnings, L"font.ttf"));
    CHECK(p.cls == QualityClass::Lossy);
    CHECK(p.estVariable && p.estBytes > 0);
    CHECK(!hasArg(p.args, L"-pix_fmt:0"));  // 10-bit source kept as is by x265
    // H.264 lossless with the 10-bit source: x264 takes yuv420p10le.
    applyPreset(s, Preset::Lossless);
    p = planConvert(m, s, caps);
    CHECK(hasPair(p.args, L"-qp:0", L"0") && hasPair(p.args, L"-c:0", L"libx264"));
    CHECK(p.cls == QualityClass::Lossless);
    // NVENC H.264 cannot take 10-bit: falls to 8-bit and says so.
    caps.works["h264_nvenc"] = true;
    s = ConvertSpec();
    s.vcodec = VCodec::H264;
    s.accel = Accel::NVENC;
    p = planConvert(m, s, caps);
    CHECK(hasPair(p.args, L"-c:0", L"h264_nvenc"));
    CHECK(hasPair(p.args, L"-pix_fmt:0", L"yuv420p"));
    CHECK(anyContains(p.warnings, L"Bit depth drops from 10 to 8"));
    // DNxHR HQX forces MOV and 10-bit 4:2:2.
    s.vcodec = VCodec::DNxHR_HQX;
    p = planConvert(m, s, caps);
    CHECK(p.ext == L".mov" && hasPair(p.args, L"-profile:0", L"dnxhr_hqx") && hasPair(p.args, L"-pix_fmt:0", L"yuv422p10le"));
    // Bitrate above what the source needs is flagged.
    s = ConvertSpec();
    s.vcodec = VCodec::H264;
    s.rate = RateMode::Bitrate;
    s.mbps = 40;
    p = planConvert(m, s, caps);
    CHECK(anyContains(p.warnings, L"exceeds what the source needs"));
    // Audio targets.
    ConvertSpec a;
    a.audio = AudioTarget::WAV;
    p = planConvert(m, a, caps);
    CHECK(p.ok && hasPair(p.args, L"-c:0", L"pcm_s24le") && p.cls == QualityClass::Lossless);  // TrueHD 24-bit decoded exactly
    a.audio = AudioTarget::MP3;
    p = planConvert(m, a, caps);
    CHECK(p.cls == QualityClass::Lossy && hasPair(p.args, L"-b:0", L"320k"));
    a.audio = AudioTarget::FLAC;
    p = planConvert(m, a, caps);
    CHECK(p.cls == QualityClass::Lossless && hasPair(p.args, L"-bits_per_raw_sample:0", L"24"));
    // Remux: MKV keeps everything bit-for-bit; MP4 lists what it cannot hold.
    RemuxSpec r;
    r.container = Container::MKV;
    p = planRemux(m, r);
    CHECK(p.ok && p.cls == QualityClass::Copy && p.outStreams == 6);
    r.container = Container::MP4;
    p = planRemux(m, r);
    CHECK(p.ok && p.cls == QualityClass::Lossless);  // TrueHD -> PCM is exact
    CHECK(p.outStreams == 4);
    CHECK(anyContains(p.warnings, L"MKV keeps it"));
    r.actions[1] = TrackAction::ToAAC;
    p = planRemux(m, r);
    CHECK(p.cls == QualityClass::Lossy);
    r.actions[1] = TrackAction::Exclude;
    r.actions[2] = TrackAction::Exclude;
    r.actions[0] = TrackAction::Exclude;
    r.actions[4] = TrackAction::Exclude;
    p = planRemux(m, r);
    CHECK(!p.ok);
    // Advice: same codec, no changes -> remux.
    ConvertSpec same;
    same.vcodec = VCodec::H264;
    Advice ad = adviseConvert(m, same);
    CHECK(ad.mode != Advice::Convert);
    // Spec round trip.
    ConvertSpec back = ConvertSpec::fromJson(s.toJson());
    CHECK(back.vcodec == s.vcodec && back.mbps == s.mbps && back.rate == s.rate);
}

static void testMisc() {
    CHECK(findHash("abc\n" + std::string(64, 'a') + "  yt-dlp.exe\n" + std::string(64, 'b') + " *yt-dlp_x86.exe\n", "yt-dlp.exe") == std::string(64, 'a'));
    CHECK(extractHash("Algorithm : SHA256\r\nHash      : CBA84B26E0DF5AE7F74FC66B86A95B866872A86292EBD84FA1E959927EDD93BF\r\nPath : x", "") ==
          "cba84b26e0df5ae7f74fc66b86a95b866872a86292ebd84fa1e959927edd93bf");
    CHECK(extractHash(std::string(64, 'c') + "  ./x/ffmpeg-master-latest-win64-gpl.zip", "ffmpeg-master-latest-win64-gpl.zip") == std::string(64, 'c'));
    CHECK(sha256Data("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string v, z, h;
    CHECK(parseReleaseSums(std::string(64, 'd') + "  EncodeAll-Setup-1.2.3.exe\n" + std::string(64, 'e') + "  EncodeAll-1.2.3-win64.zip\n", v, z, h));
    CHECK(v == "1.2.3" && z == "EncodeAll-1.2.3-win64.zip" && h == std::string(64, 'e'));
    CHECK(compareVersions("1.0.10", "1.0.9") > 0 && compareVersions("1.0", "1.0.0") == 0 && compareVersions("0.9.9", "1.0.0") < 0);
    double sec = 0, sp = 0;
    bool end = false;
    CHECK(parseFfProgress("out_time_us=1500000", sec, sp, end) && sec == 1.5);
    CHECK(parseFfProgress("speed=2.35x", sec, sp, end) && sp > 2.34);
    CHECK(parseFfProgress("progress=end", sec, sp, end) && end);
    CHECK(isHwEncoderFailure("[h264_nvenc @ 0x1] OpenEncodeSessionEx failed: unsupported device (2)"));
    CHECK(!isHwEncoderFailure("Invalid data found when processing input"));
    CHECK(pickThumb({{"https://i.ytimg.com/vi/fOT0BUpITw8/hqdefault.jpg?sqp=-oay&rs=AOn", 336}}) == "https://i.ytimg.com/vi/fOT0BUpITw8/mqdefault.jpg");
    CHECK(pickThumb({{"https://i.ytimg.com/vi_webp/abc/maxresdefault.webp", 0}}) == "https://i.ytimg.com/vi/abc/mqdefault.jpg");
    CHECK(pickThumb({{"https://cdn.example/t_small.jpg", 120}, {"https://cdn.example/t_med.jpg", 320}, {"https://cdn.example/t_big.webp", 330}}) == "https://cdn.example/t_med.jpg");
    auto pf = parsePixFmts("Encoder libx264 [libx264 H.264]:\n    Supported pixel formats: yuv420p yuvj420p yuv422p\n");
    CHECK(pf.size() == 3 && pf[2] == "yuv422p");
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    testJson();
    testUtil();
    testProbe();
    testCompat();
    testFormats();
    testYtArgs();
    testPlans();
    testMisc();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
