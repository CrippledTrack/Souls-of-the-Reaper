#!/usr/bin/env python3
"""Verify and stage the USA TU2 separately from the unmodified disc."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from title_updates import DISC_SHA256, TU2_SHA256

TU2_PACKAGE_SHA256 = "faf9feaa28e75035e61a960231d2c27f0bed3b7676db65747fa25352f19bddf3"
TITLE_ID, MEDIA_ID = 0x394F07D4, 0x38E299CD


def xex_option(data, wanted):
    if len(data) < 24 or data[:4] != b"XEX2":
        raise ValueError("Invalid XEX2 header")
    count = struct.unpack_from(">I", data, 20)[0]
    if count > (len(data) - 24) // 8:
        raise ValueError("Truncated XEX2 option table")
    for index in range(count):
        key, offset = struct.unpack_from(">II", data, 24 + index * 8)
        if key == wanted:
            return offset
    raise ValueError(f"Missing XEX2 option {wanted:#x}")


def verify_patch_source(base, patch):
    execution = xex_option(base, 0x40006)
    descriptor = xex_option(patch, 0x5FF)
    security = struct.unpack_from(">I", base, 16)[0]
    if execution + 24 > len(base) or descriptor + 32 > len(patch) or security + 264 > len(base):
        raise ValueError("Truncated XEX2 execution, security or patch descriptor")
    media, version, _, title = struct.unpack_from(">IIII", base, execution)
    _, target, source = struct.unpack_from(">III", patch, descriptor)
    if (title, media, version, source, target) != (TITLE_ID, MEDIA_ID, 2, 2, 0x202):
        raise ValueError("TU2 title/media IDs or source/target versions do not match")
    # The descriptor hashes the RSA signature; security.headerDigest is unrelated.
    signature = hashlib.sha1(base[security + 8:security + 264]).digest()
    if signature != patch[descriptor + 12:descriptor + 32]:
        raise ValueError("Patch source signature SHA-1 mismatch")


def link_or_copy(source, target):
    """Hardlink unchanged disc files; the staged folder is only read by the game."""
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def stage(base_dir, update_dir, output, patcher):
    if output.exists():
        raise ValueError("Output already exists; choose a new staging directory")
    base = (base_dir / "Default.xex").read_bytes()
    if hashlib.sha256(base).hexdigest() != DISC_SHA256:
        raise ValueError("Base-disc executable SHA-256 mismatch")
    manifest = json.loads((update_dir / "update-manifest.json").read_text())
    if (manifest.get("sha256"), manifest.get("title_id"), manifest.get("media_id"),
        manifest.get("version"), manifest.get("block_hashes_verified")) != (
            TU2_PACKAGE_SHA256, "394F07D4", "38E299CD", 2, True):
        raise ValueError("Expected the verified USA TU2 extraction")
    expected_files = {"Default.xexp", "CPKs/Patch.cpk", "CPKs/Patch2.cpk",
                      "CPKs/enUS_Patch.cpk", "CPKs/enUS_Patch2.cpk"}
    entries = manifest["files"]
    if len(entries) != 5 or {entry["path"] for entry in entries} != expected_files:
        raise ValueError("Unexpected TU2 file list")
    for entry in entries:
        data = (update_dir / entry["path"]).read_bytes()
        if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ValueError(f"Update file integrity failed: {entry['path']}")
    verify_patch_source(base, (update_dir / "Default.xexp").read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tu2-", dir=output.parent) as directory:
        work = Path(directory)
        patched = work / "Default.xex"
        subprocess.run([str(patcher.resolve()), str((base_dir / "Default.xex").resolve()),
                        str((update_dir / "Default.xexp").resolve()), str(patched)], check=True)
        patched_hash = hashlib.sha256(patched.read_bytes()).hexdigest()
        if patched_hash != TU2_SHA256:
            raise ValueError("Patched executable SHA-256 mismatch; no game directory staged")
        staged = work / "disc"
        shutil.copytree(base_dir, staged, copy_function=link_or_copy)
        # Unlink first: writing through a hardlink would modify the base disc.
        (staged / "Default.xex").unlink()
        shutil.copy2(patched, staged / "Default.xex")
        for entry in entries:
            if entry["path"] != "Default.xexp":
                target = staged / entry["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.unlink(missing_ok=True)
                shutil.copy2(update_dir / entry["path"], target)
        record = dict(manifest, source_mismatch_allowed=False, compatibility_verified=True,
                      base_executable_sha256=DISC_SHA256, patched_executable_sha256=patched_hash)
        (staged / "applied-update-manifest.json").write_text(json.dumps(record, indent=2) + "\n")
        staged.rename(output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--update", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--patcher", type=Path, required=True,
                        help="d3-patch executable with the corrected signature check")
    args = parser.parse_args()
    try:
        print(f"Staged verified TU2: {stage(args.base, args.update, args.output, args.patcher)}")
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"TU2 staging failed: {error}\n")


if __name__ == "__main__":
    main()
