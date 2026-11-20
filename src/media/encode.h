// Turns a probed file plus user settings into an FFmpeg plan: arguments, a plain explanation,
// the quality class (copy / lossless / lossy), warnings and a size estimate.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "../core/json.h"
#include "compat.h"
#include "probe.h"

namespace ea {

enum class VCodec { Copy, H264, H265, DNxHR_LB, DNxHR_SQ, DNxHR_HQ, DNxHR_HQX, DNxHR_444 };
enum class Accel { Auto, Software, NVENC, QSV, AMF };
enum class RateMode { Quality, Bitrate, Lossless };
enum class ACodec { Copy, AAC, PCM };
enum class AudioTarget { None, FLAC, MP3, WAV };  // None = video conversion
enum class Preset { Lossless, MaxQuality, Balanced, Compact, Custom };

struct ConvertSpec {
    AudioTarget audio = AudioTarget::None;
    Container container = Container::MP4;
    VCodec vcodec = VCodec::H265;
    Accel accel = Accel::Auto;
    RateMode rate = RateMode::Quality;
    int quality = 22;     // CRF scale (x264/x265); hardware CQ/QP is mapped from it
    double mbps = 8;      // bitrate mode, video only
    std::string speed = "medium";  // x264/x265 preset; hardware presets are mapped
    int width = 0, height = 0;     // 0 = source; one side 0 keeps the aspect ratio
    double fps = 0;                // 0 = source
    std::string pixFmt;            // empty = source (or nearest the encoder takes)
    ACodec acodec = ACodec::Copy;
    int aacKbps = 256;
    int pcmBits = 0;  // 0 = exact (source depth for lossless, 32-bit float for lossy sources), 16, 24
    // audio targets
    int flacLevel = 8;
    bool mp3Vbr = false;
    int mp3Kbps = 320;
    int mp3Quality = 0;  // VBR V0..V9
    int bitDepth = 0;    // 0 = source (FLAC/WAV)
    int sampleRate = 0;  // 0 = source
    int channels = 0;    // 0 = source
    bool keepSubs = true;
    bool faststart = true;

    json::Value toJson() const;
    static ConvertSpec fromJson(const json::Value& v);
};

void applyPreset(ConvertSpec& s, Preset p);
const wchar_t* presetName(Preset p);
const wchar_t* vcodecName(VCodec c);
const wchar_t* accelName(Accel a);
bool isDnxhr(VCodec c);

struct EncoderCaps {
    bool probed = false;
    std::map<std::string, bool> works;                       // "h264_nvenc" -> true
    std::map<std::string, std::vector<std::string>> pixFmts;  // encoder -> supported pix_fmts
    bool has(const std::string& enc) const;
    bool supportsPix(const std::string& enc, const std::string& pix) const;
    json::Value toJson() const;
    static EncoderCaps fromJson(const json::Value& v);
};

enum class QualityClass { Copy, Lossless, Lossy };

struct Plan {
    bool ok = false;
    std::wstring error;
    std::vector<std::wstring> args;  // after "-i input", before the output path
    std::wstring ext;                // ".mp4"
    QualityClass cls = QualityClass::Copy;
    std::wstring encoder;            // libx265, h264_nvenc, ... (empty for copy)
    std::vector<std::wstring> lines;     // what happens to each stream
    std::vector<std::wstring> warnings;  // anything the user should know before starting
    double estBytes = 0;
    bool estVariable = false;
    std::wstring estText;
    int outStreams = 0;
    std::vector<int> copiedSourceStreams;  // for verification
};

const wchar_t* className(QualityClass c);  // "STREAM COPY (bit-for-bit)", ...

// Auto takes the fastest encoder that works here: NVENC, then Quick Sync, then AMF, then x264/x265.
std::string pickEncoder(VCodec c, Accel a, RateMode r, const EncoderCaps& caps, std::wstring* why = nullptr);
const wchar_t* pcmBitsName(int bits);  // exact, 16-bit, 24-bit, 32-bit float
Plan planConvert(const MediaInfo& m, const ConvertSpec& s, const EncoderCaps& caps);

enum class TrackAction { Copy, ToAAC, ToPCM, ToText, Exclude };
const wchar_t* trackActionName(TrackAction a);

struct RemuxSpec {
    Container container = Container::MP4;
    std::map<int, TrackAction> actions;  // by source stream index; missing = default
    bool audioPcm = false;  // false keeps the original audio (PCM only where the container needs it); true decodes every track to PCM
    int pcmBits = 0;        // 0 = exact (source depth, 32-bit float for lossy sources), 16, 24, 32 (float)
    bool chapters = true;
    bool faststart = true;
    json::Value toJson() const;
    static RemuxSpec fromJson(const json::Value& v);
};

TrackAction defaultAction(Container c, const StreamInfo& s, bool audioPcm = false);
std::vector<TrackAction> allowedActions(Container c, const StreamInfo& s);
Plan planRemux(const MediaInfo& m, const RemuxSpec& s);

// Copy / Remux / Convert advice for a file and the target the user picked.
struct Advice {
    enum Mode { Copy, Remux, Convert } mode = Convert;
    std::wstring text;
};
Advice adviseConvert(const MediaInfo& m, const ConvertSpec& s);

// Codec efficiency relative to H.264 (lower = fewer bits for the same quality).
double codecEfficiency(const std::string& codec);

}  // namespace ea
