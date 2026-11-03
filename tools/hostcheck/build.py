#!/usr/bin/env python3
"""Builds a game for the PC it runs on, with tools/hostcheck/shim/ standing in for libnx.

    python tools/hostcheck/build.py SP --game-dir <folder> --iortcw <iortcw clone> --gl-include <folder with GL/>

The game folder is laid out as on the SD card: main/*.pk3. It is compiled in as the base
directory, like the console's path, and written to (settings, saves, logs). The iortcw clone
provides the zlib and libjpeg the console gets from devkitPro. Needs a GCC whose `long` has 64 bits,
as on the console: MSYS2's own "gcc" package, or any Linux. See README.md.
"""

import argparse
import concurrent.futures
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
OUT = ROOT / "build_host"

JPEG_SOURCES = """jaricom jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff jcinit jcmainct jcmarker
jcmaster jcomapi jcparam jcprepct jcsample jctrans jdapimin jdapistd jdarith jdatadst jdatasrc jdcoefct jdcolor
jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jdtrans jerror jfdctflt jfdctfst
jfdctint jidctflt jidctfst jidctint jmemmgr jmemnobs jquant1 jquant2 jutils""".split()

ZLIB_SOURCES = "adler32 crc32 inffast inflate inftrees zutil".split()

MODULES = {"cgame": ["CGAMEDLL", "CGAME"], "qagame": ["GAMEDLL", "QAGAME"], "ui": ["UI"]}


def posix(path):
    """A path as the compiler takes it. GCC on MSYS2 sees C:/ as relative where it looks for headers."""
    text = pathlib.PurePath(path).as_posix()
    return "/%s%s" % (text[0].lower(), text[2:]) if text[1:2] == ":" else text


def source_lists():
    """The lists of cmake/RtcwSources.cmake, with the references between them followed."""
    text = (ROOT / "cmake" / "RtcwSources.cmake").read_text()
    lists = {}
    for name, body in re.findall(r"set\((RTCW_\w+)\s+(.*?)\)", text, re.S):
        items = []
        for item in body.split():
            ref = re.fullmatch(r"\$\{(\w+)\}", item)
            items += lists[ref.group(1)] if ref else [item]
        lists[name] = items
    return lists


def compile_one(job):
    command, source, obj = job
    if obj.exists() and obj.stat().st_mtime >= source.stat().st_mtime:
        return None
    obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(command + ["-c", posix(source), "-o", posix(obj)], capture_output=True, text=True)
    if result.returncode:
        return "%s:\n%s" % (source, result.stderr[-3000:])
    return None


def run_jobs(jobs, workers):
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        errors = [e for e in pool.map(compile_one, jobs) if e]
    for error in errors[:12]:
        print(error)
    if errors:
        sys.exit("%d files failed" % len(errors))


def check(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        print(" ".join(str(c) for c in command))
        print(result.stdout[-3000:], result.stderr[-3000:])
        sys.exit(1)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game", choices=["SP", "MP"])
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--iortcw", required=True, type=pathlib.Path)
    parser.add_argument("--gl-include", required=True, type=pathlib.Path)
    parser.add_argument("--gcc", default="gcc")
    parser.add_argument("--flags", default="", help="extra compiler flags, for example -fsanitize=alignment")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()

    gxx = re.sub(r"gcc(\.exe)?$", "g++", args.gcc)
    tool_dir = pathlib.PurePosixPath(args.gcc).parent if "/" in args.gcc else None
    ld = str(tool_dir / "ld") if tool_dir else "ld"
    objcopy = str(tool_dir / "objcopy") if tool_dir else "objcopy"

    game = args.game
    lower = game.lower()
    code = ROOT / "third_party" / "iortcw" / game / "code"
    lists = source_lists()
    out = OUT / lower

    include = OUT / "include"
    for folder in ("GL", "KHR"):
        if (args.gl_include / folder).exists() and not (include / folder).exists():
            shutil.copytree(args.gl_include / folder, include / folder)

    upstream = args.iortcw / game / "code"
    extra = args.flags.split()
    # in a header: quotes do not survive a command line between Windows and MSYS2
    out.mkdir(parents=True, exist_ok=True)
    config = out / "host_config.h"
    config_text = '#define PRODUCT_VERSION "1.51d-%s"\n#define NX_GAME_DIR "%s"\n#define DEFAULT_BASEDIR "%s"\n' % (game, args.game_dir, args.game_dir)
    if not config.exists() or config.read_text() != config_text:
        config.write_text(config_text)
    defines = ["-D__SWITCH__", "-DNX_HOSTCHECK", "-DRTCW_DEKO3D", "-DNO_VM_COMPILED", "-include", posix(config)]
    if game == "MP":
        defines.append("-DRTCW_MP")
    base = ["-std=gnu11", "-O1", "-g", "-fsigned-char", "-fno-strict-aliasing", "-fno-omit-frame-pointer", "-w"] + extra
    includes = ["-I%s" % posix(p) for p in (ROOT / "tools/hostcheck/shim", ROOT / "tools/hostcheck", ROOT / "source/platform",
                                     ROOT / "source/renderer", code, code / "sys", include,
                                     upstream / "jpeg-8c", upstream / "zlib-1.2.11")]
    cc = [args.gcc] + base + defines + includes

    # the libraries
    jobs = []
    lib = OUT / "lib"
    for name in JPEG_SOURCES:
        jobs.append(([args.gcc, "-O2", "-w", "-I%s" % posix(upstream / "jpeg-8c")], upstream / "jpeg-8c" / (name + ".c"), lib / ("jpeg_%s.o" % name)))
    for name in ZLIB_SOURCES:
        jobs.append(([args.gcc, "-O2", "-w", "-I%s" % posix(upstream / "zlib-1.2.11")], upstream / "zlib-1.2.11" / (name + ".c"), lib / ("z_%s.o" % name)))

    # the engine
    engine_objects = []
    for rel in lists["RTCW_%s_ENGINE" % game] + lists["RTCW_RENDERER"]:
        obj = out / "engine" / (rel.replace("/", "_") + ".o")
        jobs.append((cc, code / rel, obj))
        engine_objects.append(obj)
    for rel in lists["RTCW_BOTLIB"]:
        obj = out / "engine" / (rel.replace("/", "_") + ".o")
        jobs.append((cc + ["-DBOTLIB"], code / rel, obj))
        engine_objects.append(obj)
    cxx = [gxx] + [f for f in base if f != "-std=gnu11"] + defines + includes
    for rel in lists["RTCW_SPLINES"]:
        obj = out / "engine" / (rel.replace("/", "_") + ".o")
        jobs.append((cxx, code / rel, obj))
        engine_objects.append(obj)
    for source in ("source/platform/nx_main.c", "source/platform/nx_log.c", "source/platform/nx_modules.c",
                   "source/platform/nx_zipio.c", "source/platform/sys_nx.c", "source/platform/in_nx.c",
                   "source/platform/snd_nx.c", "source/renderer/dkgl.c", "source/renderer/glimp_dk.c",
                   "tools/hostcheck/host_device.c", "tools/hostcheck/shim/switch_shim.c"):
        command = list(cc)
        if source.endswith("nx_main.c"):
            command.append("-Dmain=NX_Main")
        obj = out / "platform" / (pathlib.Path(source).name + ".o")
        jobs.append((command, ROOT / source, obj))
        engine_objects.append(obj)
    host_main = out / "platform" / "host_main.o"
    jobs.append(([args.gcc, "-O1", "-g", "-w"], ROOT / "tools/hostcheck/host_main.c", host_main))

    # the modules: each with its own names, as on the console (cmake/RtcwModule.cmake)
    module_jobs = {}
    for module, module_defines in MODULES.items():
        command = cc + ["-D%s" % d for d in module_defines]
        if os.name == "nt":
            # on MSYS2 the default goes through tables of addresses, which do not
            # survive a module's symbols being made private
            command.append("-mcmodel=small")
        objects = []
        for rel in lists["RTCW_%s_%s" % (game, module.upper())]:
            obj = out / module / (rel.replace("/", "_") + ".o")
            jobs.append((command, code / rel, obj))
            objects.append(obj)
        module_jobs[module] = objects

    print("compiling %d files with %d jobs" % (len(jobs), args.jobs))
    run_jobs(jobs, args.jobs)

    module_objects = []
    for module, objects in module_jobs.items():
        # the bounds of the module's writable data, for nx_modules.c
        bounds = out / module / "bounds.c"
        first, last = out / module / "bounds_first.o", out / module / "bounds_last.o"
        bounds.write_text('char module_data_start[32] __attribute__((aligned(32))) = { 1 };\nchar module_bss_start[32] __attribute__((aligned(32)));\n')
        check([args.gcc, "-fno-common", "-c", posix(bounds), "-o", posix(first)])
        bounds.write_text('char module_data_end[32] __attribute__((aligned(32))) = { 1 };\nchar module_bss_end[32] __attribute__((aligned(32)));\n')
        check([args.gcc, "-fno-common", "-c", posix(bounds), "-o", posix(last)])

        partial = out / ("%s_partial.o" % module)
        merged = out / ("%s.o" % module)
        check([ld, "-r", "-o", posix(partial), posix(first)] + [posix(o) for o in objects] + [posix(last)])
        keep = ["dllEntry", "vmMain", "module_data_start", "module_data_end", "module_bss_start", "module_bss_end"]
        check([objcopy] + ["--keep-global-symbol=%s" % k for k in keep] + [posix(partial), posix(partial) + ".2"])
        renames = {
            "dllEntry": "rtcw_%s_dllEntry" % module, "vmMain": "rtcw_%s_vmMain" % module,
            "module_data_start": "rtcw_%s_data_start" % module, "module_data_end": "rtcw_%s_data_end" % module,
            "module_bss_start": "rtcw_%s_bss_start" % module, "module_bss_end": "rtcw_%s_bss_end" % module,
        }
        check([objcopy] + [a for old, new in renames.items() for a in ("--redefine-sym", "%s=%s" % (old, new))]
              + [posix(partial) + ".2", posix(merged)])
        module_objects.append(merged)

    exe = OUT / ("rtcw_%s_host" % lower)
    objects = engine_objects + module_objects + [host_main] + sorted(lib.glob("*.o"))
    response = out / "link.rsp"
    response.write_text("\n".join(posix(o) for o in objects) + "\n")
    print("linking")
    check([gxx, "@%s" % posix(response), "-g", "-o", posix(exe), "-lm", "-lpthread"] + extra)
    print("built", exe)


if __name__ == "__main__":
    main()
