// zzostoom Quality of Life - Elden Ring quality-of-life mod loaded by Mod Engine 3 as a native DLL.
#include <windows.h>

#include <string>

#include "config.h"
#include "features.h"
#include "fps.h"
#include "game.h"
#include "i18n.h"
#include "log.h"
#include "memory.h"
#include "overlay.h"
#include "pad.h"

namespace {

HMODULE g_module = nullptr;

std::wstring DllDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right - rc.left < 320) return TRUE;
    *reinterpret_cast<HWND*>(param) = hwnd;
    return FALSE;
}

HWND FindGameWindow() {
    HWND found = nullptr;
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

DWORD WINAPI MainThread(LPVOID) {
    const std::wstring dir = DllDirectory();
    zz::log::Init(dir);
    zz::mem::Init();
    ZZ_LOG("zzostoom Quality of Life starting");
    zz::config::Load(dir);
    {
        auto& lang = zz::config::Get().language;
        const int code = zz::i18n::FromCode(lang.c_str());
        zz::i18n::SetLanguage(code >= 0 ? code : zz::i18n::SystemLanguage());
        if (code < 0) {  // first start: remember the language picked from Windows
            lang = zz::i18n::Info(zz::i18n::Language()).code;
            zz::config::Save();
        }
        ZZ_LOG("Menu language: %s", zz::i18n::Info(zz::i18n::Language()).code);
    }

    // Before the game creates its swap chain / reads the pad for the first time.
    zz::fps::EarlyInit();
    zz::pad::Install();
    zz::features::StartBackground();

    HWND window = nullptr;
    for (int i = 0; i < 6000 && !(window = FindGameWindow()); ++i) Sleep(100);
    if (!window) {
        ZZ_LOG("Game window not found, giving up");
        return 0;
    }
    const ULONGLONG window_time = GetTickCount64();
    Sleep(1500);
    zz::game::Init();

    // Other overlays (zzostoom cheat sheet, the minimap) hook the same DXGI functions a moment
    // after the window appears; installing ours a few seconds later keeps the hooks from racing.
    const ULONGLONG elapsed = GetTickCount64() - window_time;
    if (elapsed < 5000) Sleep(static_cast<DWORD>(5000 - elapsed));
    if (!zz::overlay::Install()) ZZ_LOG("Overlay could not be installed");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        if (HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
