#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>

#include "log.h"

namespace zz::config {

namespace {

Config g_config;
std::atomic<int> g_version{0};
std::wstring g_path;

struct KeyEntry {
    const char* name;
    int vk;
};

const KeyEntry kKeys[] = {
    {"HOME", VK_HOME},       {"END", VK_END},           {"INSERT", VK_INSERT},
    {"DELETE", VK_DELETE},   {"PAGEUP", VK_PRIOR},      {"PAGEDOWN", VK_NEXT},
    {"PAUSE", VK_PAUSE},     {"SCROLLLOCK", VK_SCROLL}, {"UNDER_ESC", kKeyUnderEsc},
    {"F1", VK_F1},           {"F2", VK_F2},             {"F3", VK_F3},
    {"F4", VK_F4},           {"F5", VK_F5},             {"F6", VK_F6},
    {"F7", VK_F7},           {"F8", VK_F8},             {"F9", VK_F9},
    {"F10", VK_F10},         {"F11", VK_F11},           {"F12", VK_F12},
    {"NUMPAD0", VK_NUMPAD0}, {"NONE", 0},
};

enum class Type { kBool, kInt, kFloat, kString };
struct Field {
    const wchar_t* section;
    const wchar_t* key;
    Type type;
    void* ptr;
};

std::vector<Field> Fields() {
    Config& c = g_config;
    return {
        {L"menu", L"language", Type::kString, &c.language},
        {L"menu", L"menu_key", Type::kString, &c.menu_key},
        {L"menu", L"menu_pad", Type::kString, &c.menu_pad},
        {L"menu", L"menu_hold_ms", Type::kInt, &c.menu_hold_ms},
        {L"menu", L"ui_scale", Type::kFloat, &c.ui_scale},
        {L"menu", L"block_game_input", Type::kBool, &c.block_game_input},
        {L"menu", L"show_hint", Type::kBool, &c.show_hint},
        {L"camera", L"enabled", Type::kBool, &c.camera},
        {L"camera", L"distance", Type::kFloat, &c.cam_distance},
        {L"camera", L"fov_add", Type::kFloat, &c.cam_fov},
        {L"camera", L"height_add", Type::kFloat, &c.cam_height},
        {L"camera", L"side", Type::kFloat, &c.cam_side},
        {L"camera", L"lock_range", Type::kFloat, &c.lock_range},
        {L"camera", L"lock_tilt", Type::kFloat, &c.lock_tilt},
        {L"fps", L"unlock", Type::kBool, &c.fps_unlock},
        {L"fps", L"limit", Type::kInt, &c.fps_limit},
        {L"fps", L"fullscreen_hz", Type::kInt, &c.fullscreen_hz},
        {L"fps", L"counter", Type::kBool, &c.fps_counter},
        {L"hud", L"enemy_stagger", Type::kBool, &c.hud_enemy_stagger},
        {L"hud", L"enemy_hp", Type::kBool, &c.hud_enemy_hp},
        {L"hud", L"focus_bar", Type::kBool, &c.hud_focus_bar},
        {L"hud", L"player_stagger", Type::kBool, &c.hud_player_stagger},
        {L"hud", L"buffs", Type::kBool, &c.hud_buffs},
        {L"hud", L"scale", Type::kFloat, &c.hud_scale},
        {L"hud", L"range", Type::kFloat, &c.hud_range},
        {L"hud", L"bar_height", Type::kFloat, &c.hud_bar_height},
        {L"hud", L"buffs_unnamed", Type::kBool, &c.hud_buffs_unnamed},
        {L"hud", L"focus_x", Type::kFloat, &c.hud_focus_x},
        {L"hud", L"focus_y", Type::kFloat, &c.hud_focus_y},
        {L"hud", L"buffs_x", Type::kFloat, &c.hud_buffs_x},
        {L"hud", L"buffs_y", Type::kFloat, &c.hud_buffs_y},
        {L"hud", L"buffs_grow_up", Type::kBool, &c.hud_buffs_grow_up},
        {L"hud", L"status", Type::kBool, &c.hud_status},
        {L"controls", L"instant_roll", Type::kBool, &c.instant_roll},
        {L"controls", L"roll_button", Type::kString, &c.roll_button},
        {L"controls", L"sprint_delay_ms", Type::kInt, &c.sprint_delay_ms},
        {L"controls", L"sprint_min_ms", Type::kInt, &c.sprint_min_ms},
        {L"misc", L"mute_background", Type::kBool, &c.mute_background},
        {L"misc", L"pause_key_enabled", Type::kBool, &c.pause_key_enabled},
        {L"misc", L"pause_key", Type::kString, &c.pause_key},
    };
}

std::string ToUtf8(const wchar_t* w) {
    char out[256];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& s) {
    wchar_t buf[256];
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf, 256);
    return buf;
}

}  // namespace

const char* const* KeyNames(int* count) {
    static const char* names[std::size(kKeys)];
    for (size_t i = 0; i < std::size(kKeys); ++i) names[i] = kKeys[i].name;
    *count = static_cast<int>(std::size(kKeys));
    return names;
}

int KeyFromName(const std::string& name) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    for (auto& k : kKeys)
        if (upper == k.name) return k.vk;
    return 0;
}

bool MatchesKey(const std::string& key, unsigned long long wparam, long long lparam) {
    const int vk = KeyFromName(key);
    if (!vk) return false;
    if (vk == kKeyUnderEsc) return ((lparam >> 16) & 0xFF) == 0x29 && !(lparam & (1 << 24));
    return static_cast<int>(wparam) == vk;
}

Config& Get() { return g_config; }
int Version() { return g_version; }
const std::wstring& IniPath() { return g_path; }

void Load(const std::wstring& dll_dir) {
    g_path = dll_dir + L"\\zzostoom_qol.ini";
    const bool exists = GetFileAttributesW(g_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists) {
        for (const Field& f : Fields()) {
            wchar_t buf[128];
            GetPrivateProfileStringW(f.section, f.key, L"\x01", buf, 128, g_path.c_str());
            if (buf[0] == 1) continue;  // missing: keep the default
            const std::string v = ToUtf8(buf);
            switch (f.type) {
                case Type::kBool: *static_cast<bool*>(f.ptr) = v != "0" && v != "false"; break;
                case Type::kInt: *static_cast<int*>(f.ptr) = std::atoi(v.c_str()); break;
                case Type::kFloat:
                    *static_cast<float*>(f.ptr) = static_cast<float>(std::atof(v.c_str()));
                    break;
                case Type::kString: *static_cast<std::string*>(f.ptr) = v; break;
            }
        }
    }
    auto& c = g_config;
    if (!KeyFromName(c.menu_key)) c.menu_key = "HOME";
    c.menu_hold_ms = std::clamp(c.menu_hold_ms, 200, 2000);
    c.cam_distance = std::clamp(c.cam_distance, 0.3f, 3.0f);
    c.cam_fov = std::clamp(c.cam_fov, -20.0f, 50.0f);
    c.cam_height = std::clamp(c.cam_height, -1.0f, 1.5f);
    c.cam_side = std::clamp(c.cam_side, -1.5f, 1.5f);
    c.lock_range = std::clamp(c.lock_range, 0.5f, 3.0f);
    c.lock_tilt = std::clamp(c.lock_tilt, 0.0f, 2.0f);
    c.fps_limit = std::clamp(c.fps_limit, 0, 1000);
    c.fullscreen_hz = std::clamp(c.fullscreen_hz, 0, 1000);
    c.hud_scale = std::clamp(c.hud_scale, 0.5f, 2.5f);
    c.hud_range = std::clamp(c.hud_range, 5.0f, 100.0f);
    c.hud_bar_height = std::clamp(c.hud_bar_height, 0.5f, 6.0f);
    c.hud_focus_x = std::clamp(c.hud_focus_x, 0.0f, 100.0f);
    c.hud_focus_y = std::clamp(c.hud_focus_y, 0.0f, 100.0f);
    c.hud_buffs_x = std::clamp(c.hud_buffs_x, 0.0f, 100.0f);
    c.hud_buffs_y = std::clamp(c.hud_buffs_y, 0.0f, 100.0f);
    c.sprint_delay_ms = std::clamp(c.sprint_delay_ms, 80, 1000);
    c.sprint_min_ms = std::clamp(c.sprint_min_ms, 0, 2000);
    Save();  // writes missing keys with their defaults
    ZZ_LOG("Config loaded from %s (%s)", ToUtf8(g_path.c_str()).c_str(),
           exists ? "existing" : "new");
}

void Save() {
    ++g_version;
    if (g_path.empty()) return;
    for (const Field& f : Fields()) {
        char buf[128];
        switch (f.type) {
            case Type::kBool: std::snprintf(buf, sizeof(buf), "%d", *static_cast<bool*>(f.ptr) ? 1 : 0); break;
            case Type::kInt: std::snprintf(buf, sizeof(buf), "%d", *static_cast<int*>(f.ptr)); break;
            case Type::kFloat: std::snprintf(buf, sizeof(buf), "%.3f", *static_cast<float*>(f.ptr)); break;
            case Type::kString:
                std::snprintf(buf, sizeof(buf), "%s", static_cast<std::string*>(f.ptr)->c_str());
                break;
        }
        WritePrivateProfileStringW(f.section, f.key, ToWide(buf).c_str(), g_path.c_str());
    }
}

}  // namespace zz::config
