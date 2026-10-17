#pragma once
#include <cstdint>
#include <string>

// Controller filter on XInputGetState (xinput1_4.dll):
//  - Menu shortcut (configurable): hold View (default), View + Start together, or L3 + R3
//    toggles the QoL menu; R3 + Start pauses. While the menu is open the game sees an idle
//    controller. Buttons that may start the shortcut are held back from the game until it is
//    clear they don't (a short View tap still opens the map, on release).
//  - Instant roll: Elden Ring rolls when the dodge button is *released* (holding it means
//    sprint), which feels like input lag. On press we show the game a short press + release, so
//    the roll starts right away; if the button is still held after `sprint_delay_ms`, we press it
//    again for the game, which then sprints. A sprint started this way is held for at least
//    `sprint_min_ms`, so letting go never looks like a second tap (= a second roll) to the game.
// The hook is installed at start-up, before zzostoom cheat sheet's (which then sees our output).
namespace zz::pad {

enum Button : uint32_t {
    kUp = 0x0001,
    kDown = 0x0002,
    kLeft = 0x0004,
    kRight = 0x0008,
    kMenu = 0x0010,  // Start / Options
    kView = 0x0020,  // Back / Share
    kL3 = 0x0040,
    kR3 = 0x0080,
    kLB = 0x0100,
    kRB = 0x0200,
    kA = 0x1000,
    kB = 0x2000,
    kX = 0x4000,
    kY = 0x8000,
    kLT = 0x10000,
    kRT = 0x20000,
};

bool Install();  // creates and enables the hook immediately
bool Installed();

enum class MenuShortcut { kHoldView, kViewStart, kL3R3, kNone };
// Names stored in the ini / shown in the menu, in MenuShortcut order.
const char* const* MenuShortcutNames(int* count);
MenuShortcut MenuShortcutFromName(const std::string& name);

void SetMenuOpen(bool open);
// The controller menu shortcut was used since the last call.
bool TakeMenuToggle();
// R3 + Start (Menu / Options) was pressed since the last call.
bool TakePauseToggle();
// Whether the instant roll may act right now (in game, on foot); set every frame.
void SetRollAllowed(bool allowed);

struct RawState {
    bool connected = false;
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1
    float lt = 0, rt = 0;                  // 0..1
};
// First connected controller, unfiltered.
RawState ReadRaw();

uint32_t ButtonFromName(const std::string& name);  // "A", "B", "RB", "LT"... (0 if unknown)
const char* const* RollButtonNames(int* count);

#ifdef ZZ_PAD_TEST
// Test hooks: feed a state through the filter with a fake clock (milliseconds).
struct TestConfig {
    bool instant_roll = true;
    uint32_t roll_button = kB;
    int sprint_delay_ms = 220;
    int sprint_min_ms = 600;
    bool cheat_sheet_loaded = false;
    MenuShortcut menu = MenuShortcut::kHoldView;
    int menu_hold_ms = 500;
};
void TestReset(const TestConfig& cfg);
void TestPoll(double now_ms, uint16_t buttons, uint16_t* out_buttons, uint8_t* out_lt = nullptr,
              uint8_t* out_rt = nullptr, uint8_t lt = 0, uint8_t rt = 0);
#endif

}  // namespace zz::pad
