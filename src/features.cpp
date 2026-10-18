#include "features.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include "config.h"
#include "fps.h"
#include "game.h"
#include "log.h"
#include "memory.h"
#include "overlay.h"
#include "pad.h"
#include "params.h"

namespace zz::features {

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- Pause ---------------------------------------------------------------------------------
// The game has its own pause, used while a tutorial message is on screen. One conditional jump
// decides whether it is in effect ("cmp byte [rbx+128h], 0; je ..."); turning the je into jne
// pauses the world (enemies, timers, everything) while the game keeps rendering and the camera
// still moves. Same method as techiew's PauseTheGame (MIT, github.com/techiew/EldenRingMods).
struct PauseState {
    bool paused = false;
    bool searched = false;
    uintptr_t branch = 0;  // the 0F 84 (je) instruction
    std::string status = "ready";
} g_pause;

uintptr_t PauseBranch() {
    if (!g_pause.searched) {
        g_pause.searched = true;
        g_pause.branch = mem::FindPattern(
            mem::MainModule(),
            "0F 84 ?? ?? ?? ?? C6 ?? ?? ?? ?? ?? 00 ?? 8D ?? ?? ?? ?? ?? ?? 89 ?? ?? 89 ?? ?? ?? 8B "
            "?? ?? ?? ?? ?? ?? 85 ?? 75");
        ZZ_LOG("Pause: game pause branch %s", g_pause.branch ? "found" : "NOT FOUND");
    }
    return g_pause.branch;
}

// Flips the jump's second byte between 84 (je, normal play) and 85 (jne, paused).
bool SetBranch(bool paused) {
    const uintptr_t at = PauseBranch();
    if (!at) return false;
    const uint8_t expected = paused ? 0x84 : 0x85;
    const uint8_t want = paused ? 0x85 : 0x84;
    const uint8_t now = mem::Read<uint8_t>(at + 1).value_or(0);
    if (now == want) return true;
    if (now != expected) return false;  // not the code we know: leave it alone
    return mem::PatchCode(at + 1, &want, 1);
}

void SetPaused(bool on) {
    if (on == g_pause.paused) return;
    if (on) {
        if (!game::InGame()) {
            g_pause.status = "not in game";
            ZZ_LOG("Pause refused: not in game");
            return;
        }
        if (const int players = game::PlayerCount(); players > 1) {
            g_pause.status = "not available while other players are in your world";
            ZZ_LOG("Pause refused: %d players in the world", players);
            return;
        }
        if (!SetBranch(true)) {
            g_pause.status = "the game's pause code was not found (game updated?)";
            return;
        }
        g_pause.paused = true;
        g_pause.status = "paused";
        ZZ_LOG("Pause on");
    } else {
        SetBranch(false);
        g_pause.paused = false;
        g_pause.status = "ready";
        ZZ_LOG("Pause off");
    }
}

// ---- Background mute -------------------------------------------------------------------------

std::atomic<bool> g_muted{false};
std::string g_audio_status = "waiting";
std::mutex g_audio_mutex;

template <typename T>
void Release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Mutes/unmutes every audio session of this process on every active output device.
// Returns the number of sessions touched.
int SetProcessMute(bool mute) {
    IMMDeviceEnumerator* devices = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices))))
        return -1;
    int touched = 0;
    IMMDeviceCollection* list = nullptr;
    if (SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
        UINT count = 0;
        list->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            IAudioSessionManager2* mgr = nullptr;
            IAudioSessionEnumerator* sessions = nullptr;
            if (SUCCEEDED(list->Item(i, &dev)) &&
                SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&mgr))) &&
                SUCCEEDED(mgr->GetSessionEnumerator(&sessions))) {
                int n = 0;
                sessions->GetCount(&n);
                for (int k = 0; k < n; ++k) {
                    IAudioSessionControl* ctl = nullptr;
                    IAudioSessionControl2* ctl2 = nullptr;
                    ISimpleAudioVolume* vol = nullptr;
                    DWORD pid = 0;
                    if (SUCCEEDED(sessions->GetSession(k, &ctl)) &&
                        SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                      reinterpret_cast<void**>(&ctl2))) &&
                        SUCCEEDED(ctl2->GetProcessId(&pid)) && pid == GetCurrentProcessId() &&
                        SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                      reinterpret_cast<void**>(&vol)))) {
                        BOOL now = FALSE;
                        vol->GetMute(&now);
                        if ((now != FALSE) != mute) vol->SetMute(mute, nullptr);
                        ++touched;
                    }
                    Release(vol);
                    Release(ctl2);
                    Release(ctl);
                }
            }
            Release(sessions);
            Release(mgr);
            Release(dev);
        }
        Release(list);
    }
    Release(devices);
    return touched;
}

DWORD WINAPI BackgroundThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool applied = false;
    // Windows remembers a session's mute per executable, so a game closed while muted starts
    // muted. Keep unmuting until the game's audio session exists.
    bool synced = false;
    int ticks = 0;
    for (;;) {
        Sleep(250);
        const bool window = overlay::GameWindow() != nullptr;
        const bool want = window && config::Get().mute_background && !overlay::GameFocused();
        // Re-assert every few seconds while muted (the game may recreate its audio session).
        if (want != applied || !synced || (want && ++ticks % 12 == 0)) {
            const int n = SetProcessMute(want);
            if (n > 0) synced = true;
            if (want != applied)
                ZZ_LOG("Audio %s (%d session%s)", want ? "muted (game in background)" : "unmuted",
                       n, n == 1 ? "" : "s");
            applied = want;
            g_muted = want && n > 0;
            std::lock_guard lock(g_audio_mutex);
            g_audio_status = n < 0   ? "audio devices not available"
                             : n == 0 ? "no audio session of the game found yet"
                             : want   ? "muted (game in background)"
                                      : "playing";
        }
    }
    return 0;
}

// ---- Per-frame -----------------------------------------------------------------------------

struct FrameState {
    double last_ctx = 0;
    bool in_game = false;
    bool riding = false;
    float last_fps_limit = -1;
    int last_hz = -1;
    bool last_unlock = false;
} g_frame;

}  // namespace

void Tick(float, bool) {
    const double now = Now();
    const auto& cfg = config::Get();

    // Game context for the controller filter, 10 times per second.
    if (now - g_frame.last_ctx > 0.1) {
        g_frame.last_ctx = now;
        g_frame.in_game = game::InGame();
        g_frame.riding = g_frame.in_game && game::Riding();
        pad::SetRollAllowed(g_frame.in_game && !g_frame.riding && !g_pause.paused);
        if (g_pause.paused && (!g_frame.in_game || game::PlayerCount() > 1)) {
            SetPaused(false);
            g_pause.status = "resumed (loading / another player joined)";
        }
    }

    params::ApplyCamera();

    if (cfg.fps_limit != g_frame.last_fps_limit || cfg.fullscreen_hz != g_frame.last_hz ||
        cfg.fps_unlock != g_frame.last_unlock) {
        g_frame.last_fps_limit = static_cast<float>(cfg.fps_limit);
        g_frame.last_hz = cfg.fullscreen_hz;
        g_frame.last_unlock = cfg.fps_unlock;
        fps::Apply();
    }
}

void StartBackground() {
    if (HANDLE t = CreateThread(nullptr, 0, BackgroundThread, nullptr, 0, nullptr)) CloseHandle(t);
}

void TogglePause() { SetPaused(!g_pause.paused); }
bool Paused() { return g_pause.paused; }
std::string PauseStatus() { return g_pause.status; }

bool AudioMuted() { return g_muted; }
std::string AudioStatus() {
    std::lock_guard lock(g_audio_mutex);
    return g_audio_status;
}

void SetGameCursorVisible(bool visible) {
    // CSMenuManImp +0xAC bit 0: the game's own "show mouse cursor" flag.
    const uintptr_t menu = game::Singleton("CSMenuManImp");
    if (!menu) return;
    const auto v = mem::Read<uint8_t>(menu + 0xAC);
    if (!v) return;
    const uint8_t next = visible ? (*v | 1) : (*v & ~1);
    if (next != *v) mem::Write<uint8_t>(menu + 0xAC, next);
}

}  // namespace zz::features
