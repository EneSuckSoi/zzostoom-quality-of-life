#include "params.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "config.h"
#include "game.h"
#include "i18n.h"
#include "log.h"
#include "memory.h"

namespace zz::params {

uintptr_t Table::RowData(int index) const {
    // Row table after the 0x40-byte header: {i32 id; pad; i64 data offset; i64 name offset}
    const auto off = mem::Read<int64_t>(header + 0x40 + index * 0x18 + 8).value_or(0);
    return off > 0 ? header + static_cast<uintptr_t>(off) : 0;
}

int32_t Table::RowId(int index) const {
    return mem::Read<int32_t>(header + 0x40 + index * 0x18).value_or(-1);
}

uintptr_t Table::Find(int32_t id) const {
    for (int i = 0; i < rows; ++i)
        if (RowId(i) == id) return RowData(i);
    return 0;
}

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A PARAM file: row count at 0x0A, offset of the struct name at 0x10 (64-bit layout).
bool ValidHeader(uintptr_t h, const char* type, uint16_t* rows_out) {
    uint8_t hdr[0x58];
    if (!mem::ReadRaw(h, hdr, sizeof(hdr))) return false;
    uint16_t rows;
    uint64_t type_off;
    int64_t first_row;
    std::memcpy(&rows, hdr + 0x0A, 2);
    std::memcpy(&type_off, hdr + 0x10, 8);
    std::memcpy(&first_row, hdr + 0x48, 8);
    if (rows == 0 || rows > 60000 || type_off < 0x40 || type_off > (64u << 20)) return false;
    if (first_row < 0x40 || static_cast<uint64_t>(first_row) > type_off) return false;
    const size_t len = std::strlen(type) + 1;
    char name[64] = {};
    if (len > sizeof(name) || !mem::ReadRaw(h + type_off, name, len)) return false;
    if (std::memcmp(name, type, len) != 0) return false;
    if (rows_out) *rows_out = rows;
    return true;
}

// FD4ResCap name: a DLWString somewhere in the first bytes (inline buffer or heap pointer).
bool ResCapHasName(uintptr_t cap, const wchar_t* name) {
    const size_t len = std::wcslen(name);
    wchar_t buf[64];
    if (len + 1 > std::size(buf)) return false;
    for (int o = 8; o < 0x80; o += 8) {
        if (mem::ReadRaw(cap + o, buf, (len + 1) * sizeof(wchar_t)) &&
            std::wmemcmp(buf, name, len + 1) == 0)
            return true;
        const uintptr_t p = mem::ReadPtr(cap + o);
        if (p && mem::ReadRaw(p, buf, (len + 1) * sizeof(wchar_t)) &&
            std::wmemcmp(buf, name, len + 1) == 0)
            return true;
    }
    return false;
}

// The param file is referenced from the ResCap directly or through one more object.
uintptr_t ParamFileOf(uintptr_t cap, const char* type, uint16_t* rows) {
    for (int o = 8; o < 0x100; o += 8) {
        const uintptr_t p = mem::ReadPtr(cap + o);
        if (!p) continue;
        if (ValidHeader(p, type, rows)) return p;
    }
    for (int o = 8; o < 0x100; o += 8) {
        const uintptr_t p = mem::ReadPtr(cap + o);
        if (!p || p == cap) continue;
        for (int k = 0; k < 0x100; k += 8) {
            const uintptr_t q = mem::ReadPtr(p + k);
            if (q && ValidHeader(q, type, rows)) return q;
        }
    }
    return 0;
}

std::mutex g_mutex;
std::map<std::wstring, Table> g_cache;
std::map<std::wstring, double> g_last_try;
uintptr_t g_rescap_vtable = 0;

bool IsResCap(uintptr_t p) {
    const uintptr_t vt = mem::ReadPtr(p);
    if (!vt) return false;
    if (g_rescap_vtable) return vt == g_rescap_vtable;
    if (mem::RttiNameOfVtable(vt) != "ParamResCap") return false;
    g_rescap_vtable = vt;
    return true;
}

bool Search(const wchar_t* name, const char* type, Table& out) {
    const uintptr_t repo = game::Singleton("SoloParamRepositoryImp");
    if (!repo) return false;
    // The repository holds its ResCaps in fixed-size holder entries; scan its pointer fields
    // (and one level below) for ParamResCap objects with the right name.
    std::vector<uintptr_t> fields(0x8000 / 8);
    size_t n = 0;
    for (; n < fields.size(); n += 0x200 / 8)
        if (!mem::ReadRaw(repo + n * 8, &fields[n], 0x200)) break;
    fields.resize(n);
    auto try_cap = [&](uintptr_t cap) -> bool {
        if (!IsResCap(cap) || !ResCapHasName(cap, name)) return false;
        uint16_t rows = 0;
        const uintptr_t file = ParamFileOf(cap, type, &rows);
        if (!file) {
            ZZ_LOG("Param %ls: ResCap %p found but no %s file below it", name,
                   reinterpret_cast<void*>(cap), type);
            return false;
        }
        out.header = file;
        out.rows = rows;
        return true;
    };
    for (size_t i = 1; i < fields.size(); ++i)
        if (fields[i] && try_cap(fields[i])) {
            ZZ_LOG("Param %ls: repository+0x%zx, %u rows", name, i * 8, out.rows);
            return true;
        }
    for (size_t i = 1; i < fields.size(); ++i) {
        const uintptr_t a = fields[i];
        if (!a || IsResCap(a)) continue;
        for (int k = 0; k < 0x48; k += 8) {
            const uintptr_t cap = mem::ReadPtr(a + k);
            if (cap && try_cap(cap)) {
                ZZ_LOG("Param %ls: [repository+0x%zx]+0x%x, %u rows", name, i * 8, k, out.rows);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

bool Find(const wchar_t* name, const char* type, Table& out) {
    std::lock_guard lock(g_mutex);
    auto it = g_cache.find(name);
    if (it != g_cache.end()) {
        if (ValidHeader(it->second.header, type, nullptr)) {
            out = it->second;
            return true;
        }
        g_cache.erase(it);
    }
    double& last = g_last_try[name];
    if (Now() - last < 3.0) return false;
    last = Now();
    Table t;
    if (!Search(name, type, t)) return false;
    g_cache[name] = t;
    out = t;
    return true;
}

// ---- Camera -------------------------------------------------------------------------------

namespace {

// LockCamParam (0x80 bytes per row), the fields we touch:
constexpr int kCamDist = 0;        // camDistTarget [m]
constexpr int kLockTilt = 2;       // lockRotXShiftRatio
constexpr int kPivotHeight = 3;    // chrOrgOffset_Y [m]
constexpr int kLockRange = 4;      // chrLockRangeMaxRadius [m]
constexpr int kFovY = 5;           // camFovY [deg]
constexpr int kLockRangeD = 6;     // chrLockRangeMaxRadius_forD (-1 = unused)
constexpr int kLockRangePD = 7;    // chrLockRangeMaxRadius_forPD (-1 = unused)
constexpr int kFields = 8;

struct CamRow {
    uintptr_t data;
    float orig[kFields];
};

struct CamState {
    uintptr_t header = 0;
    std::vector<CamRow> rows;
    float applied[6] = {-1, -1, -1, -1, -1, -1};
    bool applied_enabled = false;
    double last_check = 0;
    std::string status = "waiting for the game";
    bool status_values = false;  // status is "applied / restored" + the numbers below
    float status_dist = 0, status_fov = 0;
} g_cam;

void Capture(const Table& t) {
    g_cam.rows.clear();
    g_cam.header = t.header;
    for (int i = 0; i < t.rows; ++i) {
        CamRow r{t.RowData(i), {}};
        if (r.data && mem::ReadRaw(r.data, r.orig, sizeof(r.orig))) g_cam.rows.push_back(r);
    }
    g_cam.applied_enabled = false;
    std::fill(std::begin(g_cam.applied), std::end(g_cam.applied), -1.0f);
    ZZ_LOG("LockCamParam: %zu rows captured (row 0: distance %.2f, fov %.1f, height %.2f, lock "
           "range %.1f)",
           g_cam.rows.size(), g_cam.rows.empty() ? 0.f : g_cam.rows[0].orig[kCamDist],
           g_cam.rows.empty() ? 0.f : g_cam.rows[0].orig[kFovY],
           g_cam.rows.empty() ? 0.f : g_cam.rows[0].orig[kPivotHeight],
           g_cam.rows.empty() ? 0.f : g_cam.rows[0].orig[kLockRange]);
}

}  // namespace

bool ApplyCamera() {
    const auto& cfg = config::Get();
    const double now = Now();
    if (!g_cam.header || now - g_cam.last_check > 2.0) {
        g_cam.last_check = now;
        Table t;
        if (!Find(L"LockCamParam", "LOCK_CAM_PARAM_ST", t)) {
            if (g_cam.header) ZZ_LOG("LockCamParam lost");
            g_cam.header = 0;
            g_cam.status = "camera table not found yet";
            g_cam.status_values = false;
            return false;
        }
        if (t.header != g_cam.header) Capture(t);
    }
    if (g_cam.rows.empty()) return false;

    const float want[6] = {cfg.cam_distance, cfg.cam_fov, cfg.cam_height, cfg.lock_range,
                           cfg.lock_tilt, 0};
    if (cfg.camera == g_cam.applied_enabled &&
        (!cfg.camera || std::equal(std::begin(want), std::end(want), std::begin(g_cam.applied))))
        return true;

    for (const CamRow& r : g_cam.rows) {
        float v[kFields];
        std::memcpy(v, r.orig, sizeof(v));
        if (cfg.camera) {
            v[kCamDist] = r.orig[kCamDist] * cfg.cam_distance;
            v[kFovY] = std::clamp(r.orig[kFovY] + cfg.cam_fov, 10.0f, 130.0f);
            v[kPivotHeight] = r.orig[kPivotHeight] + cfg.cam_height;
            v[kLockTilt] = r.orig[kLockTilt] * cfg.lock_tilt;
            v[kLockRange] = r.orig[kLockRange] * cfg.lock_range;
            if (r.orig[kLockRangeD] > 0) v[kLockRangeD] = r.orig[kLockRangeD] * cfg.lock_range;
            if (r.orig[kLockRangePD] > 0) v[kLockRangePD] = r.orig[kLockRangePD] * cfg.lock_range;
        }
        mem::WriteRaw(r.data, v, sizeof(v));
    }
    std::copy(std::begin(want), std::end(want), std::begin(g_cam.applied));
    g_cam.applied_enabled = cfg.camera;
    g_cam.status = cfg.camera ? "applied" : "restored";
    g_cam.status_values = true;
    g_cam.status_dist = g_cam.rows[0].orig[kCamDist] * (cfg.camera ? cfg.cam_distance : 1.0f);
    g_cam.status_fov =
        std::clamp(g_cam.rows[0].orig[kFovY] + (cfg.camera ? cfg.cam_fov : 0.0f), 10.0f, 130.0f);
    static double last_log = 0;  // a slider being dragged applies every frame
    if (now - last_log > 2.0) {
        last_log = now;
        ZZ_LOG("Camera %s: %zu rows (normal camera: %.1f m, FOV %.0f)", g_cam.status.c_str(),
               g_cam.rows.size(), g_cam.status_dist, g_cam.status_fov);
    }
    return true;
}

std::string CameraStatus() {
    if (!g_cam.status_values) return i18n::T(g_cam.status.c_str());
    char buf[200];
    std::snprintf(buf, sizeof(buf), i18n::T("%s: %d camera rows (normal camera: %.1f m, FOV %.0f)"),
                  i18n::T(g_cam.status.c_str()), static_cast<int>(g_cam.rows.size()),
                  g_cam.status_dist, g_cam.status_fov);
    return buf;
}

bool CameraApplied() { return g_cam.status_values && g_cam.status == "applied"; }

}  // namespace zz::params
