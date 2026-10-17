"""Builds zzostoom_qol.dll with the portable llvm-mingw toolchain.

    py build.py            # incremental build -> build/zzostoom_qol.dll
    py build.py --install  # build and copy the DLL into the ME3 eldenring-mods folder
    py build.py --test     # also build the test programs (build/pad_test.exe, build/harness.exe)
    py build.py --clean    # remove build/
"""
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
TOOLCHAIN = os.environ.get("ZZ_LLVM_MINGW") or next(
    iter(sorted(glob.glob(os.path.join(ROOT, "..", "tools", "llvm-mingw-*", "bin")))), "")
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")
OUT = os.path.join(BUILD, "zzostoom_qol.dll")
ME3_MODS = os.path.expandvars(r"%LOCALAPPDATA%\garyttierney\me3\config\profiles\eldenring-mods")

IMGUI = os.path.join(ROOT, "third_party", "imgui")
MINHOOK = os.path.join(ROOT, "third_party", "minhook")

OWN_SOURCES = sorted(glob.glob(os.path.join(ROOT, "src", "*.cpp")) +
                     glob.glob(os.path.join(ROOT, "src", "generated", "*.cpp")))
THIRD_PARTY = [os.path.join(IMGUI, f) for f in
               ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
                "backends/imgui_impl_dx12.cpp", "backends/imgui_impl_win32.cpp")] + \
              [os.path.join(MINHOOK, "src", f) for f in
               ("buffer.c", "hook.c", "trampoline.c", "hde/hde64.c")]

DEFINES = ["-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-DNDEBUG", "-D_WIN32_WINNT=0x0A00",
           # MSVC ABI for COM methods that return structs (GetCPUDescriptorHandleForHeapStart...)
           "-DWIDL_EXPLICIT_AGGREGATE_RETURNS",
           # we feed the controller to ImGui ourselves (the backend would read it
           # through our own XInput hook)
           "-DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD"]
INCLUDES = ["-I" + p for p in (os.path.join(ROOT, "src"), IMGUI, os.path.join(IMGUI, "backends"),
                                os.path.join(MINHOOK, "include"))]
CXXFLAGS = ["-std=c++20", "-O2", "-Wall", "-Wno-unknown-pragmas", "-Wno-missing-braces"]
CFLAGS = ["-std=c11", "-O2", "-w"]
LIBS = ["-ld3d12", "-ldxgi", "-ld3dcompiler", "-ldinput8", "-ldxguid", "-luser32", "-lgdi32",
        "-lshell32", "-lole32", "-luuid", "-limm32", "-ldwmapi", "-lversion", "-lwinmm"]


def tool(name):
    path = os.path.join(TOOLCHAIN, name + ".exe")
    if not os.path.exists(path):
        sys.exit(f"Toolchain not found ({path}). Set ZZ_LLVM_MINGW to llvm-mingw's bin folder.")
    return path


def obj_path(src):
    rel = os.path.relpath(src, ROOT).replace(os.sep, "_").replace("/", "_")
    return os.path.join(OBJ, os.path.splitext(rel)[0] + ".o")


def headers_mtime():
    return max(os.path.getmtime(h) for h in glob.glob(os.path.join(ROOT, "src", "*.h")))


def compile_one(src, newest_header, extra=()):
    obj = obj_path(src)
    own = src in OWN_SOURCES
    if os.path.exists(obj) and not extra:
        obj_time = os.path.getmtime(obj)
        if obj_time > os.path.getmtime(src) and (not own or obj_time > newest_header):
            return src, True, ""
    is_c = src.endswith(".c")
    cmd = [tool("clang" if is_c else "clang++"), "-c", src, "-o", obj] + DEFINES + INCLUDES + \
          (CFLAGS if is_c else CXXFLAGS) + list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return src, p.returncode == 0, (p.stdout + p.stderr).strip()


def link(objs, out, shared, libs):
    cmd = [tool("clang++")] + (["-shared"] if shared else []) + ["-o", out] + objs + \
          ["-static", "-s", "-Wl,--gc-sections"] + libs
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        sys.exit("Link failed.")


def build_tests():
    # Controller filter unit test: pad.cpp + its test, built with ZZ_PAD_TEST.
    test_obj = os.path.join(OBJ, "pad_test_pad.o")
    for src, obj, extra in ((os.path.join(ROOT, "src", "pad.cpp"), test_obj, ["-DZZ_PAD_TEST"]),
                            (os.path.join(ROOT, "test", "pad_test.cpp"),
                             os.path.join(OBJ, "pad_test.o"), ["-DZZ_PAD_TEST"])):
        cmd = [tool("clang++"), "-c", src, "-o", obj] + DEFINES + INCLUDES + CXXFLAGS + extra
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if p.returncode != 0:
            print(p.stdout + p.stderr)
            sys.exit("Test build failed.")
    mh = [obj_path(os.path.join(MINHOOK, "src", f))
          for f in ("buffer.c", "hook.c", "trampoline.c", "hde/hde64.c")]
    link([test_obj, os.path.join(OBJ, "pad_test.o"), obj_path(os.path.join(ROOT, "src", "log.cpp")),
          obj_path(os.path.join(ROOT, "src", "config.cpp"))] + mh,
         os.path.join(BUILD, "pad_test.exe"), False, ["-luser32"])
    print("OK -> build/pad_test.exe")
    harness = os.path.join(ROOT, "test", "harness.cpp")
    hobj = os.path.join(OBJ, "harness.o")
    cmd = [tool("clang++"), "-c", harness, "-o", hobj] + DEFINES + CXXFLAGS
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        sys.exit("Harness build failed.")
    link([hobj, "-municode"], os.path.join(BUILD, "harness.exe"), False,
         ["-ld3d12", "-ldxgi", "-luser32", "-lole32", "-lwindowscodecs", "-luuid"])
    print("OK -> build/harness.exe")


def main():
    if "--clean" in sys.argv:
        shutil.rmtree(BUILD, ignore_errors=True)
        print("cleaned")
        return
    os.makedirs(OBJ, exist_ok=True)
    newest_header = headers_mtime()
    sources = THIRD_PARTY + OWN_SOURCES
    ok = True
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, success, output in pool.map(lambda s: compile_one(s, newest_header), sources):
            if output:
                print(f"--- {os.path.relpath(src, ROOT)}\n{output}")
            ok &= success
    if not ok:
        sys.exit("Build failed.")
    link([obj_path(s) for s in sources], OUT, True, LIBS)
    print(f"OK -> {OUT} ({os.path.getsize(OUT) // 1024} KB)")
    if "--test" in sys.argv:
        build_tests()
    if "--install" in sys.argv:
        dest = os.path.join(ME3_MODS, os.path.basename(OUT))
        shutil.copy2(OUT, dest)
        print(f"Installed -> {dest}")


if __name__ == "__main__":
    main()
