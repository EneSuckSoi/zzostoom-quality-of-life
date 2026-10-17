#include "pad.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>

#include "MinHook.h"
#include "config.h"
#include "log.h"

namespace zz::pad {

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

GetStateFn g_original = nullptr;  // trampoline once hooked
GetStateFn g_raw = nullptr;       // XInputGetStateEx (ordinal 100): never hooked by anyone
bool g_installed = false;
std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_menu_toggle{false};
std::atomic<bool> g_pause_toggle{false};
std::atomic<bool> g_roll_allowed{false};
std::mutex g_mutex;

constexpr BYTE kTriggerThreshold = 100;
// The game samples the pad once per frame: a synthetic press/release is shown for at least two
// polls and this many milliseconds, so it is never missed even at low frame rates.
constexpr int kMinPolls = 2;
constexpr double kMinPhaseMs = 12.0;
constexpr double kComboWindowMs = 90.0;
constexpr double kTapMs = 40.0;

struct Settings {
    bool instant_roll = true;
    uint32_t roll_button = kB;
    double sprint_delay_ms = 220;
    double sprint_min_ms = 600;
    bool cheat_sheet = false;  // zzostoom cheat sheet hides R3 combos itself
    MenuShortcut menu = MenuShortcut::kHoldView;
    double menu_hold_ms = 500;
};
Settings g_settings;

enum class Phase { kIdle, kTapDown, kTapUp, kWaitHold, kSprint, kSprintRelease };

struct UserState {
    uint32_t prev = 0;
    uint32_t consumed = 0;  // hidden from the game until released
    bool was_open = false;
    Phase phase = Phase::kIdle;
    double phase_start = 0;
    double press_time = 0;
    int phase_polls = 0;
    // View + Start: a lone press of either is held back for kComboWindowMs in case the other
    // follows, then given to the game (as a short tap if it was already released).
    uint32_t pending = 0;
    double pending_since = 0;
    // Hold View: View is held back from the game while it may still become the shortcut.
    bool hold_active = false;
    double hold_since = 0;
    uint32_t tap = 0;
    double tap_start = 0;
    int tap_polls = 0;
    DWORD last_raw_packet = 0;
    DWORD packet_bias = 0;
    XINPUT_GAMEPAD last_out{};
};
UserState g_users[XUSER_MAX_COUNT];

double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

uint32_t ToMask(const XINPUT_GAMEPAD& g) {
    uint32_t m = g.wButtons & 0xF3FF;
    if (g.bLeftTrigger > kTriggerThreshold) m |= kLT;
    if (g.bRightTrigger > kTriggerThreshold) m |= kRT;
    return m;
}

void SetPhase(UserState& s, Phase p, double now) {
    s.phase = p;
    s.phase_start = now;
    s.phase_polls = 0;
}

// Returns whether the game should see the roll button held this poll.
bool StepRoll(UserState& s, bool held, bool pressed, bool enabled, double now) {
    const Settings& cfg = g_settings;
    if (!enabled) {
        // Hold a sprint we started until its minimum time, then hand the button back.
        if ((s.phase == Phase::kSprint || s.phase == Phase::kSprintRelease) &&
            now - s.phase_start < cfg.sprint_min_ms)
            return true;
        s.phase = Phase::kIdle;
        return held;
    }
    ++s.phase_polls;
    const bool phase_done = s.phase_polls > kMinPolls && now - s.phase_start >= kMinPhaseMs;
    switch (s.phase) {
        case Phase::kIdle:
            if (pressed) {
                s.press_time = now;
                SetPhase(s, Phase::kTapDown, now);
                s.phase_polls = 1;
                return true;
            }
            return held;  // e.g. held since before the mod was enabled
        case Phase::kTapDown:
            if (!phase_done) return true;
            SetPhase(s, Phase::kTapUp, now);
            s.phase_polls = 1;
            return false;
        case Phase::kTapUp:
            if (!phase_done) return false;
            SetPhase(s, held ? Phase::kWaitHold : Phase::kIdle, now);
            return StepRoll(s, held, false, enabled, now);
        case Phase::kWaitHold:
            if (!held) {
                SetPhase(s, Phase::kIdle, now);
                return false;
            }
            if (now - s.press_time >= cfg.sprint_delay_ms) {
                SetPhase(s, Phase::kSprint, now);
                return true;
            }
            return false;
        case Phase::kSprint:
            if (held) return true;
            if (now - s.phase_start >= cfg.sprint_min_ms) {
                SetPhase(s, Phase::kIdle, now);
                return false;
            }
            s.phase = Phase::kSprintRelease;  // keep phase_start: the minimum counts from the press
            return true;
        case Phase::kSprintRelease:
            if (pressed) {
                // A new press while we still hold the sprint: release now; the game reads the
                // short hold as a roll, which is what the new press asked for.
                s.press_time = now;
                SetPhase(s, Phase::kTapUp, now);
                s.phase_polls = 1;
                return false;
            }
            if (now - s.phase_start >= cfg.sprint_min_ms) {
                SetPhase(s, Phase::kIdle, now);
                return false;
            }
            return true;
    }
    return held;
}

void Process(DWORD user, XINPUT_STATE* st, double now) {
    UserState& s = g_users[user];
    const Settings& cfg = g_settings;
    const uint32_t raw = ToMask(st->Gamepad);
    const uint32_t pressed = raw & ~s.prev;
    const bool open = g_menu_open;

    // Buttons still held when the menu closes stay hidden until released (B used to close the
    // menu must not dodge, Start must not open the game menu...).
    if (s.was_open && !open) s.consumed |= raw;
    s.was_open = open;

    uint32_t held_back = 0;
    auto replay_tap = [&](uint32_t buttons) {
        if (open) return;
        s.tap = buttons;
        s.tap_start = now;
        s.tap_polls = 0;
    };
    switch (cfg.menu) {
        case MenuShortcut::kHoldView:
            if ((pressed & kView) && !(s.consumed & kView) && !(raw & (kL3 | kR3))) {
                s.hold_active = true;
                s.hold_since = now;
            }
            if (s.hold_active) {
                if (!(raw & kView)) {  // a short tap: the game gets it now (map)
                    s.hold_active = false;
                    replay_tap(kView);
                } else if (now - s.hold_since >= cfg.menu_hold_ms) {
                    g_menu_toggle = true;
                    s.consumed |= kView;
                    s.hold_active = false;
                } else {
                    held_back |= kView;
                }
            }
            break;
        case MenuShortcut::kViewStart: {
            // A lone View / Start press is held back for kComboWindowMs in case the other follows.
            const uint32_t combo = kView | kMenu;
            // Opens only: in the menu, Start alone closes it (overlay).
            if (!open && (raw & combo) == combo && (pressed & combo) && !(s.consumed & combo)) {
                g_menu_toggle = true;
                s.consumed |= combo;
                s.pending = 0;
            } else if (!open && !s.pending && (pressed & combo) && !(raw & kR3) &&
                       !(s.consumed & pressed & combo)) {
                s.pending = pressed & combo;
                if (s.pending == combo) s.pending = kView;
                s.pending_since = now;
            }
            if (s.pending) {
                if (!(raw & s.pending)) {  // released inside the window: replay it as a tap
                    replay_tap(s.pending);
                    s.pending = 0;
                } else if (now - s.pending_since >= kComboWindowMs || open) {
                    s.pending = 0;  // held: the game gets it from now on
                } else {
                    held_back = s.pending;
                }
            }
            break;
        }
        case MenuShortcut::kL3R3: {
            const uint32_t combo = kL3 | kR3;
            if ((raw & combo) == combo && (pressed & combo) && !(s.consumed & combo)) {
                g_menu_toggle = true;
                s.consumed |= combo;
            }
            break;
        }
        case MenuShortcut::kNone:
            break;
    }
    // R3 + Start pauses. Without the cheat sheet nobody else hides Start (game menu) from the game.
    if ((raw & kR3) && (pressed & kMenu) && !open && !(s.consumed & kMenu)) {
        g_pause_toggle = true;
        if (!cfg.cheat_sheet) s.consumed |= kMenu;
    }
    uint32_t forced = 0;
    if (s.tap) {
        forced = s.tap;
        if (++s.tap_polls > kMinPolls && now - s.tap_start >= kTapMs) s.tap = 0;
    }
    s.consumed &= raw;

    XINPUT_GAMEPAD out = st->Gamepad;
    if (open) {
        out = XINPUT_GAMEPAD{};
        s.phase = Phase::kIdle;
    } else {
        const uint32_t hide = s.consumed | held_back;
        const uint32_t rb = cfg.instant_roll ? cfg.roll_button : 0;
        WORD buttons = out.wButtons;
        if (rb) {
            // L3/R3 held = a cheat sheet shortcut (L3 + B...): leave the roll button alone.
            const bool enabled = g_roll_allowed && !(raw & (kL3 | kR3)) && !(hide & rb);
            const bool roll_out = StepRoll(s, (raw & rb) != 0, (pressed & rb) != 0, enabled, now);
            if (rb & 0xFFFF)
                buttons = roll_out ? (buttons | static_cast<WORD>(rb))
                                   : (buttons & ~static_cast<WORD>(rb));
            else if (rb == kLT)
                out.bLeftTrigger = roll_out ? 255 : 0;
            else if (rb == kRT)
                out.bRightTrigger = roll_out ? 255 : 0;
        } else {
            s.phase = Phase::kIdle;
        }
        // Hidden buttons win over everything (also over a synthetic roll press).
        out.wButtons = (buttons & static_cast<WORD>(~(hide & 0xFFFF))) | static_cast<WORD>(forced);
        if (hide & kLT) out.bLeftTrigger = 0;
        if (hide & kRT) out.bRightTrigger = 0;
    }
    s.prev = raw;

    // Games may skip polls whose packet number did not change; make our edits visible.
    if (st->dwPacketNumber == s.last_raw_packet && std::memcmp(&out, &s.last_out, sizeof(out)) != 0)
        ++s.packet_bias;
    s.last_raw_packet = st->dwPacketNumber;
    s.last_out = out;
    st->Gamepad = out;
    st->dwPacketNumber += s.packet_bias;
}

void LoadSettings() {
    const auto& c = config::Get();
    g_settings.instant_roll = c.instant_roll;
    g_settings.roll_button = ButtonFromName(c.roll_button);
    g_settings.sprint_delay_ms = c.sprint_delay_ms;
    g_settings.sprint_min_ms = c.sprint_min_ms;
    g_settings.cheat_sheet = GetModuleHandleW(L"zzostoom_cheat_sheet.dll") != nullptr;
    g_settings.menu = MenuShortcutFromName(c.menu_pad);
    g_settings.menu_hold_ms = c.menu_hold_ms;
}

DWORD WINAPI HookedGetState(DWORD user, XINPUT_STATE* state) {
    DWORD result = g_original(user, state);
    if (result == ERROR_SUCCESS && state && user < XUSER_MAX_COUNT) {
        std::lock_guard lock(g_mutex);
        static int settings_version = -1;
        if (config::Version() != settings_version) {  // only when the menu saved something
            settings_version = config::Version();
            LoadSettings();
        }
        Process(user, state, NowMs());
    }
    return result;
}

struct NamedButton {
    uint32_t bit;
    const char* name;
};
const NamedButton kButtons[] = {
    {kA, "A"},   {kB, "B"},   {kX, "X"},   {kY, "Y"},   {kLB, "LB"},
    {kRB, "RB"}, {kLT, "LT"}, {kRT, "RT"}, {kL3, "L3"}, {kR3, "R3"},
};

float Stick(SHORT v, SHORT deadzone) {
    if (v > -deadzone && v < deadzone) return 0.0f;
    return std::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f);
}

}  // namespace

uint32_t ButtonFromName(const std::string& name) {
    std::string u;
    for (char c : name)
        if (!std::isspace(static_cast<unsigned char>(c)))
            u.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (auto& b : kButtons)
        if (u == b.name) return b.bit;
    return 0;
}

const char* const* MenuShortcutNames(int* count) {
    static const char* names[] = {"HOLD VIEW", "VIEW + START", "L3 + R3", "NONE"};
    *count = static_cast<int>(std::size(names));
    return names;
}

MenuShortcut MenuShortcutFromName(const std::string& name) {
    int count = 0;
    const char* const* names = MenuShortcutNames(&count);
    for (int i = 0; i < count; ++i)
        if (_stricmp(name.c_str(), names[i]) == 0) return static_cast<MenuShortcut>(i);
    return MenuShortcut::kHoldView;
}

const char* const* RollButtonNames(int* count) {
    static const char* names[] = {"B", "A", "X", "Y", "RB", "LB", "RT", "LT"};
    *count = static_cast<int>(std::size(names));
    return names;
}

bool Install() {
    HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
    auto target = xinput ? reinterpret_cast<GetStateFn>(GetProcAddress(xinput, "XInputGetState"))
                         : nullptr;
    if (!target) {
        ZZ_LOG("XInput not available, controller features disabled");
        return false;
    }
    g_raw = reinterpret_cast<GetStateFn>(GetProcAddress(xinput, reinterpret_cast<LPCSTR>(100)));
    g_original = target;
    LoadSettings();
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        ZZ_LOG("MH_Initialize failed (%d)", static_cast<int>(s));
        return false;
    }
    s = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(HookedGetState),
                      reinterpret_cast<void**>(&g_original));
    if (s == MH_OK) s = MH_EnableHook(reinterpret_cast<void*>(target));
    if (s != MH_OK) {
        g_original = target;
        ZZ_LOG("XInputGetState hook failed (%d)", static_cast<int>(s));
        return false;
    }
    g_installed = true;
    ZZ_LOG("XInput hook installed (raw reads via %s)", g_raw ? "XInputGetStateEx" : "trampoline");
    return true;
}

bool Installed() { return g_installed; }
void SetMenuOpen(bool open) { g_menu_open = open; }
bool TakeMenuToggle() { return g_menu_toggle.exchange(false); }
bool TakePauseToggle() { return g_pause_toggle.exchange(false); }
void SetRollAllowed(bool allowed) { g_roll_allowed = allowed; }

RawState ReadRaw() {
    RawState out;
    GetStateFn read = g_raw ? g_raw : g_original;
    if (!read) return out;
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
        XINPUT_STATE st{};
        if (read(user, &st) != ERROR_SUCCESS) continue;
        const auto& g = st.Gamepad;
        out.connected = true;
        out.buttons = ToMask(g);
        out.lx = Stick(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        out.ly = Stick(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        out.rx = Stick(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        out.ry = Stick(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        out.lt = g.bLeftTrigger / 255.0f;
        out.rt = g.bRightTrigger / 255.0f;
        break;
    }
    return out;
}

#ifdef ZZ_PAD_TEST
void TestReset(const TestConfig& cfg) {
    std::lock_guard lock(g_mutex);
    for (auto& u : g_users) u = UserState{};
    g_settings.instant_roll = cfg.instant_roll;
    g_settings.roll_button = cfg.roll_button;
    g_settings.sprint_delay_ms = cfg.sprint_delay_ms;
    g_settings.sprint_min_ms = cfg.sprint_min_ms;
    g_settings.cheat_sheet = cfg.cheat_sheet_loaded;
    g_settings.menu = cfg.menu;
    g_settings.menu_hold_ms = cfg.menu_hold_ms;
    g_roll_allowed = true;
    g_menu_open = false;
}

void TestPoll(double now_ms, uint16_t buttons, uint16_t* out_buttons, uint8_t* out_lt,
              uint8_t* out_rt, uint8_t lt, uint8_t rt) {
    std::lock_guard lock(g_mutex);
    static DWORD packet = 0;
    XINPUT_STATE st{};
    st.dwPacketNumber = ++packet;
    st.Gamepad.wButtons = buttons;
    st.Gamepad.bLeftTrigger = lt;
    st.Gamepad.bRightTrigger = rt;
    Process(0, &st, now_ms);
    *out_buttons = st.Gamepad.wButtons;
    if (out_lt) *out_lt = st.Gamepad.bLeftTrigger;
    if (out_rt) *out_rt = st.Gamepad.bRightTrigger;
}
#endif

}  // namespace zz::pad
