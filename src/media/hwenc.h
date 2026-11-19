// Detects which hardware encoders really work (a tiny test encode each) and the pixel formats
// every encoder accepts. Runs on a worker thread; the result is cached per FFmpeg build.
#pragma once
#include <atomic>
#include <string>

#include "encode.h"

namespace ea {

EncoderCaps detectEncoders(const std::wstring& ffmpeg, const std::atomic<bool>* cancel = nullptr);
std::vector<std::string> parsePixFmts(const std::string& encoderHelp);  // from `ffmpeg -h encoder=X`

}  // namespace ea
