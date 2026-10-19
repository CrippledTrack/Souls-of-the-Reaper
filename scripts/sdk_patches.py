"""Patches to the pinned ReXGlue SDK that both the Linux and Windows builds apply."""
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMON_DIR = ROOT / "patches/common"
COMMON_PATCHES = ("rexglue-registration.patch", "rexglue-texture-exponent.patch",
                  "rexglue-object-reference.patch", "rexglue-keyboard.patch",
                  "rexglue-wait-precision.patch", "rexglue-idle-toast.patch",
                  "rexglue-username.patch", "rexglue-systemlink-net.patch")


def apply_patch(sdk, patch):
    """Apply a patch to the SDK checkout unless it is already applied."""
    check = ["git", "-C", str(sdk), "apply", "--check"]
    applied = subprocess.run([*check, "--reverse", str(patch)], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL).returncode == 0
    if not applied:
        subprocess.run([*check, str(patch)], check=True)
        subprocess.run(["git", "-C", str(sdk), "apply", str(patch)], check=True)


def install_sources(sdk):
    """Copy whole source files this port adds to the SDK (patches/systemlink mirrors the SDK tree)."""
    source_root = ROOT / "patches/systemlink"
    for source in sorted(p for p in source_root.rglob("*") if p.is_file()):
        target = sdk / source.relative_to(source_root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def apply_common_patches(sdk):
    # The new files go in first: the patches below refer to them.
    install_sources(sdk)
    for name in COMMON_PATCHES:
        apply_patch(sdk, COMMON_DIR / name)
