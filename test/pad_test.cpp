// Unit test for the controller filter (instant roll, menu combo). Built by `py build.py --test`.
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <string>
#include <vector>

#include "pad.h"

namespace {

int g_failures = 0;

void Expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

struct Sample {
    double t;
    bool game_b;
};

// Simulates the game polling at `hz` while the user holds B from `down` to `up` (ms).
std::vector<Sample> Run(double hz, double down, double up, double until,
                        uint16_t extra = 0, zz::pad::TestConfig cfg = {}) {
    zz::pad::TestReset(cfg);
    std::vector<Sample> out;
    for (double t = 0; t < until; t += 1000.0 / hz) {
        uint16_t buttons = extra;
        if (t >= down && t < up) buttons |= XINPUT_GAMEPAD_B;
        uint16_t game = 0;
        zz::pad::TestPoll(t, buttons, &game);
        out.push_back({t, (game & XINPUT_GAMEPAD_B) != 0});
    }
    return out;
}

// Presses the game saw: {start, duration} in ms.
std::vector<std::pair<double, double>> Presses(const std::vector<Sample>& s) {
    std::vector<std::pair<double, double>> out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].game_b && (i == 0 || !s[i - 1].game_b)) out.push_back({s[i].t, 0});
        if (!s[i].game_b && i > 0 && s[i - 1].game_b) out.back().second = s[i].t - out.back().first;
    }
    return out;
}

void PrintPresses(const char* name, const std::vector<std::pair<double, double>>& p) {
    std::printf("  %s:", name);
    for (auto& [start, len] : p) std::printf(" [%.0f ms, %.0f ms]", start, len);
    std::printf("\n");
}

}  // namespace

int main() {
    for (double hz : {160.0, 60.0, 30.0}) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "@%.0f Hz", hz);

        // A quick tap: one short press for the game, starting on the user's press.
        auto tap = Presses(Run(hz, 100, 180, 1500));
        PrintPresses("tap 80 ms", tap);
        Expect(tap.size() == 1 && tap[0].first >= 99.9 && tap[0].first < 100 + 1000 / hz &&
                   tap[0].second < 70,
               std::string("tap -> one immediate short press ") + tag);

        // Holding: roll, then a sprint press until the user lets go.
        auto hold = Presses(Run(hz, 100, 1500, 2500));
        PrintPresses("hold 1400 ms", hold);
        Expect(hold.size() == 2 && hold[0].second < 70 && hold[1].first >= 100 + 220 &&
                   hold[1].first < 100 + 220 + 40 && hold[1].first + hold[1].second >= 1500 &&
                   hold[1].first + hold[1].second < 1550,
               std::string("hold -> roll then sprint until release ") + tag);

        // Borderline hold: the sprint press is extended so its release is not a second roll.
        auto mid = Presses(Run(hz, 100, 400, 2000));
        PrintPresses("hold 300 ms", mid);
        Expect(mid.size() == 2 && mid[1].second >= 600 - 1,
               std::string("short sprint is held for sprint_min_ms ") + tag);
    }

    // L3 held: the cheat sheet's "L3 + B" shortcut, B passes through untouched.
    {
        auto s = Run(160, 100, 400, 800, XINPUT_GAMEPAD_LEFT_THUMB);
        auto p = Presses(s);
        PrintPresses("L3 + B", p);
        Expect(p.size() == 1 && p[0].first >= 99.9 && p[0].first < 107 && p[0].second >= 293 &&
                   p[0].second < 310,
               "L3 held -> B untouched");
    }

    // Disabled: untouched.
    {
        zz::pad::TestConfig cfg;
        cfg.instant_roll = false;
        auto p = Presses(Run(160, 100, 400, 800, 0, cfg));
        Expect(p.size() == 1 && p[0].first >= 99.9 && p[0].first < 107 && p[0].second >= 293,
               "disabled -> untouched");
    }

    // Another roll button (A).
    {
        zz::pad::TestConfig cfg;
        cfg.roll_button = zz::pad::kA;
        zz::pad::TestReset(cfg);
        int presses = 0;
        bool prev = false;
        for (double t = 0; t < 1000; t += 6.25) {
            uint16_t game = 0;
            zz::pad::TestPoll(t, (t >= 100 && t < 150) ? XINPUT_GAMEPAD_A : 0, &game);
            const bool a = game & XINPUT_GAMEPAD_A;
            if (a && !prev) ++presses;
            prev = a;
        }
        Expect(presses == 1, "roll on A works");
    }

    // Trigger as roll button.
    {
        zz::pad::TestConfig cfg;
        cfg.roll_button = zz::pad::kRT;
        zz::pad::TestReset(cfg);
        uint16_t game = 0;
        uint8_t lt = 0, rt = 0;
        zz::pad::TestPoll(0, 0, &game, &lt, &rt, 0, 255);
        Expect(rt == 255, "RT roll: press shown");
        zz::pad::TestPoll(6, 0, &game, &lt, &rt, 0, 255);
        zz::pad::TestPoll(13, 0, &game, &lt, &rt, 0, 255);
        zz::pad::TestPoll(20, 0, &game, &lt, &rt, 0, 255);
        Expect(rt == 0, "RT roll: released for the game while still held");
    }

    // Hold View (default): opens the menu after the hold time; a short tap is the map.
    {
        zz::pad::TestConfig cfg;
        zz::pad::TestReset(cfg);
        uint16_t game = 0;
        bool view_seen = false;
        bool toggled_early = false;
        for (double t = 0; t < 900; t += 6.25) {
            zz::pad::TestPoll(t, (t >= 100 && t < 800) ? XINPUT_GAMEPAD_BACK : 0, &game);
            view_seen |= (game & XINPUT_GAMEPAD_BACK) != 0;
            if (t < 590) toggled_early |= zz::pad::TakeMenuToggle();
        }
        Expect(zz::pad::TakeMenuToggle() && !toggled_early && !view_seen,
               "hold View -> menu after 500 ms, View hidden from the game");
        zz::pad::TestReset(cfg);
        int presses = 0;
        bool prev = false;
        double first = -1;
        for (double t = 0; t < 600; t += 6.25) {
            zz::pad::TestPoll(t, (t >= 100 && t < 200) ? XINPUT_GAMEPAD_BACK : 0, &game);
            const bool v = game & XINPUT_GAMEPAD_BACK;
            if (v && !prev) {
                ++presses;
                if (first < 0) first = t;
            }
            prev = v;
        }
        Expect(presses == 1 && first >= 199 && first < 210 && !zz::pad::TakeMenuToggle(),
               "short View tap -> one View press on release (map), no menu");
        // Start is untouched in this mode.
        zz::pad::TestReset(cfg);
        zz::pad::TestPoll(0, XINPUT_GAMEPAD_START, &game);
        Expect(game & XINPUT_GAMEPAD_START, "hold View mode -> Start reaches the game at once");
    }

    // View + Start together: menu toggle, neither reaches the game. A lone View still does.
    {
        zz::pad::TestConfig cfg;
        cfg.menu = zz::pad::MenuShortcut::kViewStart;
        zz::pad::TestReset(cfg);
        uint16_t game = 0;
        bool view_seen = false, start_seen = false;
        for (double t = 0; t < 400; t += 6.25) {
            uint16_t b = 0;
            if (t >= 100 && t < 300) b |= XINPUT_GAMEPAD_BACK;
            if (t >= 130 && t < 300) b |= XINPUT_GAMEPAD_START;
            zz::pad::TestPoll(t, b, &game);
            view_seen |= (game & XINPUT_GAMEPAD_BACK) != 0;
            start_seen |= (game & XINPUT_GAMEPAD_START) != 0;
        }
        Expect(zz::pad::TakeMenuToggle() && !view_seen && !start_seen,
               "View + Start toggles the menu, hidden from the game");
        zz::pad::TestReset(cfg);
        int presses = 0;
        bool prev = false;
        double first = -1;
        for (double t = 0; t < 400; t += 6.25) {
            zz::pad::TestPoll(t, (t >= 100 && t < 150) ? XINPUT_GAMEPAD_BACK : 0, &game);
            const bool v = game & XINPUT_GAMEPAD_BACK;
            if (v && !prev) {
                ++presses;
                if (first < 0) first = t;
            }
            prev = v;
        }
        Expect(presses == 1 && first >= 149 && first < 160 && !zz::pad::TakeMenuToggle(),
               "short View tap reaches the game as one tap when released");
        zz::pad::TestReset(cfg);
        double start = -1;
        for (double t = 0; t < 400; t += 6.25) {
            zz::pad::TestPoll(t, (t >= 100 && t < 350) ? XINPUT_GAMEPAD_START : 0, &game);
            if ((game & XINPUT_GAMEPAD_START) && start < 0) start = t;
        }
        Expect(start >= 189 && start < 200, "held Start reaches the game after 90 ms");
    }

    // L3 + R3.
    {
        zz::pad::TestConfig cfg;
        cfg.menu = zz::pad::MenuShortcut::kL3R3;
        zz::pad::TestReset(cfg);
        uint16_t game = 0;
        zz::pad::TestPoll(0, XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB, &game);
        Expect(zz::pad::TakeMenuToggle() && game == 0, "L3 + R3 toggles the menu, hidden");
    }

    // Menu open: idle; buttons held while closing stay hidden.
    {
        zz::pad::TestConfig cfg;
        zz::pad::TestReset(cfg);
        zz::pad::SetMenuOpen(true);
        uint16_t game = 0;
        zz::pad::TestPoll(0, XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B, &game);
        Expect(game == 0, "menu open -> idle pad");
        zz::pad::SetMenuOpen(false);
        zz::pad::TestPoll(10, XINPUT_GAMEPAD_B, &game);
        Expect(!(game & XINPUT_GAMEPAD_B), "B used in the menu stays hidden after closing");
        zz::pad::TestPoll(20, 0, &game);
        zz::pad::TestPoll(30, XINPUT_GAMEPAD_B, &game);
        Expect(game & XINPUT_GAMEPAD_B, "next B press works again");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
