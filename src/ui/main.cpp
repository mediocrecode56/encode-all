// Entry point: --version for the updater's self-test, single instance, update guard, message loop.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../app/appupdate.h"
#include "../app/settings.h"
#include "../core/util.h"
#include "../version.h"
#include "window.h"

using namespace ea;

static void attachConsoleOut(const char* text) {
    // GUI exe: write to an inherited stdout (pipes from the updater) or the parent console.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    bool relaunched = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--version") {
            attachConsoleOut(EA_VERSION "\n");
            return 0;
        }
        if (a == L"--update-now") {  // scripted update check (tests/update-test.ps1)
            AppUpdater u;
            std::atomic<bool> stop{false};
            u.checkAndStage(stop);
            UpdateState st = u.state();
            std::string msg = narrow(st.staged ? L"staged " + st.stagedVersion : st.error.empty() ? st.status : st.error) + "\n";
            attachConsoleOut(msg.c_str());
            return st.staged ? 0 : st.error.empty() ? 1 : 2;
        }
        if (a == L"--startup-check") {  // the start-up guard alone, without a window (update test)
            return AppUpdater::startupGuard(false) ? 3 : 0;
        }
        if (a == L"--relaunched") {
            relaunched = true;
            continue;
        }
        args.push_back(a);
    }
    LocalFree(argv);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // One window: a second launch hands its files/links to the first one.
    HANDLE mutex = nullptr;
    for (int attempt = 0; attempt < (relaunched ? 40 : 1); attempt++) {
        mutex = CreateMutexW(nullptr, TRUE, L"Local\\EncodeAll.SingleInstance");
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        CloseHandle(mutex);
        mutex = nullptr;
        if (relaunched) Sleep(250);  // the old process is closing
    }
    if (!mutex) {
        HWND other = FindWindowW(L"EncodeAllMain", nullptr);
        if (other) {
            std::wstring joined;
            for (auto& a : args) joined += a + L'\n';
            COPYDATASTRUCT cd{0xEA01, (DWORD)(joined.size() * sizeof(wchar_t)), (void*)joined.c_str()};
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    if (AppUpdater::startupGuard()) return 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    OleInitialize(nullptr);
    ui::MainWindow win;
    if (!win.create(inst, show, args)) {
        MessageBoxW(nullptr, L"Could not create the window.", L"Encode All", MB_ICONERROR);
        return 1;
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (win.preTranslate(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    OleUninitialize();
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
