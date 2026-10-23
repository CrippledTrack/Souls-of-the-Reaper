#!/usr/bin/env python3
"""Generate verified update guest sources for any supported host platform."""
import argparse
import hashlib
import json
import pathlib
import subprocess

from apply_generated_patches import patch_generated
from title_updates import USA_UPDATE_SHA256, update_layout

ROOT = pathlib.Path(__file__).resolve().parents[1]


def generate_update(game_dir, rexglue, skip_codegen=False):
    game_dir = pathlib.Path(game_dir).resolve()
    xex = update_layout(game_dir)[2]
    if not xex.is_file() or hashlib.sha256(xex.read_bytes()).hexdigest() != USA_UPDATE_SHA256:
        raise ValueError("Default.xex must match the verified USA update executable")
    generated = ROOT / "port/generated/update"
    stamp = generated / "source-xex.sha256"
    if skip_codegen:
        if not stamp.is_file() or stamp.read_text().strip() != USA_UPDATE_SHA256:
            raise ValueError("update generated sources missing or stale; regenerate without --skip-codegen")
    else:
        # An interrupted regeneration must not leave a valid reuse stamp.
        stamp.unlink(missing_ok=True)
        manifest_dir = ROOT / "port/title_updates/update"
        manifest = (manifest_dir / "diablo3_manifest.toml").read_text()
        manifest = manifest.replace(json.dumps("../../../game-update/Default.xex"), json.dumps(xex.as_posix()))
        manifest = manifest.replace(json.dumps("../../../game-update"), json.dumps(game_dir.as_posix()))
        local = manifest_dir / "local_manifest.toml"
        local.write_text(manifest, encoding="utf-8")
        subprocess.run([str(rexglue), "codegen", str(local)], cwd=ROOT, check=True)
        generated.mkdir(parents=True, exist_ok=True)
    patch_generated(generated, "usa")
    stamp.write_text(USA_UPDATE_SHA256 + "\n")
    return generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=pathlib.Path, required=True)
    parser.add_argument("--rexglue", type=pathlib.Path, required=True)
    parser.add_argument("--skip-codegen", action="store_true")
    args = parser.parse_args()
    try:
        generate_update(args.game_dir, args.rexglue, args.skip_codegen)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"update codegen failed: {error}\n")


if __name__ == "__main__":
    main()
