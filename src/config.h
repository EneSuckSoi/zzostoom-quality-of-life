#pragma once
#include <string>

// zzostoom_qol.ini next to the DLL. Every setting can also be changed from the in-game menu.
namespace zz::config {

// Pseudo key code: the key under Esc (left of 1), matched by its scan code so it works on every
// keyboard layout (" on Turkish Q, ` on US, ^ on German...).
constexpr int kKeyUnderEsc = 0x1000;

struct Config {
    // Menu
    std::string language;  // i18n code ("en", "tr"...); empty = the Windows display language
    std::string menu_key = "HOME";
    std::string menu_pad = "HOLD VIEW";  // controller shortcut, see pad::MenuShortcutNames()
    int menu_hold_ms = 500;              // how long View is held for "HOLD VIEW"
    float ui_scale = 0.0f;         // 0 = automatic (based on screen height)
    bool block_game_input = true;  // block keyboard/mouse game input while the menu is open
    bool show_hint = true;         // short "menu: HOME" hint after the game starts

    // Camera (applied to every row of the game's LockCamParam, in memory only)
    bool camera = true;
    float cam_distance = 1.0f;  // x the game's distance behind the character
    float cam_fov = 0.0f;       // degrees added to the game's vertical field of view
    float cam_height = 0.0f;    // metres added to the point the camera looks at
    float cam_side = 0.0f;      // metres the camera is moved to the right (< 0: left)
    float lock_range = 1.0f;    // x the distance at which lock-on can be acquired / kept
    float lock_tilt = 1.0f;     // x how much the camera tilts toward a locked target

    // Frame rate
    bool fps_unlock = true;
    int fps_limit = 0;          // 0 = the monitor's refresh rate
    int fullscreen_hz = 0;      // exclusive fullscreen refresh rate, 0 = the monitor's highest
    bool fps_counter = false;

    // Combat HUD
    bool hud_enemy_stagger = true;   // stance bars above enemies
    bool hud_enemy_hp = true;        // HP numbers next to them
    bool hud_focus_bar = true;       // big bar for the enemy you are fighting (top of the screen)
    bool hud_player_stagger = true;  // your own poise bar (bottom of the screen)
    bool hud_buffs = true;           // remaining time of active buffs
    float hud_scale = 1.0f;
    float hud_range = 30.0f;         // metres
    float hud_bar_height = 2.1f;     // metres above an enemy's feet for its bar
    bool hud_buffs_unnamed = false;  // also list buffs without a known name
    float hud_focus_x = 50.0f;       // focus bar centre, % of the screen width / height
    float hud_focus_y = 11.5f;
    float hud_buffs_x = 2.2f;        // buff list position, % of the screen width / height
    float hud_buffs_y = 21.5f;       // (top of the list, or its bottom when it grows upwards)
    bool hud_buffs_grow_up = false;  // new rows go above the first one
    bool hud_status = true;          // enemy status build-up bars (blood loss, frostbite...)

    // Controls
    bool instant_roll = true;   // roll on press instead of on release
    std::string roll_button = "B";
    int sprint_delay_ms = 220;  // hold longer than this after a roll to start sprinting
    int sprint_min_ms = 600;    // a sprint started by the mod is held at least this long

    // Misc
    bool mute_background = true;
    bool pause_key_enabled = true;
    std::string pause_key = "PAUSE";

};

void Load(const std::wstring& dll_dir);
void Save();
Config& Get();
// Increases on every Load()/Save(): lets hot paths re-read settings only when they change.
int Version();
const std::wstring& IniPath();

const char* const* KeyNames(int* count);
int KeyFromName(const std::string& name);
// True if a WM_KEYDOWN (wparam/lparam) is `key` (a name from KeyNames()).
bool MatchesKey(const std::string& key, unsigned long long wparam, long long lparam);

}  // namespace zz::config
