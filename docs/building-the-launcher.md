# Building the launcher

The launcher is a Tauri app (Rust host + WebView page) in [`launcher/tauri/`](../launcher/tauri/README.md). It runs on Windows and Linux. (The earlier PowerShell/WebView2 `Launcher.ps1` has been removed.)

## Build

### Requirements

- [Rust](https://rustup.rs/) (stable) and Node.js with npm
- Windows: MSVC build tools (Visual Studio 2022 C++ workload) and the WebView2 runtime (already present on Windows 10/11 with Edge)
- Linux: GTK 3 and WebKitGTK 4.1 development packages, e.g.
  - Debian/Ubuntu: `sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev build-essential libssl-dev libayatana-appindicator3-dev librsvg2-dev`
  - Fedora: `sudo dnf install gtk3-devel webkit2gtk4.1-devel openssl-devel librsvg2-devel`
- See the [Tauri prerequisites](https://v2.tauri.app/start/prerequisites/) for other setups

### Build and run

The commands are the same on both platforms:

```sh
cd launcher/tauri
npm ci
npm run dev       # debug build, opens the launcher
npm run build     # Release build, no installer bundle
```

Outputs:

| Build | Windows | Linux |
| --- | --- | --- |
| Debug | `src-tauri\target\debug\sotr-launcher.exe` | `src-tauri/target/debug/sotr-launcher` |
| Release | `src-tauri\target\release\sotr-launcher.exe` | `src-tauri/target/release/sotr-launcher` |

`npm run icons` (run automatically) generates the icons; they and `target/` are git-ignored. The page itself is served as-is from `src/`.

### Where to run it

- **Source checkout**: run it from anywhere inside the checkout. It finds `port/out/build/...` variants and offers **Build from disc image…** (in the ⚙ menu), which drives [`scripts/build_client.py`](../scripts/build_client.py) exactly as described in [Building the game](building-the-game.md). It needs Python, Git, CMake 3.25+, Ninja and Clang available (see that guide).
- **No checkout**: a launcher built from a clean, pushed commit downloads the matching sources into the per-user data folder the first time you build (asking first) and builds from there; see [Without a checkout](../launcher/tauri/README.md#without-a-checkout). Set `SOTR_REPO` when building the launcher if `origin` is not the canonical repository.
- **Release install**: place the Release executable beside `diablo3` / `diablo3.exe`. The build panel is not shown there.

How the page and host are laid out, and how launcher text is translated, is
described in the launcher README: [Page layout](../launcher/tauri/README.md#page-layout)
and [Localization](../launcher/tauri/README.md#localization).

### Checks

```sh
npm test                                          # tsc type check, then the page tests (jsdom)
cargo test --manifest-path src-tauri/Cargo.toml   # discovery, validation, arguments, message keys
```

## Releasing a new stable version

A launcher outside a checkout builds a stable release by default, and its own
commit when Development is chosen (see
[Without a checkout](../launcher/tauri/README.md#without-a-checkout)). Which
release is stable is embedded when the launcher is built:

- **Release launchers** ([`launcher-release.yml`](../.github/workflows/launcher-release.yml)):
  publishing a release builds its launchers with `SOTR_STABLE_TAG` /
  `SOTR_STABLE_COMMIT` set to that release, so nothing needs editing first.
  A manual run of the workflow does the same unless `stable` is unticked, which
  keeps the pinned release below (for rebuilding an older release's launcher
  without changing what it builds, for example).
- **Every other build** (local, CI) uses [`launcher/stable.json`](../launcher/stable.json).
  Update it after a release so development launchers follow:

  ```sh
  git rev-parse beta-1.5^{commit}   # the commit to put in "commit"
  ```

Set both variables or neither; a malformed value or file fails the launcher
build.

## Windows installer (optional)

[`installer/SoulsOfTheReaper.iss`](../installer/SoulsOfTheReaper.iss) is an Inno Setup script that packages the game binaries and extracts game data from a user-supplied disc image during install:

```powershell
ISCC.exe installer\SoulsOfTheReaper.iss
```

The installer packages `launcher\tauri\src-tauri\target\release\sotr-launcher.exe` as `SoulsOfTheReaper.exe`, so run `npm run build` in `launcher/tauri` first (it also generates the installer icon, `src-tauri\icons\icon.ico`).
