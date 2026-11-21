// The three task runners. Each one plans, runs its process, verifies the output and only then
// gives the file its final name.
#pragma once
#include "queue.h"

namespace ea {

RunResult runDownload(RunCtx& c);
RunResult runConvert(RunCtx& c);
RunResult runRemux(RunCtx& c);

// Builds a download task spec from the current settings (used by the Download view).
json::Value makeDownloadSpec(const Settings& s, const std::wstring& url, const std::wstring& infoFile, const std::wstring& sectionStart,
                             const std::wstring& sectionEnd);
std::wstring tmpDir();
void cleanTmp();

// FFmpeg -progress parser: returns true when the line updated `outSec` or `speed`.
bool parseFfProgress(const std::string& line, double& outSec, double& speed, bool& end);
// Every output stream against the plan's expectations (type, codec, size, depth, chroma, channels, rate).
bool checkExpect(const MediaInfo& out, const std::vector<OutExpect>& expect, std::wstring& err);
// Hardware-encoder failures that a software retry fixes.
bool isHwEncoderFailure(const std::string& stderrText);

}  // namespace ea
