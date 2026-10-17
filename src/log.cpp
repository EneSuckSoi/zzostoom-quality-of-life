#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace zz::log {

static FILE* g_file = nullptr;
static std::mutex g_mutex;

void Init(const std::wstring& dll_dir) {
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    std::wstring path = dll_dir + L"\\zzostoom_qol.log";
    g_file = _wfopen(path.c_str(), L"w");
}

void Write(const char* fmt, ...) {
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

}  // namespace zz::log
