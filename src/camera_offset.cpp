#include "camera_offset.h"

#include <windows.h>

#include <cmath>
#include <cstdint>

#include "MinHook.h"
#include "config.h"
#include "log.h"
#include "memory.h"

namespace zz::camera_offset {

namespace {

// CS::CSCam vtable slot 3 (shared by CSPersCam, ChrCam, ChrExFollowCam): copies the camera into a
// plain camera data block: matrix rows right, up, forward, position at dst +0x00 / +0x10 / +0x20 /
// +0x30 (the camera itself keeps them at +0x10..+0x40), then FOV, aspect... Every frame the
// game's CSPersCam does this for the renderer (checked in 2.7.2.0); in that copy the position is
// moved along the right vector. The camera itself is not touched, so nothing adds up and aiming /
// lock-on are unchanged.
constexpr int kCopySlot = 3;
constexpr ptrdiff_t kSelfPosition = 0x40;
constexpr ptrdiff_t kDstRight = 0x00;
constexpr ptrdiff_t kDstPosition = 0x30;

using Fn = void*(__fastcall*)(void* self, void* dst, void* a3, void* a4);
Fn g_copy = nullptr;
uintptr_t g_pers_vtable = 0;  // CS::CSPersCam
bool g_installed = false;

void* __fastcall HookedCopy(void* self, void* dst, void* a3, void* a4) {
    void* result = g_copy(self, dst, a3, a4);
    const auto& cfg = config::Get();
    if (!cfg.camera || cfg.cam_side == 0.0f || !dst) return result;
    const uintptr_t cam = reinterpret_cast<uintptr_t>(self);
    if (mem::ReadPtr(cam) != g_pers_vtable) return result;
    const uintptr_t out = reinterpret_cast<uintptr_t>(dst);
    float right[3], pos[3], own[3];
    if (!mem::ReadRaw(out + kDstRight, right, sizeof(right)) ||
        !mem::ReadRaw(out + kDstPosition, pos, sizeof(pos)) ||
        !mem::ReadRaw(cam + kSelfPosition, own, sizeof(own)))
        return result;
    // Only when the copy really has the layout above: its position is the camera's own.
    for (int i = 0; i < 3; ++i)
        if (std::fabs(pos[i] - own[i]) > 0.001f) return result;
    const float len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (len < 0.5f || len > 2.0f) return result;  // not a rotation row
    for (int i = 0; i < 3; ++i) pos[i] += right[i] / len * cfg.cam_side;
    mem::WriteRaw(out + kDstPosition, pos, sizeof(pos));
    return result;
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    const auto module = mem::MainModule();
    g_pers_vtable = mem::FindVtable("CSPersCam@CS");
    const uintptr_t copy = g_pers_vtable ? mem::ReadPtr(g_pers_vtable + kCopySlot * 8) : 0;
    if (!module.Contains(copy)) {
        ZZ_LOG("Camera offset: CSPersCam not found");
        return false;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return false;
    s = MH_CreateHook(reinterpret_cast<void*>(copy), reinterpret_cast<void*>(HookedCopy),
                      reinterpret_cast<void**>(&g_copy));
    if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(copy));
    if (s != MH_OK) {
        ZZ_LOG("Camera offset: hook failed (%d)", static_cast<int>(s));
        return false;
    }
    g_installed = true;
    ZZ_LOG("Camera offset: hooked CSCam copy (RVA 0x%llx)",
           static_cast<unsigned long long>(copy - module.base));
    return true;
}

bool Installed() { return g_installed; }

}  // namespace zz::camera_offset
