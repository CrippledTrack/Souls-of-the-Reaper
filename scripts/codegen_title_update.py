#!/usr/bin/env python3
"""Generate verified TU2 guest sources for any supported host platform."""
import argparse
import hashlib
import json
import pathlib
import subprocess

from apply_generated_patches import patch_generated
from title_updates import TU2_SHA256, tu2_layout

ROOT = pathlib.Path(__file__).resolve().parents[1]


def generate_tu2(game_dir, rexglue, skip_codegen=False):
    game_dir = pathlib.Path(game_dir).resolve()
    xex = tu2_layout(game_dir)[2]
    if not xex.is_file() or hashlib.sha256(xex.read_bytes()).hexdigest() != TU2_SHA256:
        raise ValueError("Default.xex must match the verified USA TU2 executable")
    generated = ROOT / "port/generated/tu2"
    stamp = generated / "source-xex.sha256"
    if skip_codegen:
        if not stamp.is_file() or stamp.read_text().strip() != TU2_SHA256:
            raise ValueError("TU2 generated sources missing or stale; regenerate without --skip-codegen")
    else:
        # An interrupted regeneration must not leave a valid reuse stamp.
        stamp.unlink(missing_ok=True)
        manifest_dir = ROOT / "port/title_updates/tu2"
        manifest = (manifest_dir / "diablo3_manifest.toml").read_text()
        manifest = manifest.replace(json.dumps("../../../game-tu2/Default.xex"), json.dumps(xex.as_posix()))
        manifest = manifest.replace(json.dumps("../../../game-tu2"), json.dumps(game_dir.as_posix()))
        local = manifest_dir / "local_manifest.toml"
        local.write_text(manifest, encoding="utf-8")
        subprocess.run([str(rexglue), "codegen", str(local)], cwd=ROOT, check=True)
        generated.mkdir(parents=True, exist_ok=True)
    patch_generated(generated, "tu2")
    stamp.write_text(TU2_SHA256 + "\n")
    return generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=pathlib.Path, required=True)
    parser.add_argument("--rexglue", type=pathlib.Path, required=True)
    parser.add_argument("--skip-codegen", action="store_true")
    args = parser.parse_args()
    try:
        generate_tu2(args.game_dir, args.rexglue, args.skip_codegen)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"TU2 codegen failed: {error}\n")


if __name__ == "__main__":
    main()
