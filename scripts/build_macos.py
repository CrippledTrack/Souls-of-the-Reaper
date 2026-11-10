#!/usr/bin/env python3
"""Build the shared port on macOS (Vulkan through the SDK's bundled MoltenVK).

Experimental: this reuses the pinned Linux SDK revision and patches, and the
shared `port/generated` codegen. Nothing here has been validated for gameplay.
"""
import argparse
import hashlib
import pathlib
import platform
import shutil
import subprocess

import build_profiles
from codegen_title_update import generate
from sdk_patches import (apply_common_patches, apply_linux_patches, apply_profile_patches, checkout_pinned_sdk,
                         clear_stamp, write_stamp)
from title_updates import DISC_SHA256, USA_UPDATE_SHA256, update_layout

ROOT = pathlib.Path(__file__).resolve().parents[1]
CPU_FLAGS = {"arm64": "-march=armv8-a", "x86_64": "-march=x86-64-v2"}


def sdk_paths(profile, arch):
    """(SDK checkout, SDK install) of `profile`; a Rosetta (x86_64) install has its own folder."""
    source, prefix = profile.sdk_paths("macos")
    return source, prefix if arch == "arm64" else prefix.with_name(f"{prefix.name}-{arch}")


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True, **kwargs)


def build_sdk(args, cmake):
    sdk = args.sdk_source
    checkout_pinned_sdk(sdk, run)
    apply_common_patches(sdk)
    # The image-scaling presenter is shared with Linux.
    apply_linux_patches(sdk)
    # A profile with SDK patches of its own has its own SDK checkout, build and install.
    profile = build_profiles.get(getattr(args, "profile", None))
    apply_profile_patches(sdk, profile)
    build = ROOT / f"port/out/sdk-macos{profile.sdk_suffix()}"
    flags = CPU_FLAGS[args.arch]
    run(cmake, "-S", sdk, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
        f"-DCMAKE_OSX_ARCHITECTURES={args.arch}", "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3",
        f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}",
        "-DREXGLUE_USE_VULKAN=ON", "-DREXGLUE_ENABLE_TRACY=OFF", "-DREXGLUE_BUILD_TESTS=OFF",
        f"-DCMAKE_INSTALL_PREFIX={args.sdk_prefix}")
    run(cmake, "--build", build, "--parallel", args.jobs)
    clear_stamp(args.sdk_prefix)
    run(cmake, "--install", build)
    write_stamp(args.sdk_prefix, "macos", profile.sdk_patch_dirs())


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--build-sdk", action="store_true", help="Clone, patch, build and install the pinned SDK")
    parser.add_argument("--profile", choices=sorted(build_profiles.PROFILES), default="normal",
                        help="Build profile (scripts/build_profiles.py): 'experimental' compiles the "
                             "experimental features into a separate -experimental build folder")
    parser.add_argument("--arch", choices=sorted(CPU_FLAGS), default=platform.machine(),
                        help="Target architecture; x86_64 runs under Rosetta on Apple Silicon")
    parser.add_argument("--sdk-source", type=pathlib.Path, help="SDK checkout (default: the profile's, under tools/)")
    parser.add_argument("--sdk-prefix", type=pathlib.Path,
                        help="SDK install (default: the profile's, under tools/, plus -x86_64 for that architecture)")
    parser.add_argument("--game-dir", type=pathlib.Path, default=ROOT / "game")
    parser.add_argument("--probe", action="store_true", help="Build asset-free GPU and rendering diagnostics")
    parser.add_argument("--title-update", choices=["usa"], help="Build the verified USA update executable separately")
    parser.add_argument("--no-extra-features", dest="plain", action="store_true",
                        help="Compile without the optional PC features (they are built in by default)")
    parser.add_argument("--skip-codegen", action="store_true", help="Reuse previously generated sources")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.title_update and args.probe:
        parser.error("--title-update cannot be combined with --probe")
    profile = build_profiles.get(args.profile)
    if args.probe and profile.id != "normal":
        parser.error("--profile cannot be combined with --probe")
    source, prefix = sdk_paths(profile, args.arch)
    args.sdk_source, args.sdk_prefix = args.sdk_source or source, args.sdk_prefix or prefix
    for name in ("sdk_source", "sdk_prefix", "game_dir"):
        setattr(args, name, getattr(args, name).resolve())
    cmake = shutil.which(args.cmake)
    if not cmake:
        parser.error("CMake 3.25+ is required (brew install cmake ninja vulkan-loader molten-vk)")
    try:
        if args.build_sdk:
            build_sdk(args, cmake)
        if not (args.sdk_prefix / "bin/rexglue").is_file():
            raise ValueError("SDK not installed; use --build-sdk or --sdk-prefix /path/to/installed/sdk")
        if args.probe:
            source, build = ROOT / "port/probe", ROOT / "port/out/build/macos-probe"
            feature_options = []
        else:
            update = bool(args.title_update)
            xex = update_layout(args.game_dir)[2] if update else args.game_dir / "Default.xex"
            expected = USA_UPDATE_SHA256 if update else DISC_SHA256
            if not xex.is_file() or hashlib.sha256(xex.read_bytes()).hexdigest() != expected:
                raise ValueError(f"Default.xex missing or SHA-256 mismatch for {args.title_update or 'base-disc'} build")
            generate("update" if update else "base", args.game_dir, args.sdk_prefix / "bin/rexglue", args.skip_codegen)
            source = ROOT / "port"
            build = ROOT / "port/out/build" / profile.build_name(f"macos-{args.arch}", update, args.plain)
            feature_options = [f"-DSOULS_ENABLE_EXTRA_FEATURES={'OFF' if args.plain else 'ON'}",
                               f"-DSOULS_TITLE_UPDATE={'ON' if update else 'OFF'}"]
            if not args.plain:
                feature_options.append(f"-DSOULS_BASE_XEX={xex}")
            # Always passed, so a folder configured for another profile cannot keep its options.
            feature_options.append("-DSOULS_EXPERIMENTAL=OFF")
            feature_options.extend(profile.cmake_options)
        flags = CPU_FLAGS[args.arch]
        run(cmake, "-S", source, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
            f"-DCMAKE_OSX_ARCHITECTURES={args.arch}", "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3",
            f"-DCMAKE_CXX_FLAGS={flags}", f"-DCMAKE_PREFIX_PATH={args.sdk_prefix}", *feature_options)
        run(cmake, "--build", build, "--parallel", args.jobs)
        print(f"Built: {build}")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"macOS build failed: {error}\n")


if __name__ == "__main__":
    main()
