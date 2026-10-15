"""Patches to the pinned ReXGlue SDK that both the Linux and Windows builds apply."""
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMON_DIR = ROOT / "patches/common"
LINUX_DIR = ROOT / "patches/linux"
COMMON_PATCHES = ("rexglue-registration.patch", "rexglue-texture-exponent.patch",
                  "rexglue-object-reference.patch", "rexglue-keyboard.patch",
                  "rexglue-wait-precision.patch", "rexglue-idle-toast.patch",
                  "rexglue-username.patch")
LINUX_PATCHES = ("rexglue-present-effects.patch",)


def checkout_pinned_sdk(sdk, run, config=()):
    """Clone the SDK if missing, check out the pinned commit and verify it.

    `run` is the caller's command runner; `config` holds git settings (e.g.
    core.autocrlf=false) applied to the clone before checkout.
    """
    lock = json.loads((ROOT / "port/base/rexglue.lock.json").read_text())
    if not sdk.exists():
        run("git", *(a for c in config for a in ("-c", c)), "clone", "--no-checkout", lock["repository"], sdk)
        for c in config:
            run("git", "-C", sdk, "config", *c.split("=", 1))
        run("git", "-C", sdk, "checkout", "--detach", lock["commit"])
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if actual != lock["commit"]:
        raise ValueError(f"SDK revision mismatch: {actual}; expected {lock['commit']}")
    run("git", "-C", sdk, "submodule", "update", "--init", "--recursive")
    return lock


def apply_patch(sdk, patch):
    """Apply a patch to the SDK checkout unless it is already applied."""
    check = ["git", "-C", str(sdk), "apply", "--check"]
    applied = subprocess.run([*check, "--reverse", str(patch)], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL).returncode == 0
    if not applied:
        subprocess.run([*check, str(patch)], check=True)
        subprocess.run(["git", "-C", str(sdk), "apply", str(patch)], check=True)


def apply_common_patches(sdk):
    for name in COMMON_PATCHES:
        apply_patch(sdk, COMMON_DIR / name)


def apply_linux_patches(sdk):
    for name in LINUX_PATCHES:
        apply_patch(sdk, LINUX_DIR / name)
