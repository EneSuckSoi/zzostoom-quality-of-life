#pragma once
#include <string>

namespace zz::log {

// Opens <dll folder>/zzostoom_qol.log (truncated on every game start).
void Init(const std::wstring& dll_dir);
void Write(const char* fmt, ...);

}  // namespace zz::log

#define ZZ_LOG(...) ::zz::log::Write(__VA_ARGS__)
