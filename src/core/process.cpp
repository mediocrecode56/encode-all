#include "process.h"

#include <windows.h>

#include <map>

#include "util.h"

namespace ea {

using NtProcFn = LONG(NTAPI*)(HANDLE);

static NtProcFn ntFn(const char* name) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt ? (NtProcFn)(void*)GetProcAddress(nt, name) : nullptr;
}

static std::wstring buildEnv(const ProcOpts& o) {
    // Case-insensitive map of the current environment, then overrides.
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;
    wchar_t* block = GetEnvironmentStringsW();
    for (wchar_t* p = block; p && *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        size_t eq = kv.find(L'=', 1);
        if (eq == std::wstring::npos) continue;
        std::wstring k = kv.substr(0, eq);
        vars[lower(k)] = {k, kv.substr(eq + 1)};
    }
    if (block) FreeEnvironmentStringsW(block);
    for (auto& kv : o.env) vars[lower(kv.first)] = kv;
    if (!o.pathPrepend.empty()) {
        std::wstring pre;
        for (auto& d : o.pathPrepend) pre += d + L";";
        auto& path = vars[L"path"];
        if (path.first.empty()) path.first = L"Path";
        path.second = pre + path.second;
    }
    std::wstring out;
    for (auto& kv : vars) {
        out += kv.second.first + L"=" + kv.second.second;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

Process::~Process() {
    if (proc_) {
        kill();
        wait();
    }
    if (tOut_.joinable()) tOut_.join();
    if (tErr_.joinable()) tErr_.join();
    if (proc_) CloseHandle(proc_);
    if (job_) CloseHandle(job_);
}

bool Process::start(const std::wstring& exe, const std::vector<std::wstring>& args, const ProcOpts& opts, LineFn onLine,
                    std::wstring* err) {
    onLine_ = std::move(onLine);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE outR, outW, errR, errW;
    if (!CreatePipe(&outR, &outW, &sa, 1 << 16)) return false;
    if (!CreatePipe(&errR, &errW, &sa, 1 << 16)) {
        CloseHandle(outR);
        CloseHandle(outW);
        return false;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // Only the three std handles are inherited.
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    HANDLE inherit[3] = {nul, outW, errW};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoteArg(exe);
    for (auto& a : args) cmd += L" " + quoteArg(a);
    std::wstring env = buildEnv(opts);
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    if (opts.lowPriority) flags |= BELOW_NORMAL_PRIORITY_CLASS;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, env.data(),
                             opts.cwd.empty() ? nullptr : opts.cwd.c_str(), &si.StartupInfo, &pi);
    DWORD le = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(outW);
    CloseHandle(errW);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(errR);
        if (err) {
            wchar_t* msg = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, le, 0,
                           (LPWSTR)&msg, 0, nullptr);
            *err = L"Could not start " + fileName(exe) + L": " + (msg ? trim(std::wstring(msg)) : std::to_wstring(le));
            LocalFree(msg);
        }
        return false;
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
    AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    tOut_ = std::thread(&Process::reader, this, (void*)outR, false);
    tErr_ = std::thread(&Process::reader, this, (void*)errR, true);
    return true;
}

void Process::reader(void* pipe, bool isErr) {
    HANDLE h = (HANDLE)pipe;
    char buf[8192];
    std::string line;
    DWORD got;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) {
        if (!isErr && rawOut_) {
            rawOut_->append(buf, got);
            continue;
        }
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!line.empty() && onLine_) onLine_(line, isErr);
                line.clear();
            } else if (line.size() < (1 << 20)) {
                line += c;
            }
        }
    }
    if (!line.empty() && onLine_) onLine_(line, isErr);
    CloseHandle(h);
}

int Process::wait() {
    std::lock_guard<std::mutex> lk(mu_);
    if (waited_) return code_;
    if (!proc_) return -1;
    WaitForSingleObject(proc_, INFINITE);
    if (tOut_.joinable()) tOut_.join();
    if (tErr_.joinable()) tErr_.join();
    DWORD c = 0;
    GetExitCodeProcess(proc_, &c);
    code_ = (int)c;
    waited_ = true;
    return code_;
}

bool Process::waitFor(unsigned ms) { return !proc_ || WaitForSingleObject(proc_, ms) == WAIT_OBJECT_0; }

bool Process::running() const { return proc_ && WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT; }

void Process::kill() {
    if (suspended_) resume();
    if (job_) TerminateJobObject(job_, 1);
    else if (proc_) TerminateProcess(proc_, 1);
}

static bool forEachJobProcess(HANDLE job, NtProcFn fn) {
    if (!job || !fn) return false;
    struct {
        JOBOBJECT_BASIC_PROCESS_ID_LIST h;
        ULONG_PTR more[255];
    } list{};
    if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, &list, sizeof list, nullptr)) return false;
    bool any = false;
    for (DWORD i = 0; i < list.h.NumberOfProcessIdsInList; i++) {
        HANDLE p = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, (DWORD)list.h.ProcessIdList[i]);
        if (!p) continue;
        any = fn(p) >= 0 || any;
        CloseHandle(p);
    }
    return any;
}

bool Process::suspend() {
    if (!running() || suspended_) return false;
    if (forEachJobProcess(job_, ntFn("NtSuspendProcess"))) suspended_ = true;
    return suspended_;
}

bool Process::resume() {
    if (!suspended_) return false;
    forEachJobProcess(job_, ntFn("NtResumeProcess"));
    suspended_ = false;
    return true;
}

Capture runCapture(const std::wstring& exe, const std::vector<std::wstring>& args, const ProcOpts& opts, unsigned timeoutMs,
                   const std::atomic<bool>* cancel) {
    Capture c;
    Process p;
    std::mutex m;
    p.captureStdout(&c.out);
    if (!p.start(exe, args, opts,
                 [&](const std::string& l, bool) {
                     std::lock_guard<std::mutex> lk(m);
                     if (c.err.size() < (1 << 20)) c.err += l + "\n";
                 },
                 &c.startError))
        return c;
    c.started = true;
    ULONGLONG t0 = GetTickCount64();
    while (!p.waitFor(50)) {
        if ((cancel && *cancel) || GetTickCount64() - t0 > timeoutMs) {
            c.timedOut = !(cancel && *cancel);
            p.kill();
            break;
        }
    }
    c.code = p.wait();
    return c;
}

}  // namespace ea
