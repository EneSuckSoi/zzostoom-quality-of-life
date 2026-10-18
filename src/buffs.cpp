#include "buffs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

#include "buff_db.h"
#include "game.h"
#include "i18n.h"
#include "log.h"
#include "memory.h"
#include "params.h"

namespace zz::buffs {

namespace {

// The player's SpEffect list (CS::SpecialEffect, a linked list of entries {SpEffectParam row*,
// id, ...}). Field offsets are found at run time and checked against the parameter table:
// an entry is valid only if its row pointer is exactly the table's row for its id.
struct Layout {
    int chr_off = -1;        // ChrIns -> SpecialEffect*
    int head_off = -1;       // SpecialEffect -> first entry
    int next_off = -1;       // entry -> next entry
    int remaining_off = 0x44;  // entry -> seconds left (float); verified in 1.17.1, re-learned if implausible
    int bad_reads = 0;
} g_layout;

params::Table g_table;
std::unordered_map<uintptr_t, int32_t> g_row_to_id;
double g_index_try = 0;

std::vector<Active> g_list;
std::string g_status = "waiting for the game";
double g_last_update = 0;

struct Seen {
    double first_seen;
    float floats[36];  // entry +0x10..+0xA0, for finding the "time left" field
    double sample_time;
};
std::unordered_map<uintptr_t, Seen> g_seen;
std::map<int, int> g_votes;

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool BuildIndex() {
    if (!g_row_to_id.empty()) return true;
    if (Now() - g_index_try < 3.0) return false;
    g_index_try = Now();
    if (!params::Find(L"SpEffectParam", "SP_EFFECT_PARAM_ST", g_table)) return false;
    struct RowHeader {
        int32_t id;
        int32_t pad;
        int64_t data;
        int64_t name;
    };
    std::vector<RowHeader> rows(g_table.rows);
    if (!mem::ReadRaw(g_table.header + 0x40, rows.data(), rows.size() * sizeof(RowHeader)))
        return false;
    for (auto& r : rows)
        if (r.data > 0) g_row_to_id[g_table.header + static_cast<uintptr_t>(r.data)] = r.id;
    ZZ_LOG("Buffs: SpEffectParam indexed (%zu rows)", g_row_to_id.size());
    return true;
}

bool ValidEntry(uintptr_t e, int32_t* id_out = nullptr) {
    if (!e) return false;
    uintptr_t head[2];
    if (!mem::ReadRaw(e, head, sizeof(head))) return false;
    auto it = g_row_to_id.find(head[0]);
    if (it == g_row_to_id.end() || it->second != static_cast<int32_t>(head[1] & 0xFFFFFFFF))
        return false;
    if (id_out) *id_out = it->second;
    return true;
}

uintptr_t SpecialEffectOf(uintptr_t chr) {
    if (g_layout.chr_off >= 0) return mem::ReadPtr(chr + g_layout.chr_off);
    for (int o = 0x100; o < 0x400; o += 8) {
        const uintptr_t p = mem::ReadPtr(chr + o);
        if (p && mem::RttiName(p).find("SpecialEffect") != std::string::npos) {
            g_layout.chr_off = o;
            ZZ_LOG("Buffs: ChrIns+0x%x -> %s", o, mem::RttiName(p).c_str());
            return p;
        }
    }
    return 0;
}

uintptr_t HeadOf(uintptr_t se) {
    if (g_layout.head_off >= 0) return mem::ReadPtr(se + g_layout.head_off);
    for (int o = 8; o < 0x60; o += 8) {
        const uintptr_t p = mem::ReadPtr(se + o);
        if (ValidEntry(p)) {
            g_layout.head_off = o;
            ZZ_LOG("Buffs: SpecialEffect+0x%x -> first entry", o);
            return p;
        }
    }
    return 0;
}

int ChainLength(uintptr_t head, int next_off) {
    int n = 0;
    uintptr_t e = head;
    while (e && n < 512 && ValidEntry(e)) {
        ++n;
        const uintptr_t next = mem::ReadPtr(e + next_off);
        if (next == head) break;
        e = next;
    }
    return n;
}

void FindNext(uintptr_t head) {
    int best = -1, best_len = 1;
    for (int o = 0x10; o < 0x80; o += 8) {
        const int len = ChainLength(head, o);
        if (len > best_len) {
            best_len = len;
            best = o;
        }
    }
    if (best >= 0) {
        g_layout.next_off = best;
        ZZ_LOG("Buffs: entry+0x%x -> next (%d effects on the player)", best, best_len);
    }
}

// Looks for a float that falls by one per second in every timed entry: the time left.
void Calibrate(uintptr_t e, float duration, double now) {
    auto it = g_seen.find(e);
    if (it == g_seen.end()) return;
    Seen& s = it->second;
    const double dt = now - s.sample_time;
    if (dt < 0.9) return;
    float cur[36];
    if (mem::ReadRaw(e + 0x10, cur, sizeof(cur))) {
        for (int k = 0; k < 36; ++k) {
            const float v0 = s.floats[k], v1 = cur[k];
            if (!(v0 > 0 && v1 > 0 && v0 <= duration + 0.5f)) continue;
            if (std::fabs((v0 - v1) - static_cast<float>(dt)) < 0.08f * static_cast<float>(dt))
                ++g_votes[0x10 + k * 4];
        }
        std::memcpy(s.floats, cur, sizeof(cur));
    }
    s.sample_time = now;
    int best = -1, best_votes = 0, total = 0;
    for (auto [off, v] : g_votes) {
        total += v;
        if (v > best_votes) {
            best_votes = v;
            best = off;
        }
    }
    if (best_votes >= 4 && best_votes * 2 > total) {
        g_layout.remaining_off = best;
        ZZ_LOG("Buffs: entry+0x%x = seconds left (%d votes)", best, best_votes);
    }
}

// Index in kBuffNames, -1 if the effect has no known name.
int NameIndex(int id) {
    int lo = 0, hi = kBuffNameCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (kBuffNames[mid].id == id) return mid;
        if (kBuffNames[mid].id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

}  // namespace

void Update(bool show_unnamed) {
    const double now = Now();
    if (now - g_last_update < 0.1) return;
    g_last_update = now;
    g_list.clear();
    const uintptr_t player = game::PlayerIns();
    if (!player || !game::InGame()) {
        g_seen.clear();
        return;
    }
    if (!BuildIndex()) {
        g_status = "effect table not found yet";
        return;
    }
    const uintptr_t se = SpecialEffectOf(player);
    const uintptr_t head = se ? HeadOf(se) : 0;
    if (!head) {
        g_status = se ? "effect list empty" : "effect list not found";
        return;
    }
    if (g_layout.next_off < 0) {
        FindNext(head);
        if (g_layout.next_off < 0) {
            g_status = "learning the effect list layout";
            return;
        }
    }
    std::unordered_map<uintptr_t, Seen> seen;
    int n = 0;
    for (uintptr_t e = head; e && n < 512; e = mem::ReadPtr(e + g_layout.next_off), ++n) {
        int32_t id;
        if (!ValidEntry(e, &id)) break;
        const uintptr_t row = mem::ReadPtr(e);
        int32_t icon = -1;
        float duration = 0;
        mem::ReadRaw(row, &icon, sizeof(icon));
        mem::ReadRaw(row + 8, &duration, sizeof(duration));
        // Sub-second effects with an icon are internal helpers (Golden Vow has a 0.1 s one).
        // Some weapon skill buffs have no icon in the game (Seppuku): known names are kept.
        if (duration < 1.0f) continue;
        const int name_index = NameIndex(id);
        if (icon <= 0 && name_index < 0) continue;
        if (name_index < 0 && !show_unnamed) continue;

        auto old = g_seen.find(e);
        Seen s;
        if (old != g_seen.end()) {
            s = old->second;
        } else {
            s.first_seen = now;
            s.sample_time = now;
            mem::ReadRaw(e + 0x10, s.floats, sizeof(s.floats));
        }
        seen[e] = s;
        if (g_layout.remaining_off < 0) Calibrate(e, duration, now);

        Active a;
        a.id = id;
        a.name = name_index >= 0
                     ? i18n::BuffName(name_index, kBuffNames[name_index].name)
                     : i18n::T("Effect") + std::string(" ") + std::to_string(id);
        a.duration = duration;
        if (g_layout.remaining_off >= 0) {
            const float left = mem::Read<float>(e + g_layout.remaining_off).value_or(-1e9f);
            if (!(left > -5.0f && left <= duration + 1.0f) && ++g_layout.bad_reads > 20) {
                ZZ_LOG("Buffs: entry+0x%x doesn't look like the time left (%.2f of %.1f), re-learning",
                       g_layout.remaining_off, left, duration);
                g_layout.remaining_off = -1;
                g_votes.clear();
            }
            a.remaining = left;
        } else {
            a.remaining = duration - static_cast<float>(now - s.first_seen);
        }
        if (!(a.remaining > 0.05f)) continue;  // expired (the entry can linger for a moment)
        a.remaining = std::min(a.remaining, duration);
        // One line per buff name: casting Golden Vow twice refreshes, it doesn't stack lines.
        auto dup = std::find_if(g_list.begin(), g_list.end(),
                                [&](const Active& x) { return x.name == a.name; });
        if (dup != g_list.end()) {
            if (a.remaining > dup->remaining) *dup = a;
            continue;
        }
        g_list.push_back(a);
    }
    // Calibration samples survive for entries still present.
    for (auto& [e, s] : seen) {
        auto it = g_seen.find(e);
        if (it != g_seen.end()) s = it->second;
    }
    g_seen = std::move(seen);
    std::sort(g_list.begin(), g_list.end(),
              [](const Active& a, const Active& b) { return a.remaining < b.remaining; });
    g_status = g_layout.remaining_off >= 0 ? "ok" : "ok (estimating time left)";
}

const std::vector<Active>& List() { return g_list; }
std::string Status() { return g_status; }

}  // namespace zz::buffs
