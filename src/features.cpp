#include "features.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
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
// CSFlipperImp has a debug "fixed time step" mode: with +0x2C9 = 0 and +0x2C8 = 1 every frame
// advances the game by 1 / [+0x2C4] seconds instead of the real frame time (see the end of the
// flipper update, RVA 0xe850fc in 2.7.1.0). A huge step rate freezes the world while the game
// keeps rendering; restoring the three fields resumes it.
namespace off_flipper {
constexpr ptrdiff_t kTargetFrameTime = 0x1C;  // float, = 1 / frame limit
constexpr ptrdiff_t kFixedRate = 0x2C4;       // float
constexpr ptrdiff_t kFixedEnable = 0x2C8;     // u8
constexpr ptrdiff_t kVariableStep = 0x2C9;    // u8, 1 in normal play
}  // namespace off_flipper

constexpr float kPausedRate = 100000.0f;

struct PauseState {
    bool paused = false;
    uintptr_t flipper = 0;
    float saved_rate = 0;
    uint8_t saved_enable = 0, saved_variable = 0;
    std::string status = "ready";
    double last_frame = 0;
} g_pause;

bool FlipperLooksRight(uintptr_t f) {
    const float frame_time = mem::Read<float>(f + off_flipper::kTargetFrameTime).value_or(0);
    const uint8_t variable = mem::Read<uint8_t>(f + off_flipper::kVariableStep).value_or(0xFF);
    return frame_time > 0.0004f && frame_time < 0.1f && variable == 1;
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
        const uintptr_t f = game::Singleton("CSFlipperImp");
        if (!f || !FlipperLooksRight(f)) {
            g_pause.status = "frame timer not found (game updated?)";
            ZZ_LOG("Pause: CSFlipperImp %p not usable", reinterpret_cast<void*>(f));
            return;
        }
        g_pause.flipper = f;
        g_pause.saved_rate = mem::Read<float>(f + off_flipper::kFixedRate).value_or(30.0f);
        g_pause.saved_enable = mem::Read<uint8_t>(f + off_flipper::kFixedEnable).value_or(0);
        g_pause.saved_variable = mem::Read<uint8_t>(f + off_flipper::kVariableStep).value_or(1);
        mem::Write<float>(f + off_flipper::kFixedRate, kPausedRate);
        mem::Write<uint8_t>(f + off_flipper::kFixedEnable, 1);
        mem::Write<uint8_t>(f + off_flipper::kVariableStep, 0);
        g_pause.paused = true;
        g_pause.status = "paused";
        ZZ_LOG("Pause on (saved rate %.1f, enable %u, variable %u)", g_pause.saved_rate,
               g_pause.saved_enable, g_pause.saved_variable);
    } else {
        const uintptr_t f = g_pause.flipper;
        mem::Write<float>(f + off_flipper::kFixedRate, g_pause.saved_rate);
        mem::Write<uint8_t>(f + off_flipper::kFixedEnable, g_pause.saved_enable);
        mem::Write<uint8_t>(f + off_flipper::kVariableStep, g_pause.saved_variable);
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

void AfterFrame() {
    if (!g_pause.paused) return;
    // The paused flipper doesn't limit the frame rate any more: hold it near 60 FPS.
    const double now = Now();
    const double wait = (1.0 / 60.0) - (now - g_pause.last_frame);
    if (wait > 0 && wait < 0.05) Sleep(static_cast<DWORD>(wait * 1000.0));
    g_pause.last_frame = Now();
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
