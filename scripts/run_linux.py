#!/usr/bin/env python3
"""Launch the Linux base-disc port or its asset-free Vulkan diagnostics."""
import argparse
import hashlib
import os
import pathlib
import subprocess

from title_updates import DISC_SHA256, TU2_SHA256

ROOT = pathlib.Path(__file__).resolve().parents[1]


def option_value(extra, name, default):
    for index, arg in enumerate(extra):
        if arg.startswith(name + "="):
            return arg.split("=", 1)[1]
        if arg == name and index + 1 < len(extra):
            return extra[index + 1]
    return default


def saved_render_scale(state):
    try:
        value = int((state / "pc-render-scale.txt").read_text(encoding="utf-8").strip())
        return value if 1 <= value <= 3 else None
    except (OSError, ValueError):
        return None


def launch_command(args, extra):
    if args.probe or args.render_smoke:
        name = "d3-gpu-probe" if args.probe else "d3-render-smoke"
        return [str(ROOT / "port/out/build/linux-probe" / name), *extra]
    state = pathlib.Path(option_value(extra, "--user_data_root", str(args.state_dir.resolve())))
    if not state.is_absolute():
        state = ROOT / state
    state = state.resolve()
    defaults = {
        "--game_data_root": args.game_dir.resolve(),
        "--user_data_root": state,
        "--cache_root": state / "cache",
        "--log_file": state / "diablo3.log",
        "--render_target_path_vulkan": "fsi",
        "--mnk_mode": "true",
    }
    overrides = {arg.split("=", 1)[0] for arg in extra if arg.startswith("--")}
    extras = getattr(args, "extra_features", False)
    if extras and "--fullscreen" in overrides:
        defaults["--pc_use_saved_window_mode"] = "false"
    use_saved = option_value(extra, "--pc_use_saved_render_scale", "true").lower() not in {"false", "0"}
    if extras and overrides.intersection({"--resolution_scale", "--draw_resolution_scale_x", "--draw_resolution_scale_y"}):
        defaults["--pc_use_saved_render_scale"] = "false"
    if extras and use_saved and not overrides.intersection({"--resolution_scale", "--draw_resolution_scale_x", "--draw_resolution_scale_y"}):
        scale = saved_render_scale(state)
        if scale is not None:
            defaults["--resolution_scale"] = scale
    name = "linux-amd64-extras-relwithdebinfo" if extras else "linux-amd64-relwithdebinfo"
    if getattr(args, "title_update", None):
        name = "linux-amd64-tu2-extras-relwithdebinfo" if extras else "linux-amd64-tu2-relwithdebinfo"
        # TU2 reads patch assets through update:\ even with a prepatched XEX.
        defaults["--update_data_root"] = pathlib.Path(option_value(extra, "--game_data_root", str(args.game_dir.resolve())))
    return [str(ROOT / "port/out/build" / name / "diablo3"),
            *(f"{key}={value}" for key, value in defaults.items() if key not in overrides), *extra]


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--probe", action="store_true")
    mode.add_argument("--render-smoke", action="store_true")
    mode.add_argument("--extra-features", action="store_true",
                      help="Run the optional PC menu settings build and read its saved render scale")
    parser.add_argument("--title-update", choices=["tu2"], help="Run the separate verified TU2 build")
    parser.add_argument("--game-dir", type=pathlib.Path, default=ROOT / "game")
    parser.add_argument("--state-dir", type=pathlib.Path,
                        help="Override the user data directory (TU2 uses a separate default)")
    args, extra = parser.parse_known_args()
    if args.title_update:
        if args.probe or args.render_smoke:
            parser.error("--title-update cannot be combined with diagnostic modes")

    if args.state_dir is None:
        data_home = pathlib.Path(os.environ.get("XDG_DATA_HOME") or pathlib.Path.home() / ".local/share")
        args.state_dir = data_home / ("souls-of-the-reaper-tu2" if args.title_update else "souls-of-the-reaper")
    command = launch_command(args, extra)
    if not pathlib.Path(command[0]).is_file():
        flag = " --probe" if args.probe or args.render_smoke else ((" --title-update tu2" if args.title_update else "") + (" --extra-features" if args.extra_features else ""))
        parser.error("Executable missing; run scripts/build_linux.py" + flag)
    if not (args.probe or args.render_smoke):
        game = pathlib.Path(option_value(extra, "--game_data_root", str(args.game_dir.resolve())))
        if not game.is_absolute():
            game = ROOT / game
        if not (game / "Default.xex").is_file():
            parser.error("Game directory must contain Default.xex (Linux filenames are case-sensitive)")
        expected = TU2_SHA256 if args.title_update else DISC_SHA256
        if hashlib.sha256((game / "Default.xex").read_bytes()).hexdigest() != expected:
            parser.error(f"Executable SHA-256 does not match the {args.title_update or 'base-disc'} build; select the matching --title-update and --game-dir")
        state = pathlib.Path(option_value(extra, "--user_data_root", str(args.state_dir.resolve())))
        if not state.is_absolute():
            state = ROOT / state
        try:
            state.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            parser.exit(1, f"Cannot create user data directory: {error}\n")
    try:
        raise SystemExit(subprocess.run(command, cwd=ROOT).returncode)
    except KeyboardInterrupt:
        raise SystemExit(130) from None
    except OSError as error:
        parser.exit(1, f"Linux launch failed: {error}\n")


if __name__ == "__main__":
    main()
