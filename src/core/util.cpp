#include "util.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ea {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

template <class S> static S trimT(const S& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned)s[a] <= ' ') a++;
    while (b > a && (unsigned)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}
std::wstring trim(const std::wstring& s) { return trimT(s); }
std::string trim(const std::string& s) { return trimT(s); }

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}
std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}
bool startsWith(const std::wstring& s, const std::wstring& p) { return s.compare(0, p.size(), p) == 0; }
bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool contains(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }

std::vector<std::wstring> splitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

std::wstring replaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::wstring::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

std::wstring fmtFixed(double v, int d) {
    wchar_t b[64];
    swprintf(b, 64, L"%.*f", d, v);
    return b;
}

std::wstring fmtBytes(double v) {
    if (v < 0) return L"?";
    const wchar_t* u[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    return fmtFixed(v, i == 0 ? 0 : (v < 10 ? 2 : 1)) + L" " + u[i];
}

std::wstring fmtBitrate(double bps) {
    if (bps <= 0) return L"?";
    if (bps >= 1e6) return fmtFixed(bps / 1e6, bps >= 1e8 ? 0 : 1) + L" Mb/s";
    return fmtFixed(bps / 1e3, 0) + L" kb/s";
}

std::wstring fmtDuration(double s) {
    if (!(s >= 0) || s > 1e9) return L"--:--";
    long t = (long)(s + 0.5);
    wchar_t b[32];
    if (t >= 3600)
        swprintf(b, 32, L"%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else
        swprintf(b, 32, L"%02ld:%02ld", t / 60, t % 60);
    return b;
}

std::wstring fmtFps(double f) {
    if (f <= 0) return L"?";
    double r = std::round(f);
    if (std::fabs(f - r) < 0.005) return fmtFixed(r, 0);
    std::wstring s = fmtFixed(f, 3);
    while (!s.empty() && s.back() == L'0') s.pop_back();
    return s;
}

std::wstring ellipsize(const std::wstring& s, size_t w) {
    if (s.size() <= w) return s;
    if (w <= 3) return s.substr(0, w);
    return s.substr(0, w - 3) + L"...";
}

bool parseTime(const std::wstring& in, double& out) {
    std::wstring s = trim(in);
    if (s.empty()) return false;
    double total = 0, part = 0, frac = 0, scale = 0.1;
    int fields = 0;
    bool inFrac = false, any = false;
    for (wchar_t c : s) {
        if (c >= L'0' && c <= L'9') {
            any = true;
            if (inFrac) {
                frac += (c - L'0') * scale;
                scale /= 10;
            } else {
                part = part * 10 + (c - L'0');
            }
        } else if (c == L':') {
            if (inFrac || !any) return false;
            total = total * 60 + part;
            part = 0;
            any = false;
            if (++fields > 2) return false;
        } else if (c == L'.' || c == L',') {
            if (inFrac) return false;
            inFrac = true;
        } else {
            return false;
        }
    }
    if (!any && !inFrac) return false;
    out = total * 60 + part + frac;
    if (fields == 0) out = part + frac;
    return true;
}

bool parseRanges(const std::wstring& in, int max, std::vector<int>& out) {
    out.clear();
    std::wstring s = trim(in);
    if (s.empty()) return false;
    std::vector<bool> on(max + 1, false);
    size_t i = 0;
    auto num = [&](int& v) {
        while (i < s.size() && s[i] == L' ') i++;
        if (i >= s.size() || s[i] < L'0' || s[i] > L'9') return false;
        long long x = 0;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
            x = x * 10 + (s[i++] - L'0');
            if (x > 1000000000) return false;
        }
        while (i < s.size() && s[i] == L' ') i++;
        v = (int)x;
        return true;
    };
    while (i < s.size()) {
        int a, b;
        if (!num(a)) return false;
        b = a;
        if (i < s.size() && s[i] == L'-') {
            i++;
            if (i < s.size() && (s[i] == L',' || s[i] == L' ')) b = max;
            else if (i >= s.size()) b = max;
            else if (!num(b)) return false;
        }
        if (a > b) std::swap(a, b);
        for (int k = std::max(a, 1); k <= std::min(b, max); k++) on[k] = true;
        if (i < s.size()) {
            if (s[i] != L',' && s[i] != L';') return false;
            i++;
        }
    }
    for (int k = 1; k <= max; k++)
        if (on[k]) out.push_back(k);
    return !out.empty();
}

double parseRate(const std::string& s) {
    if (s.empty()) return 0;
    size_t slash = s.find('/');
    char* e = nullptr;
    if (slash == std::string::npos) {
        double v = strtod(s.c_str(), &e);
        return std::isfinite(v) ? v : 0;
    }
    double a = strtod(s.substr(0, slash).c_str(), &e);
    double b = strtod(s.substr(slash + 1).c_str(), &e);
    if (b == 0 || !std::isfinite(a) || !std::isfinite(b)) return 0;
    return a / b;
}

std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') {
            bs++;
            i++;
        }
        if (i == a.size()) {
            r.append(bs * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            r.append(bs * 2 + 1, L'\\');
            r += L'"';
        } else {
            r.append(bs, L'\\');
            r += a[i];
        }
    }
    r += L'"';
    return r;
}

std::wstring joinArgs(const std::vector<std::wstring>& args) {
    std::wstring r;
    for (auto& a : args) {
        if (!r.empty()) r += L' ';
        r += quoteArg(a);
    }
    return r;
}

std::wstring exePath() {
    std::wstring b(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, b.data(), (DWORD)b.size());
    b.resize(n);
    return b;
}

std::wstring dirName(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"" : p.substr(0, k);
}
std::wstring fileName(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}
std::wstring stem(const std::wstring& p) {
    std::wstring f = fileName(p);
    size_t k = f.find_last_of(L'.');
    return (k == std::wstring::npos || k == 0) ? f : f.substr(0, k);
}
std::wstring extension(const std::wstring& p) {
    std::wstring f = fileName(p);
    size_t k = f.find_last_of(L'.');
    return (k == std::wstring::npos || k == 0) ? L"" : lower(f.substr(k));
}
std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

// Long paths: \\?\ prefix for absolute drive or UNC paths beyond MAX_PATH.
static std::wstring lp(const std::wstring& p) {
    if (p.size() < 240 || startsWith(p, L"\\\\?\\")) return p;
    if (p.size() > 2 && p[1] == L':') return L"\\\\?\\" + replaceAll(p, L"/", L"\\");
    if (startsWith(p, L"\\\\")) return L"\\\\?\\UNC\\" + p.substr(2);
    return p;
}

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(lp(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(lp(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool makeDirs(const std::wstring& p) {
    if (p.empty() || dirExists(p)) return true;
    std::wstring parent = dirName(p);
    if (!parent.empty() && parent != p && !(parent.size() == 2 && parent[1] == L':')) makeDirs(parent);
    return CreateDirectoryW(lp(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
uint64_t fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(lp(p).c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
uint64_t freeSpace(const std::wstring& dir) {
    ULARGE_INTEGER avail;
    std::wstring d = dir;
    while (!d.empty() && !dirExists(d)) d = dirName(d);
    if (d.empty()) return UINT64_MAX;
    if (!GetDiskFreeSpaceExW(lp(d).c_str(), &avail, nullptr, nullptr)) return UINT64_MAX;
    return avail.QuadPart;
}

bool readFile(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(lp(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    size_t off = 0;
    bool ok = true;
    while (off < out.size()) {
        DWORD want = (DWORD)std::min<size_t>(out.size() - off, 1 << 24);
        if (!ReadFile(h, out.data() + off, want, &got, nullptr) || got == 0) {
            ok = false;
            break;
        }
        off += got;
    }
    CloseHandle(h);
    out.resize(off);
    return ok;
}

bool writeFileAtomic(const std::wstring& p, const std::string& data) {
    std::wstring tmp = p + L".writing";
    HANDLE h = CreateFileW(lp(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size());
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(lp(tmp).c_str());
        return false;
    }
    return moveReplace(tmp, p);
}

bool moveReplace(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(lp(from).c_str(), lp(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
}

bool removeTree(const std::wstring& p) {
    if (fileExists(p)) return DeleteFileW(lp(p).c_str());
    DWORD attr = GetFileAttributesW(lp(p).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(lp(p).c_str());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(joinPath(p, L"*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring c = joinPath(p, n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                removeTree(c);
            else {
                SetFileAttributesW(lp(c).c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(lp(c).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(lp(p).c_str());
}

std::wstring uniquePath(const std::wstring& desired) {
    if (!fileExists(desired) && !dirExists(desired) && !fileExists(desired + L".tmp")) return desired;
    std::wstring dir = dirName(desired), st = stem(desired), ext;
    std::wstring f = fileName(desired);
    size_t k = f.find_last_of(L'.');
    if (k != std::wstring::npos && k > 0) ext = f.substr(k);
    for (int i = 1; i < 100000; i++) {
        std::wstring c = joinPath(dir, st + L" (" + std::to_wstring(i) + L")" + ext);
        if (!fileExists(c) && !dirExists(c) && !fileExists(c + L".tmp")) return c;
    }
    return desired + L"." + std::to_wstring(GetTickCount64());
}

std::wstring sanitizeFileName(const std::wstring& name) {
    std::wstring r;
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c))
            r += L'_';
        else
            r += c;
    }
    while (!r.empty() && (r.back() == L'.' || r.back() == L' ')) r.pop_back();
    if (r.empty()) r = L"output";
    if (r.size() > 150) r.resize(150);
    return r;
}

static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
std::wstring downloadsFolder() { return knownFolder(FOLDERID_Downloads); }
std::wstring localAppData() { return knownFolder(FOLDERID_LocalAppData); }

int64_t nowUnix() { return (int64_t)time(nullptr); }

std::wstring nowStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

}  // namespace ea
