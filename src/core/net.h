// WinHTTP downloads and SHA-256 (BCrypt). No third-party code.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ea {

struct HttpResult {
    bool ok = false;
    unsigned status = 0;
    std::wstring error;
};

using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

// GET into memory (body) or straight into a file (toFile, written as toFile + ".part" then renamed).
HttpResult httpGet(const std::wstring& url, std::string* body, const std::wstring& toFile = L"", ProgressFn progress = nullptr,
                   const std::atomic<bool>* cancel = nullptr, size_t maxBody = 64u << 20);

std::string sha256File(const std::wstring& path);  // lower-case hex, empty on error
std::string sha256Data(const std::string& data);

// Finds the hash for `name` in a "hash  name" / "hash *name" listing (sha256sum format).
std::string findHash(const std::string& listing, const std::string& name);

}  // namespace ea
