// FFprobe analysis: everything the UI shows and the planners need.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ea {

struct StreamInfo {
    int index = 0;
    std::string type;  // video, audio, subtitle, data, attachment
    std::string codec, codecLong, profile;
    // video
    int width = 0, height = 0;
    double fps = 0;
    std::string fpsRaw;  // r_frame_rate, e.g. 24000/1001 (passed through untouched)
    std::string sar, dar, pixFmt, colorRange, colorSpace, colorTransfer, colorPrimaries, fieldOrder;
    int bitDepth = 0;
    int rotation = 0;
    bool attachedPic = false;
    std::string masterDisplay, maxCll;  // x265 syntax, empty if absent
    // audio
    int channels = 0, sampleRate = 0, bitsPerSample = 0;
    std::string layout, sampleFmt;
    // common
    double bitrate = 0;  // bit/s, 0 when unknown
    double duration = 0;
    std::string lang, title, filename, mimetype;
    bool isDefault = false, forced = false;

    bool isVideo() const { return type == "video" && !attachedPic; }
    bool isAudio() const { return type == "audio"; }
    bool isSub() const { return type == "subtitle"; }
    bool hdr() const { return colorTransfer == "smpte2084" || colorTransfer == "arib-std-b67"; }
    bool lossyAudio() const;
    bool textSub() const;
};

struct MediaInfo {
    bool ok = false;
    std::wstring path, error;
    std::string formatName, formatLong;
    double duration = 0, bitrate = 0;
    uint64_t size = 0;
    int chapters = 0;
    std::vector<StreamInfo> streams;

    const StreamInfo* video() const;  // first real video stream
    const StreamInfo* audio() const;  // first audio stream
    int count(const char* type) const;
    double videoBitrate() const;      // estimated when the container does not say
};

bool parseProbeJson(const std::string& json, MediaInfo& out);
bool probeFile(const std::wstring& ffprobe, const std::wstring& path, MediaInfo& out, const std::atomic<bool>* cancel = nullptr);

// One line per stream for the UI: "#0 video h264 High 10 1920x1080 23.976 fps 10-bit 8.2 Mb/s"
std::wstring describeStream(const StreamInfo& s);
std::wstring describeMedia(const MediaInfo& m);  // "mkv · 00:10:12 · 612 MiB · 8.4 Mb/s"
// The short line a list row shows: "H.264 1080p60 · AAC stereo · 3:21 · 183 MiB".
std::wstring briefMedia(const MediaInfo& m);
std::wstring codecLabel(const std::string& codec);  // "h264" -> "H.264", "pcm_s24le" -> "PCM 24-bit"
std::wstring resolutionLabel(int w, int h, double fps);  // "1080p60", "1080p 23.976", "1000x750"
std::wstring channelsLabel(int channels, const std::string& layout = "");  // "stereo", "5.1"
int pixFmtDepth(const std::string& pixFmt);
std::string pixFmtChroma(const std::string& pixFmt);  // "420", "422", "444", "" if unknown

}  // namespace ea
