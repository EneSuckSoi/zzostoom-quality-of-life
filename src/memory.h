#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace zz::mem {

struct Module {
    uintptr_t base = 0;
    size_t size = 0;
    bool Contains(uintptr_t a) const { return a >= base && a < base + size; }
};

// Installs the exception handler that makes Read/Write fault-tolerant. Call once at start.
void Init();

Module MainModule();

// Reads that hit invalid memory so far (each one costs an exception, i.e. microseconds).
uint64_t FaultCount();

// IDA style pattern, e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0". Only executable sections are scanned.
uintptr_t FindPattern(const Module& module, const char* pattern);
// Calls `hit` for every match; stop early by returning false.
void FindAllPatterns(const Module& module, const char* pattern,
                     const std::function<bool(uintptr_t)>& hit);

// Target of a RIP-relative operand: insn + insn_len + *(int32_t*)(insn + disp_offset).
uintptr_t ResolveRip(uintptr_t insn, int disp_offset, int insn_len);

// Reads/writes that never crash: invalid addresses just return false.
bool ReadRaw(uintptr_t address, void* out, size_t size);
bool WriteRaw(uintptr_t address, const void* data, size_t size);

template <typename T>
std::optional<T> Read(uintptr_t address) {
    T value{};
    if (address && ReadRaw(address, &value, sizeof(T))) return value;
    return std::nullopt;
}

template <typename T>
bool Write(uintptr_t address, const T& value) {
    return address && WriteRaw(address, &value, sizeof(T));
}

// Returns the pointer stored at `address`, or 0.
uintptr_t ReadPtr(uintptr_t address);

// Overwrites code bytes (VirtualProtect + instruction cache flush). Not for per-frame use.
bool PatchCode(uintptr_t address, const void* data, size_t size);

// Class name from MSVC RTTI of a polymorphic object in the game, e.g. "WorldChrManImp" (the
// namespace suffix is dropped). Empty if `object` has no vtable inside the game executable.
std::string RttiName(uintptr_t object);
// Same, starting from a vtable address.
std::string RttiNameOfVtable(uintptr_t vtable);
// True if the class of `vtable` is `base` or derives from it (RTTI class hierarchy).
bool RttiHasBase(uintptr_t vtable, const char* base);

}  // namespace zz::mem
