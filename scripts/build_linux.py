#!/usr/bin/env python3
"""Build the shared port with the pinned Linux Vulkan SDK."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

from apply_generated_patches import patch_generated
from codegen_title_update import generate_tu2
from title_updates import DISC_SHA256, TU2_SHA256

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCHES = ("rexglue-registration.patch", "rexglue-texture-exponent.patch",
           "rexglue-object-reference.patch", "rexglue-keyboard.patch",
           "rexglue-wait-precision.patch", "rexglue-idle-toast.patch")


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def build_sdk(args, cmake):
    lock = json.loads((ROOT / "port/linux/rexglue.lock.json").read_text())
    sdk = args.sdk_source
    if not sdk.exists():
        run("git", "clone", "--no-checkout", lock["repository"], sdk)
        run("git", "-C", sdk, "checkout", "--detach", lock["commit"])
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if actual != lock["commit"]:
        raise ValueError(f"SDK revision mismatch: {actual}; expected {lock['commit']}")
    run("git", "-C", sdk, "submodule", "update", "--init", "--recursive")
    for name in PATCHES:
        patch = ROOT / "patches/linux" / name
        applied = subprocess.run(["git", "-C", str(sdk), "apply", "--reverse", "--check", str(patch)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if not applied:
            run("git", "-C", sdk, "apply", "--check", patch)
            run("git", "-C", sdk, "apply", patch)
    build = ROOT / "port/out/sdk-linux"
    run(cmake, "-S", sdk, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        f"-DCMAKE_C_FLAGS=-march={lock['architecture']}",
        f"-DCMAKE_CXX_FLAGS=-march={lock['architecture']}",
        "-DREXGLUE_USE_VULKAN=ON", "-DREXGLUE_USE_D3D12=OFF",
        "-DREXGLUE_ENABLE_TRACY=OFF", "-DREXGLUE_BUILD_TESTS=OFF",
        f"-DCMAKE_INSTALL_PREFIX={args.sdk_prefix}")
    run(cmake, "--build", build, "--parallel", args.jobs)
    output = sdk / "out/linux-amd64"
    regression = ROOT / "port/out/rexglue-registration-test"
    run("clang++", "-std=c++23", f"-march={lock['architecture']}",
        f"-I{sdk / 'include'}", f"-I{sdk / 'thirdparty/fmt/include'}",
        ROOT / "tests/rexglue_registration.cpp", output / "librexcodegen.a",
        output / "libfmt.a", output / "libdisasm.a", f"-L{output}", "-lrexruntime",
        f"-Wl,-rpath,{output}", "-o", regression)
    run(regression)
    run(cmake, "--install", build)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--build-sdk", action="store_true", help="Clone, patch, build and install the pinned SDK")
    parser.add_argument("--sdk-source", type=pathlib.Path, default=ROOT / "tools/rexglue-sdk-linux")
    parser.add_argument("--sdk-prefix", type=pathlib.Path, default=ROOT / "tools/rexglue-install-linux")
    parser.add_argument("--game-dir", type=pathlib.Path, default=ROOT / "game")
    parser.add_argument("--title-update", choices=["tu2"], help="Build the verified USA TU2 executable separately")
    parser.add_argument("--probe", action="store_true", help="Build asset-free GPU and rendering diagnostics")
    parser.add_argument("--extra-features", action="store_true",
                        help="Build optional PC menu settings and autosave wording in a separate directory")
    parser.add_argument("--skip-codegen", action="store_true", help="Reuse previously generated Linux sources")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.title_update and args.probe:
        parser.error("--title-update cannot be combined with --probe")
    if args.probe and args.extra_features:
        parser.error("--probe and --extra-features cannot be combined")
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
            xex = args.game_dir / "Default.xex"
            expected = TU2_SHA256 if args.title_update else DISC_SHA256
            if not xex.is_file() or hashlib.sha256(xex.read_bytes()).hexdigest() != expected:
                raise ValueError(f"Default.xex missing or SHA-256 mismatch for {args.title_update or 'base-disc'} build")
            variant = "tu2" if args.title_update else "linux"
            generated = ROOT / "port/generated" / variant
            if args.title_update:
                generate_tu2(args.game_dir, args.sdk_prefix / "bin/rexglue", args.skip_codegen)
            elif not args.skip_codegen:
                manifest_dir = ROOT / "port/linux"
                game_placeholder = "../../game"
                manifest = (manifest_dir / "diablo3_manifest.toml").read_text()
                # JSON strings also encode paths safely as TOML basic strings.
                manifest = manifest.replace(json.dumps(game_placeholder), json.dumps(str(args.game_dir), ensure_ascii=False))
                manifest = manifest.replace(json.dumps(game_placeholder + "/Default.xex"), json.dumps(str(xex), ensure_ascii=False))
                local_manifest = manifest_dir / "local_manifest.toml"
                local_manifest.write_text(manifest, encoding="utf-8")
                # An interrupted regeneration must not leave a valid reuse stamp.
                (generated / "source-xex.sha256").unlink(missing_ok=True)
                run(args.sdk_prefix / "bin/rexglue", "codegen", local_manifest)
                generated.mkdir(parents=True, exist_ok=True)
                (generated / "source-xex.sha256").write_text(expected + "\n")
            if not args.title_update:
                patch_generated(generated)
            name = "linux-amd64-extras-relwithdebinfo" if args.extra_features else "linux-amd64-relwithdebinfo"
            if args.title_update:
                name = "linux-amd64-tu2-extras-relwithdebinfo" if args.extra_features else "linux-amd64-tu2-relwithdebinfo"
            source, build = ROOT / "port", ROOT / "port/out/build" / name
        feature_options = [] if args.probe else [
            f"-DSOULS_ENABLE_EXTRA_FEATURES={'ON' if args.extra_features else 'OFF'}",
            f"-DSOULS_TITLE_UPDATE_2={'ON' if args.title_update else 'OFF'}"]
        if args.extra_features:
            feature_options.append(f"-DSOULS_BASE_XEX={xex}")
        run(cmake, "-S", source, "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_CXX_COMPILER=clang++",
            "-DCMAKE_CXX_FLAGS=-march=x86-64-v2", f"-DCMAKE_PREFIX_PATH={args.sdk_prefix}",
            *feature_options)
        run(cmake, "--build", build, "--parallel", args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Linux build failed: {error}\n")


if __name__ == "__main__":
    main()
