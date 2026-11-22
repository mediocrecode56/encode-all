// Small string, path and file helpers. Everything is UTF-16 at the Win32 edge and UTF-8 inside JSON.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ea {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

std::wstring trim(const std::wstring& s);
std::string trim(const std::string& s);
std::wstring lower(std::wstring s);
std::string lower(std::string s);
bool startsWith(const std::wstring& s, const std::wstring& p);
bool startsWith(const std::string& s, const std::string& p);
bool contains(const std::string& s, const std::string& needle);
std::vector<std::wstring> splitLines(const std::wstring& s);
std::vector<std::string> split(const std::string& s, char sep);
std::wstring replaceAll(std::wstring s, const std::wstring& from, const std::wstring& to);

// Formatting for the UI.
std::wstring fmtBytes(double bytes);       // 612.4 MiB
std::wstring fmtBitrate(double bps);       // 8.2 Mb/s
std::wstring fmtDuration(double seconds);  // 03:12 or 1:02:03
std::wstring fmtFixed(double v, int decimals);
std::wstring fmtFps(double fps);           // 23.976, 30, 59.94
std::wstring ellipsize(const std::wstring& s, size_t width);

// "1:02:03.5", "02:03", "90", "90.5". Returns false on junk.
bool parseTime(const std::wstring& s, double& out);
// "1-5,8,10-12" into 1-based indices (sorted, unique, clamped to [1, max]).
bool parseRanges(const std::wstring& s, int max, std::vector<int>& out);
// Fraction "24000/1001" or decimal into a double. 0 on junk.
double parseRate(const std::string& s);

// Command lines (CommandLineToArgvW quoting rules).
std::wstring quoteArg(const std::wstring& a);
std::wstring joinArgs(const std::vector<std::wstring>& args);

// Paths and files. All functions accept long paths.
std::wstring exePath();
std::wstring dirName(const std::wstring& p);
std::wstring fileName(const std::wstring& p);
std::wstring stem(const std::wstring& p);
std::wstring extension(const std::wstring& p);  // ".mp4", lower case
std::wstring joinPath(const std::wstring& a, const std::wstring& b);
bool fileExists(const std::wstring& p);
bool dirExists(const std::wstring& p);
bool makeDirs(const std::wstring& p);
uint64_t fileSize(const std::wstring& p);
uint64_t freeSpace(const std::wstring& dir);
bool readFile(const std::wstring& p, std::string& out);
bool writeFileAtomic(const std::wstring& p, const std::string& data);
bool moveReplace(const std::wstring& from, const std::wstring& to);
bool removeTree(const std::wstring& p);
// A path that does not exist yet: "name.ext", "name (1).ext", "name (2).ext" ...
std::wstring uniquePath(const std::wstring& desired);
std::wstring sanitizeFileName(const std::wstring& name);
std::wstring downloadsFolder();
std::wstring localAppData();
int64_t nowUnix();
std::wstring nowStamp();  // 2026-11-19 23:41:05

}  // namespace ea
