#include "fps.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#include "config.h"
#include "i18n.h"
#include "log.h"
#include "memory.h"

namespace zz::fps {

namespace {

uintptr_t g_frame_time_imm = 0;  // float immediate: mov dword ptr [rbx+1Ch], 3C888889h (1/60)
uintptr_t g_refresh_imm = 0;     // int immediate: mov dword ptr [rbp-11h], 3Ch (60 Hz)
bool g_other_mod = false;
int g_monitor_hz = 60;
std::string g_status = "not initialized";
int g_status_fps = -1, g_status_hz = 0;  // set when the limit is active (text built in Status())
std::mutex g_mutex;

int ComputeMonitorMaxHz() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    int best = 0;
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
        // Same resolution as the desktop when known, otherwise any mode.
        if (cur.dmPelsWidth && (mode.dmPelsWidth != cur.dmPelsWidth ||
                                mode.dmPelsHeight != cur.dmPelsHeight))
            continue;
        best = std::max<int>(best, static_cast<int>(mode.dmDisplayFrequency));
    }
    if (best < 30) best = std::max<int>(60, static_cast<int>(cur.dmDisplayFrequency));
    return best;
}

int TargetFps() {
    const auto& cfg = config::Get();
    if (!cfg.fps_unlock) return 60;
    return cfg.fps_limit > 0 ? std::clamp(cfg.fps_limit, 20, 1000) : g_monitor_hz;
}

int TargetRefresh() {
    const auto& cfg = config::Get();
    if (!cfg.fps_unlock) return 60;
    return cfg.fullscreen_hz > 0 ? cfg.fullscreen_hz : g_monitor_hz;
}

void ApplyLocked();

// Frame counter
std::atomic<float> g_fps{0};
std::atomic<float> g_frame_ms{0};

}  // namespace

void EarlyInit() {
    std::lock_guard lock(g_mutex);
    g_monitor_hz = ComputeMonitorMaxHz();
    if (GetModuleHandleW(L"UnlockTheFps.dll")) {
        g_other_mod = true;
        g_status = "UnlockTheFps.dll is loaded and handles the frame rate (remove it from the "
                   "ME3 profile to use this one)";
        ZZ_LOG("FPS: %s", g_status.c_str());
        return;
    }
    const auto module = mem::MainModule();
    if (uintptr_t hit = mem::FindPattern(
            module, "C7 ?? ?? 89 88 88 3C EB ?? 89 ?? 18 EB ?? 89 ?? 18 C7"))
        g_frame_time_imm = hit + 3;
    if (uintptr_t hit = mem::FindPattern(
            module, "48 8B 83 ?? ?? 00 00 48 89 45 ?? EB ?? C7 45 ?? 3C 00 00 00"))
        g_refresh_imm = hit + 0x10;
    else if (uintptr_t hit2 = mem::FindPattern(module, "C7 45 ?? 3C 00 00 00 C7 45 ?? 01 00 00 00"))
        g_refresh_imm = hit2 + 3;
    ZZ_LOG("FPS: monitor up to %d Hz, frame limiter %s (RVA 0x%llx), fullscreen 60 Hz %s "
           "(RVA 0x%llx)",
           g_monitor_hz, g_frame_time_imm ? "found" : "NOT FOUND",
           static_cast<unsigned long long>(g_frame_time_imm ? g_frame_time_imm - module.base : 0),
           g_refresh_imm ? "found" : "NOT FOUND",
           static_cast<unsigned long long>(g_refresh_imm ? g_refresh_imm - module.base : 0));
    ApplyLocked();
}

void Apply() {
    std::lock_guard lock(g_mutex);
    ApplyLocked();
}

namespace {
void ApplyLocked() {
    if (g_other_mod) return;
    const int fps = TargetFps();
    const int hz = TargetRefresh();
    bool ok = true;
    if (g_frame_time_imm) {
        const float frame_time = 1.0f / static_cast<float>(fps);
        float current;
        std::memcpy(&current, reinterpret_cast<const void*>(g_frame_time_imm), sizeof(current));
        if (current != frame_time) {
            ok &= mem::PatchCode(g_frame_time_imm, &frame_time, sizeof(frame_time));
            ZZ_LOG("FPS: frame limit %d", fps);
        }
    } else {
        ok = false;
    }
    if (g_refresh_imm) {
        const int32_t value = hz;
        int32_t current;
        std::memcpy(&current, reinterpret_cast<const void*>(g_refresh_imm), sizeof(current));
        if (current != value) {
            mem::PatchCode(g_refresh_imm, &value, sizeof(value));
            ZZ_LOG("FPS: exclusive fullscreen asks for %d Hz", hz);
        }
    }
    g_status_fps = -1;
    if (!ok) {
        g_status = "frame limiter not found (game updated?)";
    } else if (!config::Get().fps_unlock) {
        g_status = "off (game default 60 FPS)";
    } else {
        g_status.clear();
        g_status_fps = fps;
        g_status_hz = hz;
    }
}
}  // namespace

void OnPresent() {
    using clock = std::chrono::steady_clock;
    static clock::time_point window_start = clock::now();
    static clock::time_point last = clock::now();
    static int frames = 0;
    const auto now = clock::now();
    g_frame_ms = std::chrono::duration<float, std::milli>(now - last).count();
    last = now;
    ++frames;
    const float elapsed = std::chrono::duration<float>(now - window_start).count();
    if (elapsed >= 0.5f) {
        g_fps = frames / elapsed;
        frames = 0;
        window_start = now;
    }
}

float CurrentFps() { return g_fps; }
float FrameTimeMs() { return g_frame_ms; }
int MonitorMaxHz() { return g_monitor_hz; }
bool OtherModActive() { return g_other_mod; }

std::string Status() {
    std::lock_guard lock(g_mutex);
    if (g_status_fps < 0) return i18n::T(g_status.c_str());
    char buf[160];
    std::snprintf(buf, sizeof(buf), i18n::T("limit %d FPS, fullscreen %d Hz"), g_status_fps,
                  g_status_hz);
    return buf;
}

bool Active() {
    std::lock_guard lock(g_mutex);
    return g_status_fps >= 0;
}

}  // namespace zz::fps
