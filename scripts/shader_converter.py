"""Build and stage the experimental Shadow renderer's shader converter (XenosRecomp).

A separate track from the game build: nothing here touches the ReXGlue SDK, the
generated sources or the game executable. The converter is a pinned XenosRecomp
checkout with experiments/gpu-layer/xenosrecomp-diablo3.patch applied, built under
tools/XenosRecomp and copied into `<game build>/xenosrecomp`, where the game looks
for it (port/src/experiments/shadow_shaders.cpp).
"""
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess

import sdk_patches

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXPERIMENT = ROOT / "experiments/gpu-layer"
LOCK = EXPERIMENT / "xenosrecomp.lock.json"
PATCH = EXPERIMENT / "xenosrecomp-diablo3.patch"
SOURCE = ROOT / "tools/XenosRecomp"
STAMP = ".sotr-converter.json"
WINDOWS = platform.system() == "Windows"
EXECUTABLE = "XenosRecomp.exe" if WINDOWS else "XenosRecomp"


def lock():
    return json.loads(LOCK.read_text())


def _wanted():
    """What a current build was made from: the pinned commit and the patch."""
    patch = hashlib.sha256(PATCH.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    return {"commit": lock()["commit"], "patch": patch}


def files(source=SOURCE):
    """The files the game needs beside it: the converter, its DXC libraries and the header."""
    built = source / "build/XenosRecomp"
    if WINDOWS:  # the build copies the DLLs next to the executable
        libraries = [built / "dxcompiler.dll", built / "dxil.dll"]
    else:
        dxc = source / "thirdparty/dxc-bin/lib" / ("arm64" if platform.machine() in ("aarch64", "arm64") else "x64")
        libraries = [dxc / "libdxcompiler.so", dxc / "libdxil.so"]
    return [built / EXECUTABLE, *libraries, source / "XenosRecomp/shader_common.h"]


def state(source=SOURCE):
    """"missing", "stale" (built from another commit or patch) or "current"."""
    if not all(path.is_file() for path in files(source)):
        return "missing"
    try:
        stamp = json.loads((source / "build" / STAMP).read_text())
    except (OSError, ValueError):
        return "stale"
    return "current" if stamp == _wanted() else "stale"


def build(cmake, jobs, run, source=SOURCE):
    """Check out the pinned XenosRecomp, apply the patch and build the converter."""
    pinned = lock()
    wanted = _wanted()
    # Pristine sources every time: the patch is applied in place.
    sdk_patches.checkout_pinned(source, pinned["repository"], pinned["commit"], run, clean=True)
    subprocess.run(["git", "-C", str(source), "apply", str(PATCH)], check=True)
    build_dir = source / "build"
    (build_dir / STAMP).unlink(missing_ok=True)
    # clang-cl on Windows (the configuration the fork has been built with there).
    c, cxx = ("clang-cl", "clang-cl") if WINDOWS else ("clang", "clang++")
    run(cmake, "-S", source, "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        f"-DCMAKE_C_COMPILER={c}", f"-DCMAKE_CXX_COMPILER={cxx}")
    run(cmake, "--build", build_dir, "--target", "XenosRecomp", "--parallel", jobs)
    missing = [path.name for path in files(source) if not path.is_file()]
    if missing:
        raise ValueError(f"the converter build did not produce {', '.join(missing)}")
    (build_dir / STAMP).write_text(json.dumps(wanted) + "\n")


def staged(game_build, source=SOURCE):
    """True when `game_build`/xenosrecomp holds the same files as the converter build."""
    target = game_build / "xenosrecomp"
    return all((target / path.name).is_file()
               and (target / path.name).stat().st_size == path.stat().st_size
               and (target / path.name).stat().st_mtime_ns == path.stat().st_mtime_ns
               for path in files(source))


def stage(game_build, source=SOURCE):
    """Copy the converter next to a built game (times kept: the game's shader cache is
    rebuilt when the converter's size or time changes)."""
    target = game_build / "xenosrecomp"
    target.mkdir(parents=True, exist_ok=True)
    for path in files(source):
        shutil.copy2(path, target / path.name)
