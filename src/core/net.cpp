#include "net.h"

#include <algorithm>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <vector>

#include "../version.h"
#include "util.h"

namespace ea {

namespace {
struct H {
    HINTERNET h = nullptr;
    ~H() {
        if (h) WinHttpCloseHandle(h);
    }
};
std::wstring lastErr(const wchar_t* what) {
    return std::wstring(what) + L" failed (" + std::to_wstring(GetLastError()) + L")";
}
}  // namespace

HttpResult httpGet(const std::wstring& url, std::string* body, const std::wstring& toFile, ProgressFn progress,
                   const std::atomic<bool>* cancel, size_t maxBody) {
    HttpResult r;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        r.error = L"Bad URL";
        return r;
    }
    std::wstring ua = L"EncodeAll/" EA_VERSION_W;
    H session, conn, req;
    session.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) session.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session.h) {
        r.error = lastErr(L"WinHttpOpen");
        return r;
    }
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos)) {
        protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    conn.h = WinHttpConnect(session.h, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (!conn.h) {
        r.error = lastErr(L"Connect");
        return r;
    }
    std::wstring obj = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    req.h = WinHttpOpenRequest(conn.h, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) {
        r.error = lastErr(L"OpenRequest");
        return r;
    }
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof decomp);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = lastErr(L"Request");
        return r;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
    r.status = status;
    if (status != 200) {
        r.error = L"HTTP " + std::to_wstring(status);
        return r;
    }
    uint64_t total = 0;
    wchar_t lenBuf[32];
    DWORD lenSz = sizeof lenBuf;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenBuf, &lenSz, nullptr)) total = _wcstoui64(lenBuf, nullptr, 10);

    HANDLE f = INVALID_HANDLE_VALUE;
    std::wstring part = toFile + L".part";
    if (!toFile.empty()) {
        makeDirs(dirName(toFile));
        f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            r.error = L"Cannot write " + part;
            return r;
        }
    }
    std::vector<char> buf(1 << 16);
    uint64_t done = 0;
    bool ok = true;
    for (;;) {
        if (cancel && *cancel) {
            r.error = L"Canceled";
            ok = false;
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) {
            r.error = lastErr(L"Read");
            ok = false;
            break;
        }
        if (got == 0) break;
        done += got;
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            if (!WriteFile(f, buf.data(), got, &w, nullptr) || w != got) {
                r.error = L"Disk write failed (disk full?)";
                ok = false;
                break;
            }
        } else if (body) {
            if (body->size() + got > maxBody) {
                r.error = L"Response too large";
                ok = false;
                break;
            }
            body->append(buf.data(), got);
        }
        if (progress) progress(done, total);
    }
    if (ok && total && done != total) {
        r.error = L"Truncated download";
        ok = false;
    }
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        if (ok) ok = moveReplace(part, toFile);
        if (!ok) DeleteFileW(part.c_str());
    }
    r.ok = ok;
    return r;
}

static std::string hex(const unsigned char* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += x[d[i] >> 4];
        s += x[d[i] & 15];
    }
    return s;
}

struct Sha {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Sha() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void add(const void* d, ULONG n) { BCryptHashData(h, (PUCHAR)d, n, 0); }
    std::string done() {
        unsigned char out[32];
        if (!h || BCryptFinishHash(h, out, 32, 0) != 0) return {};
        return hex(out, 32);
    }
};

std::string sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    Sha s;
    std::vector<char> buf(1 << 20);
    DWORD got;
    bool ok = true;
    while ((ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) && got) s.add(buf.data(), got);
    CloseHandle(f);
    return ok ? s.done() : std::string();
}

std::string sha256Data(const std::string& data) {
    Sha s;
    s.add(data.data(), (ULONG)data.size());
    return s.done();
}

std::string findHash(const std::string& listing, const std::string& name) {
    for (auto& raw : split(listing, '\n')) {
        std::string l = trim(raw);
        if (l.size() < 66) continue;
        std::string h = lower(l.substr(0, 64));
        bool isHex = true;
        for (char c : h) isHex = isHex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (!isHex) continue;
        std::string rest = trim(l.substr(64));
        if (!rest.empty() && rest[0] == '*') rest = rest.substr(1);
        // Some listings carry a path ("./ffmpeg-...zip"); match the basename.
        size_t slash = rest.find_last_of("/\\");
        if (slash != std::string::npos) rest = rest.substr(slash + 1);
        if (rest == name) return h;
    }
    // A bare hash file (deno's .sha256sum may hold only the hash or "hash  file").
    std::string t = trim(listing);
    if (t.size() >= 64 && name.empty()) return lower(t.substr(0, 64));
    return {};
}

}  // namespace ea
