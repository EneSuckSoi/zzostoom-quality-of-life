#include "memory.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace zz::mem {

Module MainModule() {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return {base, nt->OptionalHeader.SizeOfImage};
}

namespace {

struct PatternByte {
    uint8_t value;
    bool wildcard;
};

std::vector<PatternByte> ParsePattern(const char* pattern) {
    std::vector<PatternByte> out;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (*p == '?') {
            out.push_back({0, true});
            while (*p == '?') ++p;
            continue;
        }
        char hex[3] = {p[0], p[1], 0};
        out.push_back({static_cast<uint8_t>(std::strtoul(hex, nullptr, 16)), false});
        p += 2;
    }
    return out;
}

}  // namespace

void FindAllPatterns(const Module& module, const char* pattern,
                     const std::function<bool(uintptr_t)>& hit) {
    auto bytes = ParsePattern(pattern);
    size_t lead = 0;
    while (lead < bytes.size() && bytes[lead].wildcard) ++lead;
    if (lead == bytes.size()) return;
    const size_t n = bytes.size();
    const uint8_t first = bytes[lead].value;

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        auto start = reinterpret_cast<const uint8_t*>(module.base + section->VirtualAddress);
        size_t len = section->Misc.VirtualSize;
        if (len < n) continue;
        const uint8_t* end = start + len - n + 1;
        for (const uint8_t* p = start + lead; p < end + lead;) {
            p = static_cast<const uint8_t*>(std::memchr(p, first, (end + lead) - p));
            if (!p) break;
            const uint8_t* m = p - lead;
            size_t k = lead + 1;
            for (; k < n; ++k)
                if (!bytes[k].wildcard && m[k] != bytes[k].value) break;
            if (k == n && !hit(reinterpret_cast<uintptr_t>(m))) return;
            ++p;
        }
    }
}

uintptr_t FindPattern(const Module& module, const char* pattern) {
    uintptr_t found = 0;
    FindAllPatterns(module, pattern, [&](uintptr_t a) {
        found = a;
        return false;
    });
    return found;
}

uintptr_t ResolveRip(uintptr_t insn, int disp_offset, int insn_len) {
    if (!insn) return 0;
    int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(insn + disp_offset), sizeof(disp));
    return insn + insn_len + disp;
}

static bool PlausibleAddress(uintptr_t address, size_t size) {
    return address >= 0x10000 && address + size < 0x7FFFFFFFFFFFull && address + size > address;
}

// Reads and writes are a plain "rep movsb" that may fault: a vectored exception handler turns an
// access violation inside that one instruction into a "false" return. No system call is made on
// the normal path (ReadProcessMemory/VirtualQuery every frame caused visible stutters).
extern "C" int zz_safe_copy(void* dst, const void* src, size_t size);
extern "C" char zz_safe_copy_begin[], zz_safe_copy_end[], zz_safe_copy_fail[];
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl zz_safe_copy\n"
    "zz_safe_copy:\n"
    "    push rsi\n"
    "    push rdi\n"
    "    mov rdi, rcx\n"
    "    mov rsi, rdx\n"
    "    mov rcx, r8\n"
    ".globl zz_safe_copy_begin\n"
    "zz_safe_copy_begin:\n"
    "    rep movsb\n"
    ".globl zz_safe_copy_end\n"
    "zz_safe_copy_end:\n"
    "    mov eax, 1\n"
    "    pop rdi\n"
    "    pop rsi\n"
    "    ret\n"
    ".globl zz_safe_copy_fail\n"
    "zz_safe_copy_fail:\n"
    "    xor eax, eax\n"
    "    pop rdi\n"
    "    pop rsi\n"
    "    ret\n"
    ".att_syntax prefix\n");

namespace {

std::atomic<uint64_t> g_faults{0};

LONG CALLBACK SafeCopyHandler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != STATUS_GUARD_PAGE_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = info->ContextRecord->Rip;
    if (rip >= reinterpret_cast<uintptr_t>(zz_safe_copy_begin) &&
        rip < reinterpret_cast<uintptr_t>(zz_safe_copy_end)) {
        info->ContextRecord->Rip = reinterpret_cast<uintptr_t>(zz_safe_copy_fail);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool g_handler_installed = false;

}  // namespace

uint64_t FaultCount() { return g_faults.load(std::memory_order_relaxed); }

void Init() {
    if (g_handler_installed) return;
    g_handler_installed = AddVectoredExceptionHandler(1, SafeCopyHandler) != nullptr;
}

bool ReadRaw(uintptr_t address, void* out, size_t size) {
    if (!PlausibleAddress(address, size) || !g_handler_installed) return false;
    return zz_safe_copy(out, reinterpret_cast<const void*>(address), size) != 0;
}

bool WriteRaw(uintptr_t address, const void* data, size_t size) {
    if (!PlausibleAddress(address, size) || !g_handler_installed) return false;
    return zz_safe_copy(reinterpret_cast<void*>(address), data, size) != 0;
}

uintptr_t ReadPtr(uintptr_t address) {
    return Read<uintptr_t>(address).value_or(0);
}

bool PatchCode(uintptr_t address, const void* data, size_t size) {
    if (!address) return false;
    if (std::memcmp(reinterpret_cast<const void*>(address), data, size) == 0) return true;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old))
        return false;
    std::memcpy(reinterpret_cast<void*>(address), data, size);
    VirtualProtect(reinterpret_cast<void*>(address), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return true;
}

std::string RttiNameOfVtable(uintptr_t vtable) {
    static const Module module = MainModule();
    if (!module.Contains(vtable)) return {};
    // vtable[-1] = complete object locator {sig=1, offset, cdOffset, typeDescriptor RVA, ...}
    const uintptr_t col = ReadPtr(vtable - 8);
    if (!module.Contains(col) || Read<uint32_t>(col).value_or(0) != 1) return {};
    const uint32_t td_rva = Read<uint32_t>(col + 12).value_or(0);
    if (!td_rva || td_rva >= module.size) return {};
    char name[160] = {};
    if (!ReadRaw(module.base + td_rva + 0x10, name, sizeof(name) - 1)) return {};
    if (std::strncmp(name, ".?AV", 4) != 0 && std::strncmp(name, ".?AU", 4) != 0) return {};
    std::string out(name + 4);
    // Templates: "?$DLRuntimeClassImpl@VCSCamera@CS@@..." -> "DLRuntimeClassImpl<CSCamera>"
    if (out.rfind("?$", 0) == 0) {
        const size_t at = out.find('@');
        std::string outer = out.substr(2, at - 2);
        std::string inner;
        if (at != std::string::npos && at + 2 < out.size()) {
            inner = out.substr(at + 2);  // skip "@V" / "@U"
            inner = inner.substr(0, inner.find('@'));
        }
        return outer + "<" + inner + ">";
    }
    return out.substr(0, out.find('@'));
}

bool RttiHasBase(uintptr_t vtable, const char* base) {
    static const Module module = MainModule();
    if (!module.Contains(vtable)) return false;
    const uintptr_t col = ReadPtr(vtable - 8);
    if (!module.Contains(col) || Read<uint32_t>(col).value_or(0) != 1) return false;
    const uint32_t chd = Read<uint32_t>(col + 16).value_or(0);
    if (!chd || chd >= module.size) return false;
    const uint32_t count = Read<uint32_t>(module.base + chd + 8).value_or(0);
    const uint32_t array = Read<uint32_t>(module.base + chd + 12).value_or(0);
    if (!array || array >= module.size || count > 64) return false;
    const std::string want = std::string(".?AV") + base + "@";
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t bcd = Read<uint32_t>(module.base + array + i * 4).value_or(0);
        const uint32_t td = bcd ? Read<uint32_t>(module.base + bcd).value_or(0) : 0;
        char name[96] = {};
        if (!td || td >= module.size || !ReadRaw(module.base + td + 0x10, name, sizeof(name) - 1))
            continue;
        if (std::strncmp(name, want.c_str(), want.size()) == 0) return true;
    }
    return false;
}

uintptr_t FindVtable(const char* mangled) {
    const Module module = MainModule();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    struct Range {
        const uint8_t* start;
        size_t len;
        bool exec;
    };
    std::vector<Range> sections;
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        sections.push_back({reinterpret_cast<const uint8_t*>(module.base + section->VirtualAddress),
                            section->Misc.VirtualSize,
                            (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    auto find_all = [&](const void* needle, size_t n, bool data_only,
                        const std::function<bool(const uint8_t*)>& hit) {
        for (const Range& r : sections) {
            if (data_only && r.exec) continue;
            for (const uint8_t* p = r.start; r.len >= n && p <= r.start + r.len - n; ++p) {
                p = static_cast<const uint8_t*>(
                    std::memchr(p, *static_cast<const uint8_t*>(needle), r.start + r.len - n + 1 - p));
                if (!p) break;
                if (std::memcmp(p, needle, n) == 0 && !hit(p)) return;
            }
        }
    };
    // TypeDescriptor {vtable, spare, ".?AVName@@"} -> CompleteObjectLocator {1, 0, cd, td RVA, ...}
    // -> vtable[-1] = &COL.
    const std::string name = std::string(".?AV") + mangled + "@@";
    uintptr_t result = 0;
    find_all(name.c_str(), name.size() + 1, true, [&](const uint8_t* td) {
        const uint32_t td_rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(td) - 0x10 - module.base);
        find_all(&td_rva, 4, true, [&](const uint8_t* p) {
            const uint8_t* col = p - 12;
            uint32_t head[3];
            std::memcpy(head, col, sizeof(head));
            if (head[0] != 1 || head[1] != 0) return true;
            const uintptr_t col_va = reinterpret_cast<uintptr_t>(col);
            find_all(&col_va, 8, true, [&](const uint8_t* slot) {
                result = reinterpret_cast<uintptr_t>(slot) + 8;
                return false;
            });
            return result == 0;
        });
        return result == 0;
    });
    return result;
}

std::string RttiName(uintptr_t object) {
    if (!object) return {};
    return RttiNameOfVtable(ReadPtr(object));
}

}  // namespace zz::mem
