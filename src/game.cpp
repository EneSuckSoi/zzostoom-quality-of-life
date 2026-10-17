#include "game.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log.h"
#include "memory.h"

namespace zz::game {

namespace {

struct SlotInfo {
    uintptr_t slot;
    uintptr_t runtime_class;
    std::string name;  // from the runtime class, else from the instance once it exists
};

std::mutex g_mutex;
std::vector<SlotInfo> g_slots;
std::map<std::string, uintptr_t> g_by_name;
bool g_initialized = false;
double g_last_resolve = 0;

// Versions verified by hand, used only when the name lookup finds nothing (eldenring.exe 2.7.1.0).
struct KnownSlot {
    const char* name;
    uintptr_t rva;
};
const KnownSlot kKnown2710[] = {
    {"WorldChrManImp", 0x3d69ff8},
    {"GameDataMan", 0x3d61f98},
    {"CSMenuManImp", 0x3d6f820},
};

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string ClassFromRuntimeClass(uintptr_t object) {
    std::string n = mem::RttiName((object + 7) & ~uintptr_t{7});
    const std::string prefix = "DLRuntimeClassImpl<";
    if (n.rfind(prefix, 0) != 0) return {};
    return n.substr(prefix.size(), n.size() - prefix.size() - 1);
}

// Fills names that are still unknown from the live instances. Caller holds g_mutex.
void ResolveLocked() {
    for (auto& s : g_slots) {
        if (!s.name.empty()) continue;
        uintptr_t inst = mem::ReadPtr(s.slot);
        if (!inst) continue;
        s.name = mem::RttiName(inst);
        if (!s.name.empty()) g_by_name.emplace(s.name, s.slot);
    }
    g_last_resolve = Now();
}

std::string ExeVersion() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return "?";
    std::vector<uint8_t> buf(size);
    if (!GetFileVersionInfoW(path, 0, size, buf.data())) return "?";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return "?";
    char out[64];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
                  LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                  LOWORD(info->dwFileVersionLS));
    return out;
}

}  // namespace

void Init() {
    const auto module = mem::MainModule();
    const std::string version = ExeVersion();
    const auto t0 = Now();
    std::vector<SlotInfo> slots;
    // mov reg,[rip+slot]; test reg,reg; jnz; lea rcx,[rip+runtime class]; call name;
    // mov r9,rax; lea r8,[fmt]; mov edx,line; lea rcx,[file]; call assert
    mem::FindAllPatterns(module,
                         "48 8B ?? ?? ?? ?? ?? 48 85 ?? 75 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                         "4C 8B C8 4C 8D 05 ?? ?? ?? ?? BA ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8",
                         [&](uintptr_t m) {
                             if ((*reinterpret_cast<const uint8_t*>(m + 2) & 0xC7) != 0x05)
                                 return true;
                             const uintptr_t slot = mem::ResolveRip(m, 3, 7);
                             const uintptr_t rc = mem::ResolveRip(m + 12, 3, 7);
                             for (auto& s : slots)
                                 if (s.slot == slot) return true;
                             slots.push_back({slot, rc, {}});
                             return true;
                         });
    std::lock_guard lock(g_mutex);
    g_slots = std::move(slots);
    for (auto& s : g_slots) {
        s.name = ClassFromRuntimeClass(s.runtime_class);
        if (!s.name.empty()) g_by_name.emplace(s.name, s.slot);
    }
    ResolveLocked();
    if (version == "2.7.1.0") {
        for (auto& k : kKnown2710) {
            auto it = g_by_name.find(k.name);
            if (it == g_by_name.end())
                g_by_name.emplace(k.name, module.base + k.rva);
            else if (it->second != module.base + k.rva)
                ZZ_LOG("  note: %s slot RVA 0x%llx (verified list says 0x%llx)", k.name,
                       static_cast<unsigned long long>(it->second - module.base),
                       static_cast<unsigned long long>(k.rva));
        }
    }
    g_initialized = true;
    ZZ_LOG("eldenring.exe %s: %zu singleton slots, %zu named (%.0f ms)", version.c_str(),
           g_slots.size(), g_by_name.size(), (Now() - t0) * 1000.0);
    for (const char* n : {"WorldChrManImp", "SoloParamRepositoryImp", "CSCameraImp", "FieldArea",
                          "CSFlipperImp", "CSMenuManImp", "GameDataMan", "CSFeManImp"}) {
        auto it = g_by_name.find(n);
        if (it == g_by_name.end())
            ZZ_LOG("  %s: not found", n);
        else
            ZZ_LOG("  %s: slot RVA 0x%llx", n,
                   static_cast<unsigned long long>(it->second - module.base));
    }
}

bool Initialized() { return g_initialized; }

uintptr_t SingletonSlot(const char* name) {
    std::lock_guard lock(g_mutex);
    auto it = g_by_name.find(name);
    if (it != g_by_name.end()) return it->second;
    if (g_initialized && Now() - g_last_resolve > 1.0) {
        ResolveLocked();
        it = g_by_name.find(name);
        if (it != g_by_name.end()) return it->second;
    }
    return 0;
}

uintptr_t Singleton(const char* name) {
    const uintptr_t slot = SingletonSlot(name);
    return slot ? mem::ReadPtr(slot) : 0;
}

std::string SingletonReport() {
    std::lock_guard lock(g_mutex);
    const auto base = mem::MainModule().base;
    std::string out;
    char line[128];
    for (const char* n : {"WorldChrManImp", "SoloParamRepositoryImp", "CSCameraImp", "FieldArea",
                          "CSFlipperImp", "CSMenuManImp"}) {
        auto it = g_by_name.find(n);
        if (it == g_by_name.end())
            std::snprintf(line, sizeof(line), "%s: not found\n", n);
        else
            std::snprintf(line, sizeof(line), "%s: RVA 0x%llx\n", n,
                          static_cast<unsigned long long>(it->second - base));
        out += line;
    }
    return out;
}

// ---- Characters ---------------------------------------------------------------------------

uintptr_t WorldChrMan() { return Singleton("WorldChrManImp"); }

uintptr_t PlayerIns() {
    uintptr_t wcm = WorldChrMan();
    return wcm ? mem::ReadPtr(wcm + off::kWcmPlayerIns) : 0;
}

uintptr_t ChrModule(uintptr_t chr, ptrdiff_t module_offset) {
    if (!chr) return 0;
    uintptr_t modules = mem::ReadPtr(chr + off::kModules);
    return modules ? mem::ReadPtr(modules + module_offset) : 0;
}

bool InGame() {
    uintptr_t data = ChrModule(PlayerIns(), off::kModData);
    if (!data) return false;
    auto max_hp = mem::Read<int32_t>(data + off::kDataMaxHp);
    return max_hp && *max_hp > 0 && *max_hp < 100000;
}

namespace {

enum class Kind : uint8_t { kUnknown, kPlayer, kNpc, kOther };

// RTTI lookups are cached per vtable: a few dozen classes, read every frame.
Kind KindOf(uintptr_t chr) {
    static std::mutex m;
    static std::unordered_map<uintptr_t, Kind> cache;
    const uintptr_t vt = mem::ReadPtr(chr);
    if (!vt) return Kind::kUnknown;
    std::lock_guard lock(m);
    auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    const std::string n = mem::RttiNameOfVtable(vt);
    // Only real characters (classes derived from ChrIns): the sets also hold effect objects
    // such as CSFD4WindForceSfxIns.
    Kind k = Kind::kOther;
    if (n == "PlayerIns")
        k = Kind::kPlayer;
    else if (mem::RttiHasBase(vt, "ChrIns"))
        k = Kind::kNpc;
    if (!n.empty()) cache.emplace(vt, k);
    return n.empty() ? Kind::kUnknown : k;
}

template <typename F>
void ForEachInSet(uintptr_t set, F&& fn) {
    const uint32_t capacity = mem::Read<uint32_t>(set + off::kChrSetCapacity).value_or(0);
    const uintptr_t entries = mem::ReadPtr(set + off::kChrSetEntries);
    if (!entries || capacity == 0 || capacity > 8192) return;
    // One read for the whole entry array instead of one per character (buffer reused).
    thread_local std::vector<uintptr_t> raw;
    raw.resize(capacity * 2);
    if (!mem::ReadRaw(entries, raw.data(), raw.size() * sizeof(uintptr_t))) return;
    for (uint32_t i = 0; i < capacity; ++i)
        if (raw[i * 2]) fn(raw[i * 2]);
}

uintptr_t TorrentIns() {
    uintptr_t wcm = WorldChrMan();
    uintptr_t player = PlayerIns();
    if (!wcm || !player) return 0;
    float pp[3];
    uintptr_t phys = ChrModule(player, off::kModPhysics);
    if (!phys || !mem::ReadRaw(phys + off::kPhysicsPosition, pp, sizeof(pp))) return 0;
    uintptr_t best = 0;
    float best_d = 1e30f;
    ForEachInSet(wcm + off::kWcmSummonBuddyChrSet, [&](uintptr_t chr) {
        const int id = mem::Read<int32_t>(chr + off::kCharacterId).value_or(0);
        const int npc = mem::Read<int32_t>(chr + off::kNpcId).value_or(0);
        if (!((id >= 8000 && id < 8100) || (npc >= 8000 && npc < 8100))) return;
        float tp[3];
        uintptr_t tphys = ChrModule(chr, off::kModPhysics);
        if (!tphys || !mem::ReadRaw(tphys + off::kPhysicsPosition, tp, sizeof(tp))) return;
        const float d = (tp[0] - pp[0]) * (tp[0] - pp[0]) + (tp[1] - pp[1]) * (tp[1] - pp[1]) +
                        (tp[2] - pp[2]) * (tp[2] - pp[2]);
        if (d < best_d) {
            best_d = d;
            best = chr;
        }
    });
    return best_d < 2.5f * 2.5f ? best : 0;
}

}  // namespace

bool Riding() { return InGame() && TorrentIns() != 0; }

bool ReadChr(uintptr_t ins, Chr& out) {
    out = Chr{};
    out.ins = ins;
    const uintptr_t modules = mem::ReadPtr(ins + off::kModules);
    if (!modules) return false;
    uintptr_t mods[0x70 / 8 + 1];
    if (!mem::ReadRaw(modules, mods, sizeof(mods))) return false;
    const uintptr_t data = mods[off::kModData / 8];
    const uintptr_t sa = mods[off::kModSuperArmor / 8];
    const uintptr_t phys = mods[off::kModPhysics / 8];
    if (!data || !phys) return false;
    int32_t hp[2];
    if (!mem::ReadRaw(data + off::kDataHp, hp, sizeof(hp))) return false;
    out.hp = hp[0];
    out.max_hp = hp[1];
    if (out.max_hp <= 0 || out.max_hp > 10000000) return false;
    if (sa) {
        float v[2];
        if (mem::ReadRaw(sa + off::kSuperArmorCurrent, v, sizeof(v))) {
            out.sa = v[0];
            out.sa_max = v[1];
        }
    }
    return mem::ReadRaw(phys + off::kPhysicsPosition, out.pos, sizeof(out.pos));
}

namespace {

// The character sets live inside WorldChrManImp: one ChrSet per loaded dungeon block, an
// OpenFieldChrSet for the open world, and the player / summon / ghost sets. (The pointer table
// at +0x10DC8 holds WorldAreaChr / WorldGridAreaChr objects, not sets.) Their offsets are found
// once by looking for embedded objects whose vtable has the RTTI name ChrSet / OpenFieldChrSet.
std::vector<ptrdiff_t> g_set_offsets;
uintptr_t g_sets_wcm = 0;
double g_sets_try = 0;
std::mutex g_sets_mutex;

void FindChrSets(uintptr_t wcm) {
    const auto module = mem::MainModule();
    std::unordered_map<uintptr_t, bool> is_set_vt;
    std::vector<uintptr_t> buf(0x1000 / 8);
    std::vector<ptrdiff_t> found;
    for (ptrdiff_t base = 0; base < 0x1F000; base += 0x1000) {
        if (!mem::ReadRaw(wcm + base, buf.data(), 0x1000)) break;
        for (size_t i = 0; i < buf.size(); ++i) {
            const uintptr_t v = buf[i];
            if (!module.Contains(v)) continue;
            auto it = is_set_vt.find(v);
            if (it == is_set_vt.end()) {
                const std::string n = mem::RttiNameOfVtable(v);
                it = is_set_vt.emplace(v, n == "ChrSet" || n == "OpenFieldChrSet").first;
            }
            if (it->second) found.push_back(base + static_cast<ptrdiff_t>(i * 8));
        }
    }
    g_set_offsets = std::move(found);
    g_sets_wcm = wcm;
    ZZ_LOG("Character sets: %zu found inside WorldChrManImp", g_set_offsets.size());
}

}  // namespace

void ForEachChr(const std::function<void(const Chr&)>& fn, const float* center, float radius) {
    const uintptr_t wcm = WorldChrMan();
    if (!wcm) return;
    std::vector<ptrdiff_t> offsets;
    {
        std::lock_guard lock(g_sets_mutex);
        if ((wcm != g_sets_wcm || g_set_offsets.empty()) && Now() - g_sets_try > 2.0) {
            g_sets_try = Now();
            FindChrSets(wcm);
        }
        offsets = g_set_offsets;
    }
    std::vector<uintptr_t> buddies;
    ForEachInSet(wcm + off::kWcmSummonBuddyChrSet, [&](uintptr_t c) { buddies.push_back(c); });
    std::unordered_set<uintptr_t> seen;
    const float r2 = radius * radius;
    for (ptrdiff_t o : offsets) {
        ForEachInSet(wcm + o, [&](uintptr_t ins) {
            if (!seen.insert(ins).second) return;
            const Kind kind = KindOf(ins);
            if (kind != Kind::kPlayer && kind != Kind::kNpc) return;
            if (center) {  // cheap position check before reading everything
                float p[3];
                const uintptr_t phys = ChrModule(ins, off::kModPhysics);
                if (!phys || !mem::ReadRaw(phys + off::kPhysicsPosition, p, sizeof(p))) return;
                const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
                if (dx * dx + dy * dy + dz * dz > r2) return;
            }
            Chr c;
            if (!ReadChr(ins, c)) return;
            c.player = kind == Kind::kPlayer;
            c.buddy = std::find(buddies.begin(), buddies.end(), ins) != buddies.end();
            fn(c);
        });
    }
}

bool IsChr(uintptr_t ins) {
    const Kind k = KindOf(ins);
    return k == Kind::kPlayer || k == Kind::kNpc;
}

int PlayerCount() {
    // Players are all in the player set (+0x10EE0 in 1.17.1); counting through every set is
    // simpler than relying on that offset and only runs while paused.
    // NPC phantoms (invaders, summoned NPCs, mimic-like copies) are PlayerIns too, but they carry
    // an NpcParam id; real players don't.
    const uintptr_t local = PlayerIns();
    int n = local ? 1 : 0;
    ForEachChr([&](const Chr& c) {
        if (!c.player || c.hp <= 0 || c.ins == local) return;
        const int npc = mem::Read<int32_t>(c.ins + off::kNpcId).value_or(0);
        if (npc > 0) return;
        ++n;
    });
    return n;
}

// ---- Camera -------------------------------------------------------------------------------

namespace {

struct CamSource {
    uintptr_t cam = 0;  // CSPersCam-derived object
    double found_at = 0;
};
CamSource g_cam;

bool LooksLikeCamera(uintptr_t cam, Camera* out) {
    float m[16 + 2];
    if (!mem::ReadRaw(cam + 0x10, m, sizeof(m))) return false;
    for (int r = 0; r < 3; ++r) {
        const float len = std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] +
                                    m[r * 4 + 2] * m[r * 4 + 2]);
        if (!(len > 0.95f && len < 1.05f)) return false;
    }
    const float fov = m[16], aspect = m[17];
    if (!(fov > 0.15f && fov < 2.8f) || !(aspect > 0.4f && aspect < 6.0f)) return false;
    if (out) {
        for (int i = 0; i < 3; ++i) {
            out->right[i] = m[0 + i];
            out->up[i] = m[4 + i];
            out->forward[i] = m[8 + i];
            out->pos[i] = m[12 + i];
        }
        out->fov_y = fov;
        out->aspect = aspect;
    }
    return true;
}


// Scans the executable's .data for a pointer to a live object of class `name` (for objects that
// are not reached through the singleton null check). Pointers are checked with VirtualQuery
// (cached per region) so that random values never fault.
uintptr_t FindStaticInstance(const char* name) {
    static std::map<std::string, double> last_try;
    double& t = last_try[name];
    if (Now() - t < 5.0) return 0;
    t = Now();
    const auto module = mem::MainModule();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    std::vector<std::pair<uintptr_t, uintptr_t>> readable;  // [start, end)
    auto is_readable = [&](uintptr_t a) {
        for (auto& [s0, s1] : readable)
            if (a >= s0 && a < s1) return true;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
        const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE |
                                        PAGE_WRITECOPY));
        if (ok) {
            const auto b0 = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            readable.emplace_back(b0, b0 + mbi.RegionSize);
        }
        return ok;
    };
    std::unordered_map<uintptr_t, bool> vt_match;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".data", 6) != 0) continue;
        auto* p = reinterpret_cast<const uintptr_t*>(module.base + sec->VirtualAddress);
        const size_t n = sec->Misc.VirtualSize / 8;
        for (size_t k = 0; k < n; ++k) {
            const uintptr_t v = p[k];
            if (v < 0x10000 || v > 0x7FFFFFFF0000ull || (v & 7) || module.Contains(v)) continue;
            if (!is_readable(v)) continue;
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(v);
            if (!module.Contains(vt)) continue;
            auto it = vt_match.find(vt);
            if (it == vt_match.end()) it = vt_match.emplace(vt, mem::RttiNameOfVtable(vt) == name).first;
            if (it->second) {
                ZZ_LOG("%s found through .data RVA 0x%llx", name,
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&p[k]) - module.base));
                return v;
            }
        }
    }
    ZZ_LOG("%s not found in .data", name);
    return 0;
}

}  // namespace

bool ReadCamera(Camera& out) {
    const double now = Now();
    // Re-pick once per second: during loading screens CSCameraImp's first camera is a menu
    // camera at the origin; in game the camera rendering the world sits next to the player.
    if (now - g_cam.found_at > 1.0) {
        g_cam.found_at = now;
        g_cam.cam = 0;
        float player[3];
        const uintptr_t phys = ChrModule(PlayerIns(), off::kModPhysics);
        const bool have_player = phys && mem::ReadRaw(phys + off::kPhysicsPosition, player, sizeof(player));
        uintptr_t root = Singleton("CSCameraImp");
        if (!root) root = FindStaticInstance("CSCameraImp");
        float best = 1e30f;
        for (int i = 1; root && i < 8; ++i) {
            const uintptr_t p = mem::ReadPtr(root + i * 8);
            Camera c;
            if (!p || !LooksLikeCamera(p, &c)) continue;
            float d = 0;
            if (have_player) {
                const float dx = c.pos[0] - player[0], dy = c.pos[1] - player[1], dz = c.pos[2] - player[2];
                d = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            // The first camera (pers_cam_1) wins when it is near the player.
            if (d < 25.0f && (i == 1 || d < best)) {
                if (i == 1) {
                    g_cam.cam = p;
                    break;
                }
                best = d;
                g_cam.cam = p;
            }
        }
        static uintptr_t logged = 0;
        if (g_cam.cam && g_cam.cam != logged) {
            logged = g_cam.cam;
            ZZ_LOG("Render camera: %s (%s)", root ? "CSCameraImp" : "?", mem::RttiName(g_cam.cam).c_str());
        }
    }
    return g_cam.cam && LooksLikeCamera(g_cam.cam, &out);
}

bool WorldToScreen(const Camera& cam, const float p[3], float width, float height, float out[2]) {
    const float d[3] = {p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2]};
    auto dot = [&](const float* a) { return d[0] * a[0] + d[1] * a[1] + d[2] * a[2]; };
    const float z = dot(cam.forward);
    if (z < 0.1f) return false;
    const float tan_y = std::tan(cam.fov_y * 0.5f);
    const float tan_x = tan_y * cam.aspect;
    const float nx = dot(cam.right) / (z * tan_x);
    const float ny = dot(cam.up) / (z * tan_y);
    out[0] = (nx + 1.0f) * 0.5f * width;
    out[1] = (1.0f - ny) * 0.5f * height;
    return nx > -1.3f && nx < 1.3f && ny > -1.3f && ny < 1.3f;
}

}  // namespace zz::game
