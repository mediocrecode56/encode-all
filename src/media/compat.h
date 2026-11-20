// Which streams each container can hold, measured against FFmpeg's muxers (see tests).
#pragma once
#include <string>
#include <vector>

#include "probe.h"

namespace ea {

enum class Container { MP4, MOV, MKV, M4A, MP3, FLAC, WAV, OPUS, WEBM };

enum class Fit {
    Copy,     // the stream goes in bit-for-bit
    Convert,  // only after conversion (text subtitles to mov_text, a lossless PCM decode, ...)
    No        // cannot be held at all
};

const wchar_t* containerName(Container c);  // "MP4"
const wchar_t* containerExt(Container c);   // ".mp4"
const char* containerMuxer(Container c);    // ffmpeg -f value
bool containerFromName(const std::wstring& s, Container& out);

Fit fitStream(Container c, const StreamInfo& s);
// Same question for yt-dlp format codecs ("avc1.640028", "opus", "mp4a.40.2", "av01.0.08M.08").
bool ytCodecFitsMp4(const std::string& vcodec, const std::string& acodec);
std::string normalizeYtCodec(const std::string& c);  // "avc1.64..." -> "h264", "mp4a.40.2" -> "aac", "none" -> ""
bool chaptersFit(Container c);
bool attachmentsFit(Container c);

// MP4 or MOV, whichever keeps every stream bit-for-bit (the wanted one when neither does). Used by Remux.
struct ContainerAdvice {
    Container best = Container::MP4;
    int copyable = 0, convertible = 0, impossible = 0;
    std::vector<std::wstring> notes;  // human sentences, one per issue
};
ContainerAdvice adviseContainer(Container wanted, const MediaInfo& m);

}  // namespace ea
