#include "hwenc.h"

#include <algorithm>
#include <thread>
#include <vector>

#include "../core/process.h"
#include "../core/util.h"

namespace ea {

std::vector<std::string> parsePixFmts(const std::string& help) {
    std::vector<std::string> out;
    for (auto& line : split(help, '\n')) {
        std::string l = trim(line);
        const std::string key = "Supported pixel formats:";
        if (!startsWith(l, key)) continue;
        for (auto& f : split(trim(l.substr(key.size())), ' '))
            if (!trim(f).empty()) out.push_back(trim(f));
    }
    return out;
}

EncoderCaps detectEncoders(const std::wstring& ffmpeg, const std::atomic<bool>* cancel) {
    EncoderCaps caps;
    auto list = runCapture(ffmpeg, {L"-hide_banner", L"-encoders"}, {}, 20000, cancel);
    if (!list.started || list.code != 0) return caps;
    const char* all[] = {"libx264", "libx265", "dnxhd", "h264_nvenc", "hevc_nvenc", "h264_qsv", "hevc_qsv", "h264_amf", "hevc_amf"};
    std::vector<std::string> present;
    for (auto e : all) {
        // Lines look like " V....D h264_nvenc           NVIDIA NVENC H.264 encoder".
        if (contains(list.out, std::string(" ") + e + " ")) present.push_back(e);
        else caps.works[e] = false;
    }
    // Test encodes in parallel: each is ~0.3 s when the hardware exists, a quick failure when not.
    std::vector<std::thread> ts;
    std::vector<int> ok(present.size(), 0);
    std::vector<std::string> help(present.size());
    for (size_t i = 0; i < present.size(); i++) {
        ts.emplace_back([&, i] {
            const std::string& e = present[i];
            auto h = runCapture(ffmpeg, {L"-hide_banner", L"-h", L"encoder=" + widen(e)}, {}, 15000, cancel);
            help[i] = h.out;
            if (!contains(e, "_nvenc") && !contains(e, "_qsv") && !contains(e, "_amf")) {
                ok[i] = 1;
                return;
            }
            std::wstring pix = contains(e, "_qsv") || contains(e, "_amf") ? L"nv12" : L"yuv420p";
            auto r = runCapture(ffmpeg, {L"-hide_banner", L"-v", L"error", L"-f", L"lavfi", L"-i", L"color=c=black:s=256x256:r=30:d=0.2", L"-frames:v", L"4",
                                         L"-pix_fmt", pix, L"-c:v", widen(e), L"-f", L"null", L"-"},
                                {}, 20000, cancel);
            ok[i] = r.code == 0;
        });
    }
    for (auto& t : ts) t.join();
    for (size_t i = 0; i < present.size(); i++) {
        caps.works[present[i]] = ok[i] != 0;
        auto f = parsePixFmts(help[i]);
        if (!f.empty()) caps.pixFmts[present[i]] = f;
    }
    caps.probed = true;
    return caps;
}

}  // namespace ea
