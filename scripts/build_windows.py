#!/usr/bin/env python3
"""Build the shared port on Windows with the pinned ReXGlue SDK (Direct3D 12).

Mirrors build_linux.py: the same pinned SDK commit, the same Linux patches and
the same codegen output, plus patches/windows/rexglue-windows.patch for the
Direct3D 12 renderer. Needs Visual Studio 2022 (C++ and CMake workload), LLVM
clang 22+, Git and Python on the machine.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

from apply_generated_patches import patch_generated
from build_linux import PATCHES
from codegen_title_update import generate_tu2
from title_updates import DISC_SHA256, TU2_SHA256, tu2_layout

ROOT = pathlib.Path(__file__).resolve().parents[1]
VS_CMAKE = pathlib.Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
                        r"\CommonExtensions\Microsoft\CMake")


def run(*args, cwd=ROOT):
    print("$ " + " ".join(str(arg) for arg in args), flush=True)
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True)


def add_tool_paths():
    """Add the usual install locations of clang, CMake and Ninja when they are not on PATH."""
    candidates = [pathlib.Path(r"C:\Program Files\LLVM\bin"), VS_CMAKE / "CMake/bin", VS_CMAKE / "Ninja"]
    for tool, folder in (("clang", candidates[0]), ("cmake", candidates[1]), ("ninja", candidates[2])):
        if not shutil.which(tool) and folder.is_dir():
            os.environ["PATH"] = f"{folder}{os.pathsep}{os.environ['PATH']}"


def prepare_path():
    add_tool_paths()
    missing = [tool for tool in ("git", "clang", "clang++", "cmake", "ninja") if not shutil.which(tool)]
    if missing:
        raise ValueError(f"Not found on PATH: {', '.join(missing)}. Install Visual Studio 2022 with the C++ "
                         "and CMake workload, and LLVM clang 22+.")


def apply_patch(sdk, patch):
    check = ["git", "-C", str(sdk), "apply", "--check"]
    reverse = subprocess.run([*check, "--reverse", str(patch)], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL).returncode == 0
    if not reverse:
        run(*check, patch)
        run("git", "-C", sdk, "apply", patch)


def restore_symlinks(sdk):
    """Git checks symlinks out as text stubs on Windows; replace them with copies of their targets."""
    out = subprocess.check_output(["git", "-C", str(sdk / "thirdparty/libmspack"), "ls-files", "-s"], text=True)
    for line in out.splitlines():
        mode, _, _, name = line.split(maxsplit=3)
        if mode != "120000":
            continue
        link = sdk / "thirdparty/libmspack" / name
        target = link.parent / link.read_text().strip()
        if target.is_file() and link.stat().st_size < 260:
            shutil.copyfile(target, link)


def build_sdk(args):
    lock = json.loads((ROOT / "port/linux/rexglue.lock.json").read_text())
    sdk = args.sdk_source
    if not sdk.exists():
        run("git", "-c", "core.autocrlf=false", "clone", "--no-checkout", lock["repository"], sdk)
        run("git", "-C", sdk, "config", "core.autocrlf", "false")
        run("git", "-C", sdk, "checkout", "--detach", lock["commit"])
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if actual != lock["commit"]:
        raise ValueError(f"SDK revision mismatch: {actual}; expected {lock['commit']}")
    run("git", "-C", sdk, "submodule", "update", "--init", "--recursive")
    restore_symlinks(sdk)
    for name in PATCHES:
        apply_patch(sdk, ROOT / "patches/linux" / name)
    apply_patch(sdk, ROOT / "patches/windows/rexglue-windows.patch")
    shutil.copyfile(ROOT / "patches/d3d_screenshot.cpp", sdk / "src/graphics/d3d12/d3d_screenshot.cpp")
    run("cmake", "--preset", "win-amd64", "-DREXGLUE_ENABLE_TRACY=OFF", "-DREXGLUE_BUILD_TESTS=OFF", cwd=sdk)
    build = sdk / "out/build/win-amd64"
    run("cmake", "--build", build, "--config", "RelWithDebInfo", "--parallel", args.jobs)
    run("cmake", "--install", build, "--config", "RelWithDebInfo", "--prefix", args.sdk_prefix)
    return sdk / "out/win-amd64/RelWithDebInfo/rexgluerd.exe"


def codegen_exe(args):
    exe = args.sdk_source / "out/win-amd64/RelWithDebInfo/rexgluerd.exe"
    if not exe.is_file():
        raise ValueError("SDK not built; use --build-sdk")
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                     allow_abbrev=False)
    parser.add_argument("--build-sdk", action="store_true", help="Clone, patch, build and install the pinned SDK")
    parser.add_argument("--sdk-source", type=pathlib.Path, default=ROOT / "tools/rexglue-sdk-win")
    parser.add_argument("--sdk-prefix", type=pathlib.Path, default=ROOT / "tools/rexglue-install-win")
    parser.add_argument("--game-dir", type=pathlib.Path, default=ROOT / "game")
    parser.add_argument("--title-update", choices=["tu2"], help="Build the verified USA TU2 executable separately")
    parser.add_argument("--no-extra-features", dest="plain", action="store_true",
                        help="Compile without the optional PC features (separate -plain directory). "
                             "By default they are built in and switched on at launch with --extra_features")
    parser.add_argument("--extra-features", action="store_true", help=argparse.SUPPRESS)  # now the default
    parser.add_argument("--skip-codegen", action="store_true", help="Reuse previously generated sources")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    args = parser.parse_args()
    args.extra_features = not args.plain
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    for name in ("sdk_source", "sdk_prefix", "game_dir"):
        setattr(args, name, getattr(args, name).resolve())
    try:
        prepare_path()
        if args.build_sdk:
            build_sdk(args)
        rexglue = codegen_exe(args)
        if not (args.sdk_prefix / "lib/cmake/rexglue").is_dir():
            raise ValueError("SDK not installed; use --build-sdk")
        xex = tu2_layout(args.game_dir)[2] if args.title_update else args.game_dir / "Default.xex"
        expected = TU2_SHA256 if args.title_update else DISC_SHA256
        if not xex.is_file() or hashlib.sha256(xex.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Default.xex missing or SHA-256 mismatch for {args.title_update or 'base-disc'} build")
        generated = ROOT / "port/generated" / ("tu2" if args.title_update else "linux")
        if args.title_update:
            generate_tu2(args.game_dir, rexglue, args.skip_codegen)
        else:
            if not args.skip_codegen:
                manifest_dir = ROOT / "port/linux"
                manifest = (manifest_dir / "diablo3_manifest.toml").read_text()
                for suffix in ("", "/Default.xex"):
                    manifest = manifest.replace(json.dumps("../../game" + suffix),
                                                json.dumps(args.game_dir.as_posix() + suffix, ensure_ascii=False))
                local_manifest = manifest_dir / "local_manifest.toml"
                local_manifest.write_text(manifest, encoding="utf-8")
                # An interrupted regeneration must not leave a valid reuse stamp.
                (generated / "source-xex.sha256").unlink(missing_ok=True)
                run(rexglue, "codegen", local_manifest)
                generated.mkdir(parents=True, exist_ok=True)
                (generated / "source-xex.sha256").write_text(expected + "\n")
            patch_generated(generated)
        variant = ("tu2-" if args.title_update else "") + ("" if args.extra_features else "plain-")
        build = ROOT / f"port/out/build/win-amd64-{variant}relwithdebinfo"
        options = [f"-DSOULS_ENABLE_EXTRA_FEATURES={'ON' if args.extra_features else 'OFF'}",
                   f"-DSOULS_TITLE_UPDATE_2={'ON' if args.title_update else 'OFF'}"]
        if args.extra_features:
            options.append(f"-DSOULS_BASE_XEX={xex}")
        run("cmake", "-S", ROOT / "port", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_CXX_FLAGS=-march=x86-64-v2",
            f"-DCMAKE_PREFIX_PATH={args.sdk_prefix}", *options)
        run("cmake", "--build", build, "--parallel", args.jobs)
        print(f"Built {build / 'diablo3.exe'}")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Windows build failed: {error}\n")


if __name__ == "__main__":
    sys.exit(main())
