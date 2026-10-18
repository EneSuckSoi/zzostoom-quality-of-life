#include "ui.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include "buffs.h"
#include "config.h"
#include "features.h"
#include "fps.h"
#include "game.h"
#include "hud.h"
#include "i18n.h"
#include "imgui.h"
#include "log.h"
#include "overlay.h"
#include "pad.h"
#include "params.h"

namespace zz::ui {

namespace {

using i18n::T;

constexpr const char* kModVersion = "1.1.0";

const char* const kTabs[] = {"Camera", "Combat HUD", "Controls", "Performance", "Misc", "Settings"};
constexpr int kTabCount = 6;

const ImVec4 kGold(0.92f, 0.78f, 0.42f, 1.0f);
const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kDim(0.62f, 0.60f, 0.56f, 1.0f);

float g_scale = 1.0f;
int g_current_tab = 0;
int g_tab_request = -1;
bool g_focus_request = false;

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Toast {
    std::string text;
    double expires;
};
std::mutex g_toast_mutex;
std::vector<Toast> g_toasts;
double g_start_time = 0.0;

// Used only by test/harness.cpp to screenshot each tab; inert unless the variable is set.
int TestEnvInt(const wchar_t* name) {
    wchar_t buf[16];
    DWORD n = GetEnvironmentVariableW(name, buf, 16);
    return (n > 0 && n < 16) ? _wtoi(buf) : -1;
}

// Screenshots from the test harness: no game, so status lines are left out.
bool Showcase() { return TestEnvInt(L"ZZ_UI_TEST_SHOWCASE") > 0; }

// Translated label with a language-independent ImGui ID.
std::string Id(const char* english) { return std::string(T(english)) + "###" + english; }

std::string Fmt(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

void Separator(const char* english) { ImGui::SeparatorText(T(english)); }
void Wrapped(const char* english) { ImGui::TextWrapped("%s", T(english)); }
void Dim(const char* english) { ImGui::TextDisabled("%s", T(english)); }

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("%s", T(text));
}

// Every widget saves the config when the user is done with it.
void SaveIfEdited() {
    if (ImGui::IsItemDeactivatedAfterEdit()) config::Save();
}

bool Check(const char* label, bool* v, const char* help = nullptr) {
    const bool changed = ImGui::Checkbox(Id(label).c_str(), v);
    if (changed) config::Save();
    if (help) HelpMarker(help);
    return changed;
}

void SliderF(const char* label, float* v, float min, float max, const char* fmt, float reset,
             const char* help = nullptr) {
    ImGui::SetNextItemWidth(300 * g_scale);
    ImGui::SliderFloat(Id(label).c_str(), v, min, max, T(fmt));
    SaveIfEdited();
    ImGui::SameLine();
    ImGui::PushID(label);
    if (ImGui::SmallButton(T("Reset"))) {
        *v = reset;
        config::Save();
    }
    ImGui::PopID();
    if (help) HelpMarker(help);
}

void SliderI(const char* label, int* v, int min, int max, const char* fmt, int reset,
             const char* help = nullptr) {
    ImGui::SetNextItemWidth(300 * g_scale);
    ImGui::SliderInt(Id(label).c_str(), v, min, max, T(fmt));
    SaveIfEdited();
    ImGui::SameLine();
    ImGui::PushID(label);
    if (ImGui::SmallButton(T("Reset"))) {
        *v = reset;
        config::Save();
    }
    ImGui::PopID();
    if (help) HelpMarker(help);
}

// `text` is already in the menu language.
void Status(const std::string& text, bool ok) {
    if (Showcase()) return;  // nothing to report without a game
    ImGui::TextDisabled("%s", T("Status:"));
    ImGui::SameLine();
    ImGui::TextColored(ok ? kGreen : kGold, "%s", text.c_str());
}

void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.TabRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 6);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c = style.Colors;
    const ImVec4 gold_dark(0.36f, 0.29f, 0.14f, 1.0f);
    const ImVec4 gold_mid(0.55f, 0.44f, 0.20f, 1.0f);
    const ImVec4 gold_hi(0.72f, 0.58f, 0.26f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.91f, 0.86f, 1.0f);
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.07f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.09f, 0.10f, 0.60f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.09f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.42f, 0.35f, 0.20f, 0.55f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.14f, 0.12f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.21f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.26f, 0.17f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.09f, 0.07f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.16f, 0.09f, 1.0f);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_CheckboxSelectedBg] = ImVec4(0.30f, 0.24f, 0.11f, 1.0f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0.55f, 0.44f, 0.20f, 0.35f);
    c[ImGuiCol_ResizeGripHovered] = gold_mid;
    c[ImGuiCol_ResizeGripActive] = gold_hi;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.32f, 0.28f, 0.20f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = gold_mid;
    c[ImGuiCol_ScrollbarGrabActive] = gold_hi;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.55f, 0.44f, 0.20f, 0.45f);
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_InputTextCursor] = kGold;
    c[ImGuiCol_SliderGrab] = gold_mid;
    c[ImGuiCol_SliderGrabActive] = gold_hi;
    c[ImGuiCol_Button] = gold_dark;
    c[ImGuiCol_ButtonHovered] = gold_mid;
    c[ImGuiCol_ButtonActive] = gold_hi;
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.25f, 0.13f, 0.85f);
    c[ImGuiCol_HeaderHovered] = gold_mid;
    c[ImGuiCol_HeaderActive] = gold_hi;
    c[ImGuiCol_Separator] = ImVec4(0.42f, 0.35f, 0.20f, 0.6f);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.14f, 0.10f, 1.0f);
    c[ImGuiCol_TabHovered] = gold_mid;
    c[ImGuiCol_TabSelected] = gold_dark;
    c[ImGuiCol_TabSelectedOverline] = kGold;
}

// ---- Tabs ---------------------------------------------------------------------------------

void DrawCameraTab() {
    auto& c = config::Get();
    Check("Camera adjustments", &c.camera,
          "Edits the game's camera table (LockCamParam) in memory. Nothing is saved to "
          "regulation.bin, so it works with any regulation mod and with Seamless Co-op.");
    ImGui::BeginDisabled(!c.camera);
    Separator("View");
    SliderF("Distance", &c.cam_distance, 0.5f, 2.5f, "x%.2f", 1.0f,
            "How far behind the character the camera sits (x the game's own distance, which "
            "differs per situation: on foot, on Torrent, in some boss arenas...).");
    SliderF("Field of view", &c.cam_fov, -15.0f, 40.0f, "%+.0f deg", 0.0f,
            "Added to the game's vertical field of view (48 deg on foot).");
    SliderF("Height", &c.cam_height, -0.8f, 1.2f, "%+.2f m", 0.0f,
            "Raises or lowers the point the camera looks at (1.45 m above the feet on foot).");
    Separator("Lock-on");
    SliderF("Lock-on range", &c.lock_range, 0.6f, 2.5f, "x%.2f", 1.0f,
            "Distance at which you can lock on (15 m on foot, more for some bosses).");
    SliderF("Lock-on camera tilt", &c.lock_tilt, 0.0f, 2.0f, "x%.2f", 1.0f,
            "How much the camera pitches toward a locked target. Lower values keep the camera "
            "level against tall bosses or enemies above / below you.");
    ImGui::EndDisabled();
    Status(params::CameraStatus(), params::CameraApplied());

    Separator("Also in the game's own settings");
    Wrapped("System > Camera Options has two switches for the camera fighting you:\n"
            "  - Camera Auto Wall Recovery: the camera swings away from walls by itself.\n"
            "  - Camera Auto Rotation: the camera turns while you walk left / right.\n"
            "Turn both Off to keep the camera where you put it.");
}

void DrawHudTab() {
    auto& c = config::Get();
    Separator("Stance (poise) bars");
    Check("Enemy bars", &c.hud_enemy_stagger,
          "A bar above every enemy you are fighting. It fills up as you break its stance; when "
          "it is full the enemy staggers (bosses kneel for a critical hit).");
    Check("Focus bar (top of the screen)", &c.hud_focus_bar,
          "A large bar for the enemy that was hit most recently: good for bosses.");
    ImGui::BeginDisabled(!c.hud_focus_bar);
    SliderF("Focus bar X", &c.hud_focus_x, 0.0f, 100.0f, "%.1f %%", 50.0f,
            "Position of the focus bar's centre, in percent of the screen width. Turn on Preview "
            "below to see it.");
    SliderF("Focus bar Y", &c.hud_focus_y, 0.0f, 100.0f, "%.1f %%", 11.5f,
            "Position of the focus bar, in percent of the screen height.");
    ImGui::EndDisabled();
    Check("My own bar (bottom of the screen)", &c.hud_player_stagger,
          "Your poise: when it fills up, you get staggered. Shown only while it is damaged.");
    Dim("On the enemy bars and the focus bar:");
    ImGui::BeginDisabled(!c.hud_enemy_stagger && !c.hud_focus_bar);
    Check("Enemy HP numbers", &c.hud_enemy_hp);
    Check("Status build-up bars", &c.hud_status,
          "An icon and a bar under the enemy bars while blood loss, frostbite, poison, scarlet "
          "rot, sleep, madness or death blight builds up. Only statuses that are building up are "
          "shown.");
    ImGui::EndDisabled();
    Separator("Buffs");
    Check("Buff timers", &c.hud_buffs,
          "Time left on your active buffs (Golden Vow, Flame Grant Me Strength, crystal tears, "
          "boluses...).");
    ImGui::BeginDisabled(!c.hud_buffs);
    Check("Also buffs without a known name", &c.hud_buffs_unnamed);
    SliderF("Buff list X", &c.hud_buffs_x, 0.0f, 95.0f, "%.1f %%", 2.2f,
            "Position of the buff list, in percent of the screen width. Turn on Preview below to "
            "see it.");
    SliderF("Buff list Y", &c.hud_buffs_y, 0.0f, 100.0f, "%.1f %%", 21.5f,
            "Position of the buff list, in percent of the screen height (its top, or its bottom when it "
            "grows upwards).");
    Check("Grow upwards", &c.hud_buffs_grow_up,
          "New buffs are added above the first one instead of below it, so the list can sit at "
          "the bottom of the screen.");
    ImGui::EndDisabled();
    if (c.hud_buffs) {
        const std::string status = buffs::Status();
        Status(T(status.c_str()), status.rfind("ok", 0) == 0);
    }
    Separator("Layout");
    SliderF("HUD size", &c.hud_scale, 0.6f, 2.0f, "x%.2f", 1.0f);
    SliderF("Bar height above enemies", &c.hud_bar_height, 1.0f, 4.0f, "%.1f m", 2.1f);
    SliderF("Max. distance", &c.hud_range, 10.0f, 80.0f, "%.0f m", 30.0f);
    static bool preview = false;
    ImGui::Checkbox(Id("Preview (example bars on screen)").c_str(), &preview);
    if (preview) hud::DrawPreview();
}

void DrawControlsTab() {
    auto& c = config::Get();
    Separator("Instant roll");
    Wrapped("Elden Ring rolls when you RELEASE the dodge button, because holding it means sprint. "
            "With this on, the roll starts the moment you press; keep holding to sprint afterwards.");
    Check("Roll on press", &c.instant_roll);
    ImGui::BeginDisabled(!c.instant_roll);
    int count = 0;
    const char* const* names = pad::RollButtonNames(&count);
    ImGui::SetNextItemWidth(160 * g_scale);
    if (ImGui::BeginCombo(Id("Dodge button").c_str(), c.roll_button.c_str())) {
        for (int i = 0; i < count; ++i)
            if (ImGui::Selectable(names[i], c.roll_button == names[i])) {
                c.roll_button = names[i];
                config::Save();
            }
        ImGui::EndCombo();
    }
    HelpMarker("The controller button your game uses for \"Backstep, Dodge Roll, Dash\" (B / "
               "Circle by default).");
    SliderI("Sprint after", &c.sprint_delay_ms, 120, 600, "%d ms", 220,
            "Keep the button held this long after pressing to start sprinting.");
    SliderI("Minimum sprint", &c.sprint_min_ms, 300, 1200, "%d ms", 600,
            "A sprint started by the mod is held at least this long, so that letting go is not "
            "read as a second tap (= a second roll) by the game. Raise it if you sometimes get "
            "a double roll when releasing a short sprint.");
    ImGui::EndDisabled();
    Dim("Not active on Torrent, in this menu or while L3 / R3 is held (cheat sheet shortcuts).");
    if (!Showcase())
        ImGui::TextDisabled(T("Controller: %s"), T(pad::Installed() ? "hooked" : "not available"));
}

void DrawPerformanceTab() {
    auto& c = config::Get();
    Separator("Frame rate");
    if (fps::OtherModActive()) {
        ImGui::TextColored(kGold, "%s", fps::Status().c_str());
    } else {
        Check("Unlock the frame rate", &c.fps_unlock,
              "Removes the 60 FPS limit. Works in exclusive fullscreen, borderless and windowed "
              "mode and with Seamless Co-op.");
        ImGui::BeginDisabled(!c.fps_unlock);
        const std::string fps_auto = Fmt(T("auto (%d FPS)"), fps::MonitorMaxHz());
        SliderI("FPS limit", &c.fps_limit, 0, 360, c.fps_limit == 0 ? fps_auto.c_str() : "%d FPS", 0,
                "0 = your monitor's refresh rate. Changes apply immediately.");
        if (c.fps_limit > 0 && c.fps_limit < 30) c.fps_limit = 30;
        const std::string hz_auto = Fmt(T("auto (%d Hz)"), fps::MonitorMaxHz());
        SliderI("Fullscreen refresh rate", &c.fullscreen_hz, 0, 360,
                c.fullscreen_hz == 0 ? hz_auto.c_str() : "%d Hz", 0,
                "The refresh rate requested in exclusive fullscreen (the game forces 60 Hz). "
                "Applies the next time the game switches to fullscreen (restart, or change the "
                "screen mode in the game's settings).");
        ImGui::EndDisabled();
        Status(fps::Status(), fps::Active());
    }
    Check("FPS counter", &c.fps_counter);
    ImGui::TextDisabled(T("Now: %.0f FPS (%.1f ms)"), fps::CurrentFps(), fps::FrameTimeMs());
}

void DrawMiscTab() {
    auto& c = config::Get();
    Separator("Pause");
    Check("Pause key", &c.pause_key_enabled,
          "Freezes the game (enemies, timers, everything) until you press it again. Only when "
          "you are alone in your world: with Seamless Co-op partners it does nothing.");
    ImGui::BeginDisabled(!c.pause_key_enabled);
    int count = 0;
    const char* const* keys = config::KeyNames(&count);
    ImGui::SetNextItemWidth(160 * g_scale);
    if (ImGui::BeginCombo(Id("Keyboard").c_str(), c.pause_key.c_str())) {
        for (int i = 0; i < count; ++i)
            if (ImGui::Selectable(keys[i], c.pause_key == keys[i])) {
                c.pause_key = keys[i];
                config::Save();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(T("Controller: %s"), "R3 + Start");
    ImGui::EndDisabled();
    if (ImGui::Button(Id(features::Paused() ? "Resume now" : "Pause now").c_str())) {
        features::TogglePause();
        overlay::SetMenuOpen(false);
    }
    if (!Showcase()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T(features::PauseStatus().c_str()));
    }

    Separator("Sound");
    Check("Mute when the game is in the background", &c.mute_background,
          "Alt-Tab / another window in front: the game goes silent until you come back.");
    if (!Showcase()) ImGui::TextDisabled(T("Status: %s"), T(features::AudioStatus().c_str()));
}

const char* MenuPadShort() {
    switch (pad::MenuShortcutFromName(config::Get().menu_pad)) {
        case pad::MenuShortcut::kHoldView: return T("hold View");
        case pad::MenuShortcut::kViewStart: return "View + Start";
        case pad::MenuShortcut::kL3R3: return "L3 + R3";
        case pad::MenuShortcut::kNone: break;
    }
    return T("keyboard only");
}

const char* MenuPadHelp() {
    switch (pad::MenuShortcutFromName(config::Get().menu_pad)) {
        case pad::MenuShortcut::kHoldView:
            return T("Hold View to open or close the menu; a short tap still opens the map. "
                     "Start also closes it. LB / RB switch tabs.");
        case pad::MenuShortcut::kViewStart:
            return T("View + Start together opens it, Start closes it. LB / RB switch tabs.");
        case pad::MenuShortcut::kL3R3:
            return T("Click both sticks (L3 + R3) to open or close it. LB / RB switch tabs.");
        case pad::MenuShortcut::kNone: break;
    }
    return T("No controller shortcut: use the keyboard key.");
}

// Shown names of the controller shortcuts (the ini keeps the English names).
const char* PadShortcutLabel(const char* name) {
    if (std::strcmp(name, "HOLD VIEW") == 0) return T("Hold View");
    if (std::strcmp(name, "NONE") == 0) return T("None");
    return name;
}

void DrawSettingsTab() {
    auto& c = config::Get();
    Separator("Language");
    ImGui::SetNextItemWidth(220 * g_scale);
    if (ImGui::BeginCombo(Id("Menu language").c_str(), i18n::Info(i18n::Language()).name)) {
        for (int l = 0; l < i18n::kLangCount; ++l) {
            if (ImGui::Selectable(i18n::Info(l).name, l == i18n::Language())) {
                i18n::SetLanguage(l);
                c.language = i18n::Info(l).code;
                config::Save();
            }
        }
        ImGui::EndCombo();
    }

    Separator("Menu");
    int count = 0;
    const char* const* keys = config::KeyNames(&count);
    ImGui::SetNextItemWidth(220 * g_scale);
    if (ImGui::BeginCombo(Id("Menu key (keyboard)").c_str(), c.menu_key.c_str())) {
        for (int i = 0; i < count; ++i) {
            if (std::string(keys[i]) == "NONE") continue;
            if (ImGui::Selectable(keys[i], c.menu_key == keys[i])) {
                c.menu_key = keys[i];
                config::Save();
            }
        }
        ImGui::EndCombo();
    }
    int pad_count = 0;
    const char* const* pad_names = pad::MenuShortcutNames(&pad_count);
    ImGui::SetNextItemWidth(220 * g_scale);
    if (ImGui::BeginCombo(Id("Menu shortcut (controller)").c_str(),
                          PadShortcutLabel(c.menu_pad.c_str()))) {
        for (int i = 0; i < pad_count; ++i)
            if (ImGui::Selectable(PadShortcutLabel(pad_names[i]), c.menu_pad == pad_names[i])) {
                c.menu_pad = pad_names[i];
                config::Save();
            }
        ImGui::EndCombo();
    }
    if (pad::MenuShortcutFromName(c.menu_pad) == pad::MenuShortcut::kHoldView) {
        ImGui::SetNextItemWidth(220 * g_scale);
        ImGui::SliderInt(Id("Hold time").c_str(), &c.menu_hold_ms, 200, 1500, "%d ms");
        SaveIfEdited();
    }
    ImGui::TextDisabled("%s", MenuPadHelp());
    float scale = ImGui::GetStyle().FontScaleMain;
    ImGui::SetNextItemWidth(220 * g_scale);
    if (ImGui::SliderFloat(Id("Text size").c_str(), &scale, 0.75f, 2.5f, "%.2fx"))
        ImGui::GetStyle().FontScaleMain = scale;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        c.ui_scale = scale;
        config::Save();
    }
    Check("Block game keyboard/mouse input while the menu is open", &c.block_game_input);
    Check("Show a short hint when the game starts", &c.show_hint);

    Separator("About");
    ImGui::Text("zzostoom Quality of Life v%s", kModVersion);
    if (!Showcase())
        ImGui::TextDisabled("%s", game::SingletonReport().c_str());
    Wrapped("No Cheat Engine needed; loaded by Mod Engine 3 as a DLL. Game objects are found by "
            "their class names (RTTI), not fixed addresses. Changes are made in memory only. "
            "UI: Dear ImGui, hooks: MinHook. Log: zzostoom_qol.log next to the DLL.");
}

// ---- Overlay bits -------------------------------------------------------------------------

void DrawToasts() {
    std::lock_guard lock(g_toast_mutex);
    const double now = Now();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(),
                                  [&](const Toast& t) { return t.expires < now; }),
                   g_toasts.end());
    if (g_toasts.empty()) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float pad = 10 * g_scale;
    float y = display.y - 40 * g_scale;
    for (auto it = g_toasts.rbegin(); it != g_toasts.rend(); ++it) {
        const ImVec2 size = ImGui::CalcTextSize(it->text.c_str());
        const ImVec2 max(display.x - 20 * g_scale, y);
        const ImVec2 min(max.x - size.x - pad * 2, y - size.y - pad * 2);
        dl->AddRectFilled(min, max, IM_COL32(16, 14, 10, 230), 6 * g_scale);
        dl->AddRect(min, max, IM_COL32(200, 165, 90, 200), 6 * g_scale);
        dl->AddText(ImVec2(min.x + pad, min.y + pad), IM_COL32(240, 230, 210, 255), it->text.c_str());
        y = min.y - 6 * g_scale;
    }
}

void DrawHint() {
    if (!config::Get().show_hint || Now() - g_start_time > 20.0) return;
    const std::string hint =
        Fmt(T("zzostoom Quality of Life ready  -  menu: %s / %s"), config::Get().menu_key.c_str(), MenuPadShort());
    const char* text = hint.c_str();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 pos(18 * g_scale, 40 * g_scale);
    dl->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(pos, IM_COL32(235, 200, 110, 255), text);
}

// Segoe UI for Latin/Cyrillic, then the Windows CJK fonts merged in for Chinese, Japanese and
// Korean (glyphs load on demand). The menu language's own CJK font goes first so shared
// characters look the way that language writes them.
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    auto add = [&](const wchar_t* file, bool merge, int index = 0) -> ImFont* {
        std::wstring path = std::wstring(windir) + L"\\Fonts\\" + file;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
        ImFontConfig cfg;
        cfg.MergeMode = merge;
        cfg.FontNo = index;
        return io.Fonts->AddFontFromFileTTF(utf8, 18.0f, &cfg);
    };
    if (!add(L"segoeui.ttf", false)) io.Fonts->AddFontDefault();
    // Microsoft YaHei UI, Yu Gothic UI and Malgun Gothic (the .ttc "UI" faces have tighter spacing).
    struct Face {
        const wchar_t* file;
        int index;
    };
    Face cjk[3] = {{L"msyh.ttc", 1}, {L"YuGothR.ttc", 1}, {L"malgun.ttf", 0}};
    if (i18n::Language() == i18n::kJapanese) std::swap(cjk[0], cjk[1]);
    if (i18n::Language() == i18n::kKorean) std::swap(cjk[0], cjk[2]);
    for (const Face& f : cjk) add(f.file, true, f.index);
}

}  // namespace

void SwitchTab(int delta) { g_tab_request = (g_current_tab + delta + kTabCount) % kTabCount; }

void OnMenuToggled(bool open) {
    if (open) g_focus_request = true;
}

void Notify(const std::string& text) {
    std::lock_guard lock(g_toast_mutex);
    g_toasts.push_back({text, Now() + 4.0});
    if (g_toasts.size() > 6) g_toasts.erase(g_toasts.begin());
    ZZ_LOG("Notify: %s", text.c_str());
}

void Init(float scale) {
    g_scale = scale;
    g_start_time = Now();
    LoadFonts();
    ApplyStyle();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontSizeBase = 18.0f;
    const float saved = config::Get().ui_scale;
    style.FontScaleMain = saved > 0.0f ? saved : scale;
    ZZ_LOG("UI ready (language %s)", i18n::Info(i18n::Language()).code);
}

void Draw(bool menu_open) {
    if (TestEnvInt(L"ZZ_HUD_PREVIEW") > 0) hud::DrawPreview();
    hud::Draw();
    DrawToasts();
    if (!menu_open) {
        DrawHint();
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(820 * g_scale, 620 * g_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    if (g_focus_request) {
        ImGui::SetNextWindowFocus();
        g_focus_request = false;
    }
    bool open = true;
    if (ImGui::Begin("zzostoom Quality of Life###zz_qol_main", &open, ImGuiWindowFlags_NoCollapse)) {
        // Screenshots from the test harness (no game) leave out the warning.
        if (!game::InGame() && !Showcase())
            Dim("No character loaded: most features start once you are in game.");
        static int last_test_tab = -1;
        const int test_tab = TestEnvInt(L"ZZ_UI_TEST_TAB");
        int requested = g_tab_request;
        g_tab_request = -1;
        if (test_tab >= 0 && test_tab != last_test_tab) requested = test_tab;
        last_test_tab = test_tab;
        static int last_test_lang = -1;
        if (const int test_lang = TestEnvInt(L"ZZ_UI_TEST_LANG");
            test_lang >= 0 && test_lang != last_test_lang) {
            last_test_lang = test_lang;
            i18n::SetLanguage(test_lang);
        }
        if (ImGui::BeginTabBar("tabs")) {
            for (int t = 0; t < kTabCount; ++t) {
                const ImGuiTabItemFlags flags =
                    requested == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                const std::string tab_label = std::string(T(kTabs[t])) + "###tab" + std::to_string(t);
                if (!ImGui::BeginTabItem(tab_label.c_str(), nullptr, flags)) continue;
                g_current_tab = t;
                switch (t) {
                    case 0: DrawCameraTab(); break;
                    case 1: DrawHudTab(); break;
                    case 2: DrawControlsTab(); break;
                    case 3: DrawPerformanceTab(); break;
                    case 4: DrawMiscTab(); break;
                    case 5: DrawSettingsTab(); break;
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) overlay::SetMenuOpen(false);
}

}  // namespace zz::ui
