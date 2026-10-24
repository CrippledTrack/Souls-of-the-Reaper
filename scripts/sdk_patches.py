"""Patches to the pinned ReXGlue SDK that both the Linux and Windows builds apply."""
import hashlib
import json
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMON_DIR = ROOT / "patches/common"
LINUX_DIR = ROOT / "patches/linux"
COMMON_PATCHES = ("rexglue-registration.patch", "rexglue-texture-exponent.patch",
                  "rexglue-object-reference.patch", "rexglue-keyboard.patch",
                  "rexglue-wait-precision.patch", "rexglue-idle-toast.patch",
                  "rexglue-username.patch", "rexglue-systemlink-net.patch",
                  "rexglue-timer-queue.patch")
LINUX_PATCHES = ("rexglue-present-effects.patch",)
STAMP = ".sotr-sdk.json"
# Folders under patches/ that go into the SDK, per host.
PATCH_DIRS = {"linux": ("common", "systemlink", "linux"), "win": ("common", "systemlink", "windows")}


def lock():
    return json.loads((ROOT / "port/base/rexglue.lock.json").read_text())


def patch_hash(host):
    """Digest of everything this port adds to the SDK for `host` ("linux" or "win")."""
    digest = hashlib.sha256()
    for name in PATCH_DIRS[host]:
        base = ROOT / "patches" / name
        for path in sorted(p for p in base.rglob("*") if p.is_file()):
            digest.update(path.relative_to(ROOT).as_posix().encode() + b"\0")
            # Line endings differ between checkouts and must not change the digest.
            digest.update(path.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return digest.hexdigest()


def write_stamp(prefix, host):
    """Record which SDK revision and patch set an install was built from."""
    (prefix / STAMP).write_text(json.dumps({"commit": lock()["commit"], "patches": patch_hash(host)}) + "\n")


def stamp_state(prefix, host):
    """"current" when the install matches the pinned commit and patches, else "stale"
    (an install from before stamps existed has no stamp and counts as stale)."""
    try:
        stamp = json.loads((prefix / STAMP).read_text())
    except (OSError, ValueError):
        return "stale"
    return "current" if stamp == {"commit": lock()["commit"], "patches": patch_hash(host)} else "stale"


def checkout_pinned_sdk(sdk, run, config=()):
    """Clone the SDK if missing, check out the pinned commit and verify it.

    `run` is the caller's command runner; `config` holds git settings (e.g.
    core.autocrlf=false) applied to the clone before checkout. Submodules do not
    read the clone's config, so they get the same settings on the command line.
    """
    lock = json.loads((ROOT / "port/base/rexglue.lock.json").read_text())
    options = [a for c in config for a in ("-c", c)]
    if not sdk.exists():
        run("git", *options, "clone", "--no-checkout", lock["repository"], sdk)
        for c in config:
            run("git", "-C", sdk, "config", *c.split("=", 1))
        run("git", "-C", sdk, "checkout", "--detach", lock["commit"])
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if actual != lock["commit"]:
        raise ValueError(f"SDK revision mismatch: {actual}; expected {lock['commit']}")
    run("git", *options, "-C", sdk, "submodule", "update", "--init", "--recursive")
    return lock


def _applies(sdk, patch, reverse=False):
    command = ["git", "-C", str(sdk), "apply", "--check", *(["--reverse"] if reverse else []), str(patch)]
    return subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def apply_patch(sdk, patch, later=()):
    """Apply a patch to the SDK checkout unless it is already applied.

    `later` lists the patches applied after this one. Those may rewrite the lines this patch
    added, so it no longer reverses cleanly; it then counts as applied when one of them is.
    """
    if _applies(sdk, patch, reverse=True):
        return
    if _applies(sdk, patch):
        subprocess.run(["git", "-C", str(sdk), "apply", str(patch)], check=True)
    elif not any(_applies(sdk, other, reverse=True) for other in later):
        # Neither direction applies and nothing stacked on top explains it: a real conflict.
        subprocess.run(["git", "-C", str(sdk), "apply", "--check", str(patch)], check=True)


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
    for index, name in enumerate(COMMON_PATCHES):
        apply_patch(sdk, COMMON_DIR / name, [COMMON_DIR / later for later in COMMON_PATCHES[index + 1:]])


def apply_linux_patches(sdk):
    for name in LINUX_PATCHES:
        apply_patch(sdk, LINUX_DIR / name)
