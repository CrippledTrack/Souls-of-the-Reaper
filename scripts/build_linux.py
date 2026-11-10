#!/usr/bin/env python3
"""Build the shared port with the pinned Linux Vulkan SDK."""
import argparse
import hashlib
import pathlib
import shutil
import subprocess

from codegen_title_update import generate
import build_profiles
from sdk_patches import (apply_common_patches, apply_linux_patches, apply_profile_patches, checkout_pinned_sdk,
                         clear_stamp, write_stamp)
from title_updates import DISC_SHA256, USA_UPDATE_SHA256, update_layout

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def build_sdk(args, cmake):
    sdk = args.sdk_source
    lock = checkout_pinned_sdk(sdk, run)
    apply_common_patches(sdk)
    apply_linux_patches(sdk)
    # A profile with SDK patches of its own has its own SDK checkout, build and install.
    profile = build_profiles.get(getattr(args, "profile", None))
    apply_profile_patches(sdk, profile)
    build = ROOT / f"port/out/sdk-linux{profile.sdk_suffix()}"
    run(cmake, "-S", sdk, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        f"-DCMAKE_C_FLAGS=-march={lock['architecture']}",
        f"-DCMAKE_CXX_FLAGS=-march={lock['architecture']}",
        "-DREXGLUE_USE_VULKAN=ON", "-DREXGLUE_USE_D3D12=OFF",
        "-DREXGLUE_ENABLE_TRACY=OFF", "-DREXGLUE_BUILD_TESTS=OFF",
        f"-DCMAKE_INSTALL_PREFIX={args.sdk_prefix}")
    run(cmake, "--build", build, "--parallel", args.jobs)
    output = sdk / "out/linux-amd64"
    regression = ROOT / f"port/out/rexglue-registration-test{profile.sdk_suffix()}"
    run("clang++", "-std=c++23", f"-march={lock['architecture']}",
        f"-I{sdk / 'include'}", f"-I{sdk / 'thirdparty/fmt/include'}",
        ROOT / "tests/rexglue_registration.cpp", output / "librexcodegen.a",
        output / "libfmt.a", output / "libdisasm.a", f"-L{output}", "-lrexruntime",
        f"-Wl,-rpath,{output}", "-o", regression)
    run(regression)
    clear_stamp(args.sdk_prefix)
    run(cmake, "--install", build)
    write_stamp(args.sdk_prefix, "linux", profile.sdk_patch_dirs())


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--build-sdk", action="store_true", help="Clone, patch, build and install the pinned SDK")
    parser.add_argument("--profile", choices=sorted(build_profiles.PROFILES), default="normal",
                        help="Build profile (scripts/build_profiles.py): 'experimental' compiles the "
                             "experimental features into a separate -experimental build folder")
    parser.add_argument("--sdk-source", type=pathlib.Path, help="SDK checkout (default: the profile's, under tools/)")
    parser.add_argument("--sdk-prefix", type=pathlib.Path, help="SDK install (default: the profile's, under tools/)")
    parser.add_argument("--game-dir", type=pathlib.Path, default=ROOT / "game")
    parser.add_argument("--title-update", choices=["usa"], help="Build the verified USA update executable separately")
    parser.add_argument("--probe", action="store_true", help="Build asset-free GPU and rendering diagnostics")
    parser.add_argument("--no-extra-features", dest="plain", action="store_true",
                        help="Compile without the optional PC features (separate -plain directory). "
                             "By default they are built in and switched on at launch with --extra_features")
    parser.add_argument("--extra-features", action="store_true", help=argparse.SUPPRESS)  # now the default
    parser.add_argument("--skip-codegen", action="store_true", help="Reuse previously generated Linux sources")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.title_update and args.probe:
        parser.error("--title-update cannot be combined with --probe")
    args.extra_features = not args.plain and not args.probe
    profile = build_profiles.get(args.profile)
    if args.probe and profile.id != "normal":
        parser.error("--profile cannot be combined with --probe")
    source, prefix = profile.sdk_paths("linux")
    args.sdk_source, args.sdk_prefix = args.sdk_source or source, args.sdk_prefix or prefix
    for name in ("sdk_source", "sdk_prefix", "game_dir"):
        setattr(args, name, getattr(args, name).resolve())
    cmake = shutil.which(args.cmake)
    if not cmake:
        parser.error("CMake 3.25+ is required; use --cmake /path/to/cmake")
    try:
        if args.build_sdk:
            build_sdk(args, cmake)
        if not (args.sdk_prefix / "bin/rexglue").is_file():
            raise ValueError("SDK not installed; use --build-sdk or --sdk-prefix /path/to/installed/sdk")
        if args.probe:
            source, build = ROOT / "port/probe", ROOT / "port/out/build/linux-probe"
        else:
            xex = update_layout(args.game_dir)[2] if args.title_update else args.game_dir / "Default.xex"
            expected = USA_UPDATE_SHA256 if args.title_update else DISC_SHA256
            if not xex.is_file() or hashlib.sha256(xex.read_bytes()).hexdigest() != expected:
                raise ValueError(f"Default.xex missing or SHA-256 mismatch for {args.title_update or 'base-disc'} build")
            variant = "update" if args.title_update else "base"
            generate(variant, args.game_dir, args.sdk_prefix / "bin/rexglue", args.skip_codegen)
            name = profile.build_name("linux-amd64", bool(args.title_update), not args.extra_features)
            source, build = ROOT / "port", ROOT / "port/out/build" / name
        feature_options = [] if args.probe else [
            f"-DSOULS_ENABLE_EXTRA_FEATURES={'ON' if args.extra_features else 'OFF'}",
            f"-DSOULS_TITLE_UPDATE={'ON' if args.title_update else 'OFF'}"]
        if args.extra_features:
            feature_options.append(f"-DSOULS_BASE_XEX={xex}")
        if not args.probe:
            # Always passed, so a folder configured for another profile cannot keep its options.
            feature_options.append("-DSOULS_EXPERIMENTAL=OFF")
            feature_options.extend(profile.cmake_options)
        run(cmake, "-S", source, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_CXX_COMPILER=clang++",
            "-DCMAKE_CXX_FLAGS=-march=x86-64-v2", f"-DCMAKE_PREFIX_PATH={args.sdk_prefix}",
            *feature_options)
        run(cmake, "--build", build, "--parallel", args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Linux build failed: {error}\n")


if __name__ == "__main__":
    main()
