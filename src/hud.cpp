#include "hud.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "buff_db.h"
#include "buffs.h"
#include "config.h"
#include "features.h"
#include "fps.h"
#include "game.h"
#include "i18n.h"
#include "imgui.h"
#include "log.h"

namespace zz::hud {

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Track {
    float last_sa = -1;
    int last_hp = -1;
    double first_seen = 0;
    double last_change = -1e9;  // last time poise or HP went down
    double last_seen = 0;
    double broken_until = 0;
    float shown = 0;  // smoothed damage ratio for the bar
    int last_buildup = 0;  // sum of all status build-ups
    game::ActiveStatus active;  // statuses in effect, re-read a few times per second
    double active_read = -1;
    game::Chr chr;
};
std::unordered_map<uintptr_t, Track> g_tracks;
Track g_player;

struct CameraFix {
    int flip_votes = 0;
    bool space_ok = true;
} g_camfix;

ImU32 Rgba(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

// Sekiro-like colours: amber while low, orange, then red close to breaking.
ImU32 StanceColor(float ratio, float alpha) {
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    int r, g, b;
    if (ratio < 0.6f) {
        const float t = ratio / 0.6f;
        r = 236;
        g = static_cast<int>(196 - 60 * t);
        b = static_cast<int>(92 - 50 * t);
    } else {
        const float t = (ratio - 0.6f) / 0.4f;
        r = static_cast<int>(236 - 10 * t);
        g = static_cast<int>(136 - 96 * t);
        b = static_cast<int>(42 - 10 * t);
    }
    return Rgba(r, g, b, alpha);
}

void Shadowed(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text,
              float align = 0.0f) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(size, 1e9f, 0.0f, text);
    pos.x -= sz.x * align;
    const float a = ((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    dl->AddText(font, size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), Rgba(0, 0, 0, a * 0.85f), text);
    dl->AddText(font, size, pos, color, text);
}

// Bar filling from its centre outwards (the posture bar look). `flash` 0..1 after a break.
void StanceBar(ImDrawList* dl, ImVec2 c, float w, float h, float ratio, float flash, float alpha) {
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    const float round = h * 0.5f;
    dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), Rgba(0, 0, 0, alpha * 0.55f),
                      round);
    dl->AddRectFilled(a, b, Rgba(28, 24, 18, alpha * 0.75f), round);
    const float half = w * 0.5f * std::clamp(ratio, 0.0f, 1.0f);
    if (half > 0.5f)
        dl->AddRectFilled(ImVec2(c.x - half, a.y), ImVec2(c.x + half, b.y),
                          StanceColor(ratio, alpha), round);
    if (flash > 0)
        dl->AddRectFilled(a, b, Rgba(255, 238, 190, alpha * flash), round);
    // centre notch
    dl->AddLine(ImVec2(c.x, a.y - h * 0.35f), ImVec2(c.x, b.y + h * 0.35f),
                Rgba(255, 230, 170, alpha * 0.9f), std::max(1.0f, h * 0.18f));
    dl->AddRect(a, b, Rgba(205, 170, 95, alpha * 0.7f), round, 0, std::max(1.0f, h * 0.12f));
}

void HpBar(ImDrawList* dl, ImVec2 c, float w, float h, float ratio, float alpha) {
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(a, b, Rgba(20, 10, 10, alpha * 0.7f));
    dl->AddRectFilled(a, ImVec2(a.x + w * std::clamp(ratio, 0.0f, 1.0f), b.y),
                      Rgba(170, 30, 30, alpha * 0.9f));
}

std::string FormatHp(int hp, int max_hp) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d / %d", hp > 0 ? hp : 0, max_hp);
    return buf;
}

std::string FormatTime(float seconds) {
    char buf[16];
    const int s = static_cast<int>(std::ceil(seconds));
    if (s >= 60)
        std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    else
        std::snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

// `seen`: the character was found in the game's character sets (not just re-read).
void UpdateTrack(Track& t, const game::Chr& c, double now, bool seen = true) {
    if (t.last_hp < 0) {
        t.first_seen = now;
    } else {
        // Characters without poise use -1 as max (e.g. NPCs standing invisible at graces):
        // only real poise damage or HP loss counts as being in a fight.
        const bool has_poise = c.sa_max > 1.0f && t.last_sa >= 0.0f && c.sa >= 0.0f;
        const bool sa_down = has_poise && c.sa < t.last_sa - 0.01f;
        const bool hp_down = c.hp > 0 && c.hp < t.last_hp && t.last_hp <= c.max_hp;
        int buildup = 0;
        for (int b : c.buildup) buildup += b;
        const bool status_up = buildup > t.last_buildup;
        t.last_buildup = buildup;
        if (sa_down || hp_down || status_up) t.last_change = now;
        const bool hit_zero = has_poise && t.last_sa > 0.01f && c.sa <= 0.01f;
        const bool reset_after_low = has_poise && t.last_sa < 0.25f * c.sa_max &&
                                     c.sa >= c.sa_max - 0.01f && hp_down;
        if (hit_zero || reset_after_low) t.broken_until = now + 0.9;
    }
    t.last_sa = c.sa;
    t.last_hp = c.hp;
    if (seen) t.last_seen = now;
    t.chr = c;
    const float target = c.sa_max > 1.0f ? 1.0f - std::clamp(c.sa / c.sa_max, 0.0f, 1.0f) : 0.0f;
    t.shown += (target - t.shown) * 0.35f;  // smooth the bar a little
    if (std::fabs(target - t.shown) < 0.002f) t.shown = target;
}

float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Status colours, muted to sit with the game's own HUD (dark frames, worn gold, desaturated hues).
const ImU32 kStatusColor[game::kStatusCount] = {
    IM_COL32(124, 146, 78, 255),   // poison: moss green
    IM_COL32(176, 92, 62, 255),    // scarlet rot: rust
    IM_COL32(158, 40, 44, 255),    // blood loss: dried blood
    IM_COL32(124, 112, 130, 255),  // death blight: ashen violet
    IM_COL32(146, 178, 194, 255),  // frostbite: pale ice
    IM_COL32(126, 116, 166, 255),  // sleep: dusk lavender
    IM_COL32(200, 150, 62, 255),   // madness: tarnished amber
};

ImU32 WithAlpha(ImU32 c, float a) {
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT);
}

// `c` scaled towards black (k < 1) or white (k > 1).
ImU32 Shade(ImU32 c, float k, float alpha) {
    auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xFF);
        return static_cast<int>(std::clamp(k <= 1.0f ? v * k : v + (255.0f - v) * (k - 1.0f), 0.0f, 255.0f));
    };
    return Rgba(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), alpha);
}

const ImU32 kFrameGold = IM_COL32(190, 160, 100, 255);

// The symbol of a status, `u` = its radius. `hole` is the colour used to cut shapes out.
void StatusGlyph(ImDrawList* dl, int status, ImVec2 c, float u, ImU32 col, ImU32 hole) {
    const float line = std::max(1.0f, u * 0.26f);
    switch (status) {
        case game::kBloodLoss:  // drop
            dl->AddCircleFilled(ImVec2(c.x, c.y + u * 0.35f), u * 0.62f, col, 16);
            dl->AddTriangleFilled(ImVec2(c.x, c.y - u), ImVec2(c.x - u * 0.6f, c.y + u * 0.2f),
                                  ImVec2(c.x + u * 0.6f, c.y + u * 0.2f), col);
            break;
        case game::kFrostbite:  // snowflake
            for (int k = 0; k < 3; ++k) {
                const float a = 3.14159265f * (0.5f + k / 3.0f);
                const ImVec2 d(std::cos(a) * u, std::sin(a) * u);
                dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), col, line);
            }
            break;
        case game::kPoison:  // bubbles
            dl->AddCircleFilled(ImVec2(c.x - u * 0.35f, c.y + u * 0.3f), u * 0.5f, col, 14);
            dl->AddCircleFilled(ImVec2(c.x + u * 0.45f, c.y + u * 0.05f), u * 0.33f, col, 12);
            dl->AddCircleFilled(ImVec2(c.x + u * 0.05f, c.y - u * 0.62f), u * 0.24f, col, 10);
            break;
        case game::kScarletRot:  // blotch with spores
            dl->AddCircleFilled(c, u * 0.48f, col, 14);
            for (int k = 0; k < 5; ++k) {
                const float a = 3.14159265f * 2.0f * k / 5.0f - 1.57f;
                dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * u * 0.8f, c.y + std::sin(a) * u * 0.8f),
                                    u * 0.2f, col, 8);
            }
            break;
        case game::kSleep:  // crescent moon
            dl->AddCircleFilled(c, u * 0.85f, col, 18);
            dl->AddCircleFilled(ImVec2(c.x + u * 0.42f, c.y - u * 0.3f), u * 0.72f, hole, 18);
            break;
        case game::kMadness: {  // eye
            const int n = 14;
            ImVec2 pts[2 * n];
            for (int k = 0; k < n; ++k) {
                const float t = static_cast<float>(k) / (n - 1);
                const float x = c.x - u + 2 * u * t;
                const float h = u * 0.62f * std::sin(t * 3.14159265f);
                pts[k] = ImVec2(x, c.y - h);
                pts[2 * n - 1 - k] = ImVec2(x, c.y + h);
            }
            dl->AddConvexPolyFilled(pts, 2 * n, col);
            dl->AddCircleFilled(c, u * 0.3f, hole, 10);
            break;
        }
        case game::kDeathBlight:  // skull
            dl->AddCircleFilled(ImVec2(c.x, c.y - u * 0.15f), u * 0.7f, col, 16);
            dl->AddRectFilled(ImVec2(c.x - u * 0.38f, c.y + u * 0.3f), ImVec2(c.x + u * 0.38f, c.y + u * 0.8f),
                              col, u * 0.1f);
            dl->AddCircleFilled(ImVec2(c.x - u * 0.28f, c.y - u * 0.15f), u * 0.17f, hole, 8);
            dl->AddCircleFilled(ImVec2(c.x + u * 0.28f, c.y - u * 0.15f), u * 0.17f, hole, 8);
            break;
    }
}

// Build-up icon: the symbol in its colour on a dark tile with a worn gold edge.
void StatusIcon(ImDrawList* dl, int status, ImVec2 c, float r, float alpha) {
    const ImU32 bg = Rgba(16, 14, 11, alpha * 0.85f);
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), bg, r * 0.25f);
    dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), WithAlpha(kFrameGold, alpha * 0.5f),
                r * 0.25f, 0, std::max(1.0f, r * 0.1f));
    StatusGlyph(dl, status, c, r * 0.62f, Shade(kStatusColor[status], 1.1f, alpha), bg);
}

// The status is in effect: a tile filled with its colour, the symbol dark on it, a slow glow, and
// the time left as a darkened sweep (clockwise from the top, like the game's cooldowns).
void ActiveIcon(ImDrawList* dl, int status, ImVec2 c, float r, float left, float total, float alpha,
                double now) {
    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(now * 4.0 + status));
    dl->AddRectFilled(ImVec2(c.x - r * 1.35f, c.y - r * 1.35f), ImVec2(c.x + r * 1.35f, c.y + r * 1.35f),
                      WithAlpha(kStatusColor[status], alpha * (0.10f + 0.12f * pulse)), r * 0.5f);
    const ImU32 fill = Shade(kStatusColor[status], 0.9f, alpha * 0.95f);
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fill, r * 0.25f);
    if (total > 0 && left > 0) {  // time used up so far, darkened
        const float used = 1.0f - std::clamp(left / total, 0.0f, 1.0f);
        if (used > 0.01f) {
            dl->PushClipRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), true);
            const float a0 = -1.5707963f, a1 = a0 + used * 6.2831853f;
            dl->PathLineTo(c);
            dl->PathArcTo(c, r * 1.5f, a0, a1, 24);
            dl->PathFillConvex(Rgba(0, 0, 0, alpha * 0.35f));
            dl->PopClipRect();
        }
    }
    dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                WithAlpha(kFrameGold, alpha * (0.65f + 0.3f * pulse)), r * 0.25f, 0, std::max(1.0f, r * 0.14f));
    StatusGlyph(dl, status, c, r * 0.62f, Rgba(22, 18, 14, alpha), fill);
}

// Under an enemy bar (`top` = centre of the block's top edge, `w` its width):
//  - a row of icons for statuses in effect (poisoned, rotting, asleep...);
//  - one row per status building up: icon on the left, bar on the right (like the game's meters).
void StatusBlock(ImDrawList* dl, ImVec2 top, float w, float h, const game::Chr& c,
                 const game::ActiveStatus& active, float alpha, double now) {
    const float icon = h * 2.4f;
    const float left = top.x - w * 0.5f;
    float y = top.y;
    if (active.mask) {
        const float r = icon * 0.62f;
        float x = left + r * 1.1f;
        for (int i = 0; i < game::kStatusCount; ++i) {
            if (!(active.mask & (1u << i))) continue;
            ActiveIcon(dl, i, ImVec2(x, y + r * 1.15f), r, active.left[i], active.total[i], alpha, now);
            x += r * 2.6f;
        }
        y += r * 2.3f + h * 0.6f;
    }
    for (int i = 0; i < game::kStatusCount; ++i) {
        if (c.resistance[i] <= 0 || c.buildup[i] <= 0) continue;
        const float ratio = std::clamp(static_cast<float>(c.buildup[i]) / c.resistance[i], 0.0f, 1.0f);
        StatusIcon(dl, i, ImVec2(left + icon * 0.5f, y + icon * 0.5f), icon * 0.5f, alpha);
        const ImVec2 a(left + icon + h * 0.8f, y + (icon - h) * 0.5f), b(top.x + w * 0.5f, a.y + h);
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), Rgba(0, 0, 0, alpha * 0.55f));
        dl->AddRectFilled(a, b, Rgba(26, 22, 17, alpha * 0.8f));
        if (ratio > 0) {
            const float x1 = a.x + (b.x - a.x) * ratio;
            const ImU32 dark = Shade(kStatusColor[i], 0.55f, alpha), lit = Shade(kStatusColor[i], 1.0f, alpha);
            dl->AddRectFilledMultiColor(a, ImVec2(x1, b.y), dark, lit, lit, dark);
        }
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), WithAlpha(kFrameGold, alpha * 0.35f));
        y += icon + h * 0.5f;
    }
}

// `origin`: top-left of the list, or its bottom-left when it grows upwards.
void DrawBuffs(ImDrawList* dl, const std::vector<buffs::Active>& list, float s, ImVec2 origin,
               bool grow_up) {
    const float row = 30 * s;
    const float w = 230 * s;
    float y = grow_up ? origin.y - row : origin.y;
    for (const auto& b : list) {
        const float ratio = b.duration > 0 ? b.remaining / b.duration : 0;
        const bool ending = b.remaining < 10.0f;
        const ImU32 text = ending ? Rgba(255, 150, 120, 1) : Rgba(238, 226, 200, 1);
        dl->AddRectFilled(ImVec2(origin.x - 6 * s, y - 3 * s), ImVec2(origin.x + w + 6 * s, y + row - 5 * s),
                          Rgba(10, 9, 8, 0.45f), 4 * s);
        Shadowed(dl, 17 * s, ImVec2(origin.x, y), text, b.name.c_str());
        Shadowed(dl, 17 * s, ImVec2(origin.x + w, y), text, FormatTime(b.remaining).c_str(), 1.0f);
        const float by = y + 20 * s;
        dl->AddRectFilled(ImVec2(origin.x, by), ImVec2(origin.x + w, by + 3 * s), Rgba(0, 0, 0, 0.5f));
        dl->AddRectFilled(ImVec2(origin.x, by), ImVec2(origin.x + w * ratio, by + 3 * s),
                          ending ? Rgba(230, 90, 60, 0.95f) : Rgba(214, 178, 96, 0.95f));
        y += grow_up ? -row : row;
    }
}

// Statuses in effect: the effect list is walked a few times per second, not every frame.
void RefreshActive(Track& t, double now) {
    if (now - t.active_read < 0.15) return;
    t.active_read = now;
    t.active = game::ReadActiveStatus(t.chr.ins);
}

// Centre of the focus bar.
ImVec2 FocusPos(ImVec2 display) {
    const auto& cfg = config::Get();
    return ImVec2(display.x * cfg.hud_focus_x / 100.0f, display.y * cfg.hud_focus_y / 100.0f);
}

ImVec2 BuffOrigin(ImVec2 display) {
    const auto& cfg = config::Get();
    return ImVec2(display.x * cfg.hud_buffs_x / 100.0f, display.y * cfg.hud_buffs_y / 100.0f);
}

void DrawFps(ImDrawList* dl, float s) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f FPS  %.1f ms", fps::CurrentFps(), fps::FrameTimeMs());
    Shadowed(dl, 16 * s, ImVec2(10 * s, 8 * s), Rgba(200, 235, 160, 0.95f), buf);
}

void DrawPause(ImDrawList* dl, float s, ImVec2 display) {
    const ImVec2 c(display.x * 0.5f, display.y * 0.30f);
    dl->AddRectFilled(ImVec2(0, c.y - 34 * s), ImVec2(display.x, c.y + 40 * s), Rgba(0, 0, 0, 0.45f));
    Shadowed(dl, 44 * s, ImVec2(c.x, c.y - 28 * s), Rgba(236, 206, 130, 1), i18n::T("PAUSED"), 0.5f);
    char hint[256];
    std::snprintf(hint, sizeof(hint), i18n::T("%s or R3 + Start to resume"),
                  config::Get().pause_key.c_str());
    Shadowed(dl, 18 * s, ImVec2(c.x, c.y + 18 * s), Rgba(220, 210, 190, 0.9f), hint, 0.5f);
}

}  // namespace

void Draw() {
    const auto& cfg = config::Get();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float s = display.y / 1080.0f * cfg.hud_scale;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const double now = Now();

    if (cfg.fps_counter) DrawFps(dl, display.y / 1080.0f);
    if (features::Paused()) DrawPause(dl, display.y / 1080.0f, display);

    if (!game::InGame()) {
        g_tracks.clear();
        g_player = Track{};
        return;
    }

    // ---- gather ----
    const bool want_enemies =
        cfg.hud_enemy_stagger || cfg.hud_focus_bar;  // HP numbers / status bars go on these
    game::Chr me;
    const bool have_me = game::ReadChr(game::PlayerIns(), me);
    if (have_me) UpdateTrack(g_player, me, now);
    if (want_enemies && have_me) {
        // Full scan of the character sets 4 times per second, only around the player (the
        // open-world set alone holds ~700 characters); tracked enemies are re-read every frame.
        static double last_scan = 0;
        if (now - last_scan > 0.25) {
            last_scan = now;
            game::ForEachChr(
                [&](const game::Chr& c) {
                    if (c.player || c.buddy || c.ins == me.ins) return;
                    UpdateTrack(g_tracks[c.ins], c, now);
                },
                me.pos, cfg.hud_range + 10.0f);
        } else {
            for (auto& [ins, t] : g_tracks) {
                game::Chr c;
                if (game::IsChr(ins) && game::ReadChr(ins, c)) {
                    c.player = t.chr.player;
                    c.buddy = t.chr.buddy;
                    UpdateTrack(t, c, now, false);
                }
            }
        }
        for (auto it = g_tracks.begin(); it != g_tracks.end();)
            it = now - it->second.last_seen > 0.8 ? g_tracks.erase(it) : std::next(it);
    }

    // ---- camera ----
    game::Camera cam;
    bool cam_ok = have_me && game::ReadCamera(cam);
    if (cam_ok) {
        const float d = Distance(cam.pos, me.pos);
        const float head[3] = {me.pos[0], me.pos[1] + 1.2f, me.pos[2]};
        const float rel[3] = {head[0] - cam.pos[0], head[1] - cam.pos[1], head[2] - cam.pos[2]};
        const float z = rel[0] * cam.forward[0] + rel[1] * cam.forward[1] + rel[2] * cam.forward[2];
        if (d < 40.0f) g_camfix.flip_votes = std::clamp(g_camfix.flip_votes + (z < 0 ? 1 : -1), -30, 30);
        g_camfix.space_ok = d < 60.0f;
        if (g_camfix.flip_votes > 0)
            for (float& f : cam.forward) f = -f;
        cam_ok = g_camfix.space_ok;
    }
    // The game renders at its own aspect ratio (black bars on other screens).
    const float vw = std::min(display.x, display.y * (cam_ok ? cam.aspect : 16.0f / 9.0f));
    const float vh = cam_ok ? vw / cam.aspect : display.y;
    const ImVec2 vo((display.x - vw) * 0.5f, (display.y - vh) * 0.5f);

    // ---- enemies ----
    Track* focus = nullptr;
    if (want_enemies) {
        for (auto& [ins, t] : g_tracks) {
            const game::Chr& c = t.chr;
            if (c.hp <= 0 || c.max_hp <= 1) continue;
            const bool in_range = Distance(c.pos, me.pos) <= cfg.hud_range;
            const double since = now - t.last_change;
            const bool damaged_poise = c.sa_max > 1.0f && c.sa >= 0.0f && c.sa < c.sa_max - 0.01f;
            const bool engaged = since < 8.0 || (damaged_poise && since < 60.0) ||
                                 now < t.broken_until;
            if (!engaged) continue;
            if (in_range && since < 20.0 && (!focus || t.last_change > focus->last_change))
                focus = &t;
            if (!cam_ok || !in_range) continue;
            const float top[3] = {c.pos[0], c.pos[1] + cfg.hud_bar_height, c.pos[2]};
            float scr[2];
            if (!game::WorldToScreen(cam, top, vw, vh, scr)) continue;
            const ImVec2 p(vo.x + scr[0], vo.y + scr[1]);
            const float fade = since < 6.0 ? 1.0f : std::max(0.35f, 1.0f - static_cast<float>(since - 6.0) / 10.0f);
            const float flash = now < t.broken_until ? static_cast<float>((t.broken_until - now) / 0.9) : 0.0f;
            if (!cfg.hud_enemy_stagger) continue;
            if (c.sa_max > 1) StanceBar(dl, p, 120 * s, 7 * s, t.shown, flash, fade);
            if (cfg.hud_enemy_hp) {
                const std::string hp = FormatHp(c.hp, c.max_hp);
                Shadowed(dl, 15 * s, ImVec2(p.x, p.y - 22 * s), Rgba(240, 225, 205, fade), hp.c_str(), 0.5f);
            }
            if (cfg.hud_status) {
                RefreshActive(t, now);
                StatusBlock(dl, ImVec2(p.x, p.y + 8 * s), 120 * s, 5 * s, c, t.active, fade, now);
            }
        }
    }
    if (focus && cfg.hud_focus_bar) {
        const game::Chr& c = focus->chr;
        const ImVec2 p = FocusPos(display);
        const float flash = now < focus->broken_until ? static_cast<float>((focus->broken_until - now) / 0.9) : 0.0f;
        if (c.sa_max > 1) StanceBar(dl, p, 520 * s, 11 * s, focus->shown, flash, 1.0f);
        if (cfg.hud_enemy_hp) {
            HpBar(dl, ImVec2(p.x, p.y + 15 * s), 520 * s, 4 * s,
                  c.max_hp > 0 ? static_cast<float>(c.hp) / c.max_hp : 0, 0.9f);
            char line[256];
            if (c.sa_max > 1)
                std::snprintf(line, sizeof(line), i18n::T("HP %s     Stance %.0f / %.0f"),
                              FormatHp(c.hp, c.max_hp).c_str(), c.sa > 0 ? c.sa : 0.0f, c.sa_max);
            else
                std::snprintf(line, sizeof(line), "HP %s", FormatHp(c.hp, c.max_hp).c_str());
            Shadowed(dl, 16 * s, ImVec2(p.x, p.y + 22 * s), Rgba(236, 224, 200, 0.95f), line, 0.5f);
        }
        if (cfg.hud_status) {
            RefreshActive(*focus, now);
            StatusBlock(dl, ImVec2(p.x, p.y + (cfg.hud_enemy_hp ? 46 : 14) * s), 300 * s, 7 * s, c,
                        focus->active, 1.0f, now);
        }
    }

    // ---- player ----
    if (cfg.hud_player_stagger && have_me && me.sa_max > 0) {
        const double since = now - g_player.last_change;
        const bool damaged = me.sa < me.sa_max - 0.01f;
        if (damaged || since < 2.5 || now < g_player.broken_until) {
            const float alpha = damaged ? 1.0f : static_cast<float>(1.0 - std::max(0.0, since - 1.5));
            const float flash = now < g_player.broken_until ? static_cast<float>((g_player.broken_until - now) / 0.9) : 0.0f;
            const ImVec2 pc(display.x * 0.5f, display.y * 0.955f);
            StanceBar(dl, pc, 300 * s, 8 * s, g_player.shown, flash, alpha);
            char poise[48];
            std::snprintf(poise, sizeof(poise), "%.0f / %.0f", me.sa > 0 ? me.sa : 0.0f, me.sa_max);
            Shadowed(dl, 14 * s, ImVec2(pc.x + 160 * s, pc.y - 9 * s), Rgba(236, 224, 200, alpha * 0.9f),
                     poise);
        }
    }

    // ---- buffs ----
    if (cfg.hud_buffs) {
        buffs::Update(cfg.hud_buffs_unnamed);
        DrawBuffs(dl, buffs::List(), s, BuffOrigin(display), cfg.hud_buffs_grow_up);
    }
}

void DrawPreview() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float s = display.y / 1080.0f * config::Get().hud_scale;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const auto& cfg = config::Get();
    game::Chr sample;  // an enemy building up blood loss and frostbite
    sample.resistance[game::kBloodLoss] = 300;
    sample.buildup[game::kBloodLoss] = 210;
    sample.resistance[game::kFrostbite] = 250;
    sample.buildup[game::kFrostbite] = 90;
    game::ActiveStatus sample_active;  // ... while poisoned
    sample_active.mask = 1u << game::kPoison;
    sample_active.left[game::kPoison] = 40;
    sample_active.total[game::kPoison] = 90;
    if (cfg.hud_focus_bar) {
        const ImVec2 f = FocusPos(display);
        StanceBar(dl, f, 520 * s, 11 * s, 0.72f, 0, 1);
        if (cfg.hud_enemy_hp) {
            HpBar(dl, ImVec2(f.x, f.y + 15 * s), 520 * s, 4 * s, 0.64f, 0.9f);
            Shadowed(dl, 16 * s, ImVec2(f.x, f.y + 22 * s), Rgba(236, 224, 200, 0.95f),
                     i18n::T("HP 3840 / 6000     Stance 34 / 120"), 0.5f);
        }
        if (cfg.hud_status)
            StatusBlock(dl, ImVec2(f.x, f.y + (cfg.hud_enemy_hp ? 46 : 14) * s), 300 * s, 7 * s, sample,
                        sample_active, 1.0f, Now());
    }
    const float xs[3] = {0.30f, 0.45f, 0.62f};
    const float rs[3] = {0.15f, 0.55f, 0.95f};
    for (int i = 0; i < 3 && cfg.hud_enemy_stagger; ++i) {
        const ImVec2 p(display.x * xs[i], display.y * 0.42f);
        StanceBar(dl, p, 120 * s, 7 * s, rs[i], i == 2 ? 0.5f : 0.0f, 1.0f);
        if (cfg.hud_enemy_hp)
            Shadowed(dl, 15 * s, ImVec2(p.x, p.y - 22 * s), Rgba(240, 225, 205, 1), "812 / 1200", 0.5f);
        if (i == 1 && cfg.hud_status)
            StatusBlock(dl, ImVec2(p.x, p.y + 8 * s), 120 * s, 5 * s, sample, sample_active, 1.0f, Now());
    }
    StanceBar(dl, ImVec2(display.x * 0.5f, display.y * 0.955f), 300 * s, 8 * s, 0.4f, 0, 1);
    auto name = [](const char* english) -> std::string {
        for (int i = 0; i < buffs::kBuffNameCount; ++i)
            if (std::strcmp(buffs::kBuffNames[i].name, english) == 0)
                return i18n::BuffName(i, english);
        return english;
    };
    std::vector<buffs::Active> list = {{0, name("Flame, Grant Me Strength"), 8.0f, 30.0f},
                                       {0, name("Golden Vow"), 52.0f, 80.0f},
                                       {0, name("Exalted Flesh"), 21.0f, 30.0f}};
    std::sort(list.begin(), list.end(),
              [](const buffs::Active& a, const buffs::Active& b) { return a.remaining < b.remaining; });
    DrawBuffs(dl, list, s, BuffOrigin(display), config::Get().hud_buffs_grow_up);
}

}  // namespace zz::hud
