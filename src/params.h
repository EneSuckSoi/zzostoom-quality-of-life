#pragma once
#include <cstdint>
#include <string>

// Game parameter tables (regulation.bin) as loaded in memory, edited in place. Nothing is
// written to disk; the original values are kept so every change can be undone.
namespace zz::params {

struct Table {
    uintptr_t header = 0;  // PARAM file header in memory
    uint16_t rows = 0;
    uintptr_t RowData(int index) const;
    int32_t RowId(int index) const;
    uintptr_t Find(int32_t id) const;  // row data of `id`, 0 if missing
};

// Looks the table up through SoloParamRepository (cached once found). `name` is the file name
// without extension ("LockCamParam"), `type` the struct name stored in the file
// ("LOCK_CAM_PARAM_ST").
bool Find(const wchar_t* name, const char* type, Table& out);

// Camera settings from the config, applied to every LockCamParam row. Safe to call every frame
// (only writes when something changed). Returns false while the table isn't available.
bool ApplyCamera();
std::string CameraStatus();  // in the menu language
bool CameraApplied();

}  // namespace zz::params
