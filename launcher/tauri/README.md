# Cross-platform launcher

A Tauri 2 launcher for Linux and Windows. It shows the same page as the
Windows `Launcher.ps1` (`../v6_borderless_final.html`) and replaces the
PowerShell/WebView2 host with Rust. Linux has been built and run on a desktop.
Windows has not been built or validated yet, and no complete game launch
through this launcher has been tested.

## What it offers

- **Game Version** lists the builds it finds: `Base`, `Base + Extras`, `TU2`
  and `TU2 + Extras`. A source checkout offers each built
  `port/out/build/<platform>-amd64[-tu2][-extras]-<config>` variant
  (RelWithDebInfo first, then Release and Debug). A release install uses the
  `diablo3`/`diablo3.exe` beside the launcher. Its TU2 flavour comes from
  `game/Default.xex`; extras builds are recognised from the executable itself.
- **Extras builds** add an `In-game` choice for Internal Resolution and
  Display. It keeps the value saved in the game's Options > Video menu. Any
  other choice overrides it for that launch
  (`--pc_use_saved_render_scale=false` / `--pc_use_saved_window_mode=false`).
- **Linux** shows the Vulkan render path (FSI, or the experimental Host path)
  and VSync. It hides Frame Rate and Image Scaling, because the Linux SDK has
  no frame limiter and is built without FidelityFX.
- **Windows** keeps the D3D12 options: ROV/RTV, frame limit and FSR/CAS.
- **Play** checks that `Default.xex` matches the selected build (base disc or
  staged TU2). For TU2 it also checks the four update CPKs and mounts the folder
  as `update:`. It then starts the game and closes the launcher. If the game
  exits during startup, the launcher stays open and shows the error.

Options are passed as command-line arguments; `diablo3.toml` and F4 keybinds
are never modified. Launch rules mirror `scripts/run_linux.py` and
`scripts/run_windows.ps1`.

## Build the game

In a source checkout, **Build from disc image…** (in ⚙) runs
[`scripts/build_client.py`](../../scripts/build_client.py). The panel also opens
by itself when no build is found. Choose:

- the USA Ultimate Evil Edition disc image (ISO);
- optionally, the USA Title Update 2 package (`tu00000002_00000000`);
- which builds to compile: Base, Base + Extras, TU2, TU2 + Extras.

The script writes into the game folders from Launch settings (`game/` and
`game-tu2/` by default). It runs these steps and skips any that are already done:

1. Check the disc's `Default.xex` before copying anything, then extract the disc.
2. Extract the title update, build the `d3-patch` tool and stage TU2. Unchanged
   disc files are hardlinked when both folders share a filesystem.
3. Build the pinned ReXGlue SDK once (`tools/`).
4. Generate code and compile each selected build. Generated code is reused for
   later builds of the same executable.

Progress and output appear in the panel. **Cancel build** stops the script and
the compilers it started, and so does closing the launcher; running it again
continues where it stopped. When the build finishes, Game Version lists the new
builds.

Requirements: Python 3, git, CMake 3.25+ (choose its path in the panel if it
is not on `PATH`), Ninja, Clang, and the SDK's Linux development packages.
Allow about 15 GiB of free space for a first build with TU2. On Windows the panel only
prepares the game folders; compile with the README steps. A release install
has no sources, so the panel is not shown there.

## Launch settings (⚙)

Game and save folders for base and TU2 builds, plus the Vulkan device on Linux.
Empty fields use these defaults:

| | Game folder | Save folder |
| --- | --- | --- |
| Base / extras | `game/` | Linux: `$XDG_DATA_HOME/souls-of-the-reaper`; Windows: `Documents\diablo3` |
| TU2 | `game-tu2/` | `…/souls-of-the-reaper-tu2`; Windows: `Documents\diablo3-tu2` |

Game folders sit at the repository root for a source checkout, or beside the
launcher for an install. Settings and the last menu choices are saved in
`launcher.json` under Tauri's app config directory
(`~/.config/org.soulsofthereaper.launcher` on Linux). Settings from the
first prototype move to the TU2 fields when it was set to TU2. The game's
output goes to `launcher-stdio.log` in the save folder; on Linux the game log
is `diablo3.log` there.

## Run and build

Install Rust, Node.js/npm and the
[Tauri prerequisites](https://v2.tauri.app/start/prerequisites/). Linux needs
the GTK 3 and WebKitGTK 4.1 development packages.

```sh
cd launcher/tauri
npm ci
npm run dev          # or: npm run build (Release, no installer bundle)
```

The debug executable is `src-tauri/target/debug/sotr-launcher`. Run it from
anywhere inside the checkout, or place a Release build beside an installed
`diablo3`. `prepare-ui.mjs` generates `dist/index.html` and icons; generated
files, dependencies and build outputs are ignored.

## Checks

```sh
npm test                                      # page tests (jsdom), both hosts
cargo test --manifest-path src-tauri/Cargo.toml   # discovery, validation, arguments
```

The page tests run both the generated Tauri page and the unmodified page as
`Launcher.ps1` loads it, so the original Windows launcher stays covered. The
Rust tests cover both platforms' arguments on any host.

## Known gaps

- On Wayland with NVIDIA, the launcher window captured blank in a desktop test
  but rendered correctly under XWayland (`GDK_BACKEND=x11`).
- Windows: untested. Focus hand-off to the game uses `AllowSetForegroundWindow`
  only; `Launcher.ps1` also polled and raised the game window.
- The Remap Controls screen still does not send its bindings, as in
  `Launcher.ps1`; use the in-game F4 menu.
- The installer and `setup.ps1` still package and start `Launcher.ps1`'s
  `SoulsOfTheReaper.exe`.
- Building needs a source checkout and Python. Compiling is automated only on
  Linux, and only the USA disc and USA TU2 are accepted.
