#!/usr/bin/env python3
"""Build playable clients from a Diablo III disc image and, optionally, the USA TU2 package.

Each step is skipped when its output already exists and verifies, so an
interrupted build can be resumed with the same command. Progress lines of the
form ``::step::<n>/<total>::<title>`` are printed for the launcher.
"""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

import build_windows
import extract_disc
import extract_update
from stage_title_update import TU2_PACKAGE_SHA256, stage
from title_updates import DISC_SHA256, TU2_SHA256

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
TU2_CPKS = ("Patch.cpk", "Patch2.cpk", "enUS_Patch.cpk", "enUS_Patch2.cpk")
# Variant id -> (title update, extra features); ids match the launcher's builds.
VARIANTS = {"base": (False, False), "extras": (False, True),
            "tu2": (True, False), "tu2-extras": (True, True)}
LABELS = {"base": "Base", "extras": "Base + Extras", "tu2": "TU2", "tu2-extras": "TU2 + Extras"}
WINDOWS = platform.system() == "Windows"
GIB = 1024 ** 3
# Rough upper bounds measured on Linux RelWithDebInfo builds.
SDK_BYTES, VARIANT_BYTES, CODEGEN_BYTES = 3 * GIB, 1 * GIB, GIB // 2


class BuildError(Exception):
    pass


def step(index, total, title):
    print(f"::step::{index}/{total}::{title}", flush=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(*args, cwd=ROOT):
    print("$ " + " ".join(str(arg) for arg in args), flush=True)
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True)


def base_ready(game_dir):
    """A completed extraction writes disc-manifest.json last."""
    xex = game_dir / "Default.xex"
    return (game_dir / "disc-manifest.json").is_file() and xex.is_file() and sha256(xex) == DISC_SHA256


def tu2_ready(game_dir):
    xex = game_dir / "Default.xex"
    return ((game_dir / "applied-update-manifest.json").is_file() and xex.is_file()
            and sha256(xex) == TU2_SHA256
            and all((game_dir / "CPKs" / name).is_file() for name in TU2_CPKS))


def stamp_matches(variant_dir, expected):
    stamp = ROOT / "port/generated" / variant_dir / "source-xex.sha256"
    return stamp.is_file() and stamp.read_text().strip() == expected


def cmake_version(cmake):
    try:
        output = subprocess.run([cmake, "--version"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    match = re.search(r"(\d+)\.(\d+)", output)
    return (int(match[1]), int(match[2])) if match else None


def missing_tools(args, compile_needed, need_patcher):
    """Human-readable problems that would stop the build before it starts."""
    problems = []
    if not shutil.which("git") and (compile_needed or need_patcher):
        problems.append("git is required to fetch the pinned SDK and patch tools.")
    if not compile_needed and not need_patcher:
        return problems
    if not args.cmake:
        problems.append("CMake 3.25 or newer was not found; install it or choose its path.")
    elif (cmake_version(args.cmake) or (0, 0)) < (3, 25):
        problems.append(f"{args.cmake} is older than CMake 3.25.")
    for tool in ("ninja", "clang", "clang++"):
        if not shutil.which(tool):
            problems.append(f"{tool} was not found on PATH.")
    if compile_needed and platform.system() not in ("Linux", "Windows"):
        problems.append("Automated compilation supports Linux and Windows only; use --data-only to prepare "
                        "the game folders.")
    return problems


def sdk_installed(args):
    if WINDOWS:
        return ((args.sdk_prefix / "lib/cmake/rexglue").is_dir()
                and (args.sdk_source / "out/win-amd64/RelWithDebInfo/rexgluerd.exe").is_file())
    return (args.sdk_prefix / "bin/rexglue").is_file()


def disc_bytes(iso):
    with iso.open("rb") as source:
        return sum(length for _, is_dir, _, length in extract_disc.Disc(source).entries() if not is_dir)


def same_filesystem(a, b):
    a = next(p for p in (a, *a.parents) if p.exists())
    b = next(p for p in (b, *b.parents) if p.exists())
    return os.stat(a).st_dev == os.stat(b).st_dev


def verify_iso(iso):
    if not iso.is_file():
        raise BuildError(f"Disc image not found: {iso}")
    try:
        found = extract_disc.file_sha256(iso, "Default.xex")
    except ValueError as error:
        raise BuildError(f"{iso.name} is not a readable Xbox 360 game disc image ({error}).")
    if found == TU2_SHA256:
        raise BuildError("That image already contains the TU2 executable; choose the original disc image.")
    if found != DISC_SHA256:
        raise BuildError(f"{iso.name} is not the USA Diablo III: Reaper of Souls - Ultimate Evil Edition disc "
                         "(Default.xex does not match). Other regions are not supported yet.")


def verify_package(package):
    if not package.is_file():
        raise BuildError(f"Title update package not found: {package}")
    if sha256(package) != TU2_PACKAGE_SHA256:
        raise BuildError(f"{package.name} is not the verified USA Title Update 2 package "
                         f"(expected SHA-256 {TU2_PACKAGE_SHA256}).")


def build_patcher(args):
    """Clone the pinned XenonRecomp and build tools/d3-patch/d3-patch."""
    lock = json.loads((SCRIPTS / "d3-patch/xenonrecomp.lock.json").read_text())
    source = ROOT / "tools/XenonRecomp"
    if not source.exists():
        run("git", "clone", "--no-checkout", lock["repository"], source)
        run("git", "-C", source, "checkout", "--detach", lock["commit"])
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if actual != lock["commit"]:
        raise BuildError(f"XenonRecomp revision mismatch: {actual}; expected {lock['commit']}")
    run("git", "-C", source, "submodule", "update", "--init", "--recursive")
    build = ROOT / "tools/d3-patch"
    run(args.cmake, "-S", SCRIPTS / "d3-patch", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", f"-DXENONRECOMP_DIR={source}")
    run(args.cmake, "--build", build, "--target", "d3-patch", "--parallel", args.jobs)
    return build / ("d3-patch.exe" if os.name == "nt" else "d3-patch")


def build_variant(args, variant):
    tu2, extras = VARIANTS[variant]
    game = args.game_dir_tu2 if tu2 else args.game_dir
    command = [sys.executable, SCRIPTS / ("build_windows.py" if WINDOWS else "build_linux.py"),
               "--game-dir", game, "--sdk-prefix", args.sdk_prefix, "--jobs", args.jobs]
    if WINDOWS:
        command += ["--sdk-source", args.sdk_source]  # CMake, Ninja and Clang come from PATH
    else:
        command += ["--cmake", args.cmake]
    if tu2:
        command += ["--title-update", "tu2"]
    if extras:
        command.append("--extra-features")
    # Reuse sources generated earlier in this or a previous run for the same executable.
    if not args.regenerate and stamp_matches("tu2" if tu2 else "linux", TU2_SHA256 if tu2 else DISC_SHA256):
        command.append("--skip-codegen")
    run(*command)


def plan(args):
    """Return [(title, action)] for the work still to do, after checking inputs."""
    steps = []
    wanted_tu2 = any(VARIANTS[v][0] for v in args.variants) or (args.data_only and args.title_update)
    need_base = not base_ready(args.game_dir)
    need_tu2 = wanted_tu2 and not tu2_ready(args.game_dir_tu2)
    compile_needed = bool(args.variants) and not args.data_only
    need_sdk = compile_needed and not sdk_installed(args)

    if need_base and not args.iso:
        raise BuildError(f"No extracted disc in {args.game_dir}; choose your disc image (ISO).")
    if need_tu2:
        if args.game_dir_tu2.exists():
            raise BuildError(f"{args.game_dir_tu2} exists but is not a complete staged TU2; "
                             "move it aside or choose another TU2 game folder.")
        if not args.title_update:
            raise BuildError("TU2 builds need the USA Title Update 2 package (tu00000002_00000000).")
    problems = missing_tools(args, compile_needed, need_tu2 and not args.patcher)
    if problems:
        raise BuildError("\n".join(problems))

    if need_base:
        verify_iso(args.iso)
    if need_tu2:
        verify_package(args.title_update)

    disc = disc_bytes(args.iso) if need_base else sum(
        f.stat().st_size for f in args.game_dir.rglob("*") if f.is_file())
    needed = disc if need_base else 0
    if need_tu2 and not same_filesystem(args.game_dir, args.game_dir_tu2):
        needed += disc
    if compile_needed:
        needed += (SDK_BYTES if need_sdk else 0) + VARIANT_BYTES * len(args.variants) + CODEGEN_BYTES * 2
    target = next(p for p in (args.game_dir, *args.game_dir.parents) if p.exists())
    free = shutil.disk_usage(target).free
    if needed > free:
        raise BuildError(f"About {needed / GIB:.1f} GiB of free space is needed; {free / GIB:.1f} GiB is free.")

    if need_base:
        steps.append(("Extract disc image", lambda: extract_disc.extract(args.iso, args.game_dir)))
    if need_tu2:
        update_dir = ROOT / "port/out/tu2-update"
        steps.append(("Extract title update", lambda: extract_update.extract(args.title_update, update_dir)))
        patcher = [args.patcher]
        if not args.patcher:
            def patcher_step():
                patcher[0] = build_patcher(args)
            steps.append(("Build TU2 patch tool", patcher_step))
        steps.append(("Apply TU2", lambda: stage(args.game_dir, update_dir, args.game_dir_tu2, patcher[0])))
    if need_sdk:
        sdk = argparse.Namespace(sdk_source=args.sdk_source, sdk_prefix=args.sdk_prefix, jobs=args.jobs)
        if WINDOWS:
            steps.append(("Build ReXGlue SDK (one-time)", lambda: build_windows.build_sdk(sdk)))
        else:
            import build_linux
            steps.append(("Build ReXGlue SDK (one-time)", lambda: build_linux.build_sdk(sdk, args.cmake)))
    if compile_needed:
        for variant in args.variants:
            steps.append((f"Build {LABELS[variant]} (codegen and compile)", lambda v=variant: build_variant(args, v)))
    return steps


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                     allow_abbrev=False)
    parser.add_argument("--iso", type=pathlib.Path, help="USA Ultimate Evil Edition disc image")
    parser.add_argument("--title-update", type=pathlib.Path, metavar="PACKAGE",
                        help="USA Title Update 2 package (tu00000002_00000000)")
    parser.add_argument("--variants", default=None,
                        help="Comma-separated builds: base, extras, tu2, tu2-extras "
                             "(default: base, plus tu2 when a title update is given)")
    parser.add_argument("--data-only", action="store_true",
                        help="Only prepare the game folders; do not compile")
    parser.add_argument("--game-dir", type=pathlib.Path, default=ROOT / "game")
    parser.add_argument("--game-dir-tu2", type=pathlib.Path, default=ROOT / "game-tu2")
    host = "win" if WINDOWS else "linux"
    parser.add_argument("--sdk-source", type=pathlib.Path, default=ROOT / f"tools/rexglue-sdk-{host}")
    parser.add_argument("--sdk-prefix", type=pathlib.Path, default=ROOT / f"tools/rexglue-install-{host}")
    parser.add_argument("--patcher", type=pathlib.Path, help="Existing d3-patch executable")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    parser.add_argument("--regenerate", action="store_true", help="Rerun codegen even if sources are current")
    parser.add_argument("--check", action="store_true", help="Validate inputs and tools, list the steps, and stop")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.variants is None:
        args.variants = ["base"] + (["tu2"] if args.title_update else [])
    else:
        args.variants = [v.strip() for v in args.variants.split(",") if v.strip()]
        unknown = [v for v in args.variants if v not in VARIANTS]
        if unknown:
            parser.error(f"unknown variant(s): {', '.join(unknown)}")
    for name in ("iso", "title_update", "game_dir", "game_dir_tu2", "sdk_source", "sdk_prefix", "patcher"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.expanduser().resolve())
    try:
        if WINDOWS:
            # Finds clang, CMake and Ninja in their usual install folders (Visual Studio, LLVM).
            build_windows.add_tool_paths()
        args.cmake = shutil.which(args.cmake)
        steps = plan(args)
        if args.check:
            for index, (title, _) in enumerate(steps, 1):
                print(f"{index}. {title}")
            print("Everything is already built." if not steps else "Inputs and tools look good.")
            return
        for index, (title, action) in enumerate(steps, 1):
            step(index, len(steps), title)
            action()
        print("::done::", flush=True)
        print("Build complete." if steps else "Everything is already built.")
    except (BuildError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Build failed: {error}\n")


if __name__ == "__main__":
    main()
