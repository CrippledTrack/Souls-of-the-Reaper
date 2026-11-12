# Cross-platform launcher

The game's launcher, a Tauri 2 app for Linux, Windows and macOS (it replaced the
earlier PowerShell/WebView2 `Launcher.ps1`, which has been removed). Linux has been built and run on a desktop.
macOS (Apple Silicon) support is new and experimental; see [macOS](#macos).
On Windows 11 a Release launcher outside a source checkout has fetched its
sources, built the SDK and compiled the game through the Build panel (the same
`scripts/build_client.py` as Linux), then started the game with Play.

## What it offers

- **Game Version** lists the builds it finds: `Base`, `Base + Extras`, `update`
  and `Update + Extras`. `+ Extras` is the same executable launched with
  `--extra_features=true`. A source checkout offers each built
  `port/out/build/<platform>-amd64[-update][-plain]-<config>` variant
  (RelWithDebInfo first, then Release and Debug). A release install uses the
  `diablo3`/`diablo3.exe` beside the launcher. Its update flavour comes from
  `game/Default.xex`; whether it carries the extras is read from the executable itself. `-plain` builds are compiled without them and only offer the plain choice.
- **Extras builds** add an `In-game` choice for Internal Resolution and
  Display. It keeps the value saved in the game's Options > Video menu. Any
  other choice overrides it for that launch
  (`--pc_use_saved_render_scale=false` / `--pc_use_saved_window_mode=false`).
  They also add a **Button Icons** row (`Xbox`, `PlayStation` or `Auto`, saved
  with the other menu choices); `PlayStation` and `Auto` pass
  `--pc_button_glyphs=playstation|auto`. `Auto` (the default for new
  installs) picks the prompts from the controller connected when the game starts.
  Normal builds do not have the row, since the option only exists in extras builds.
- **Profile** chooses who player 1 is: the local profiles of the chosen Game
  Version's save folder (`game/profiles.rs`, the same files the game reads in
  `patches/profiles`). It is passed as `SOTR_PROFILE=<XUID>`. The last choice,
  **Choose in game** (always offered, saved as `ingame`), passes none, so the game
  asks player 1 at start. **Manage Profiles ›** makes, renames and deletes
  them (deleting asks first, and is only offered here). The first listing of a
  save folder names the saves made before profiles: the old default XUID gets
  the former Player Name setting (`username` in launcher.json), old co-op guest
  folders become `Guest2`–`Guest4`.
- **Frame Rate** and **VSync** apply to both renderers on every platform: the
  frame limiter (`--d3_frame_limit`, `patches/common/rexglue-frame-limit.patch`)
  and the game's 60 Hz internal timing (`--vsync`) live in the shared GPU
  command processor. VSync does not sync presentation to the monitor; On also
  holds the game to 60 FPS. `Auto` (the default) turns it on for caps up to
  60 FPS and off above that, which is what Direct3D 12 always did. Vulkan, which
  has no ROV path, offers the RTV frame rates (30, 60, 120, 144, 240; ROV stops
  at 144). The default is 120 FPS.
- **Linux** shows the Vulkan render path (FSI, or the experimental Host path)
  and Image Scaling (`Off`, `FSR` or `CAS`, passed as
  `--present_effect=bilinear|fsr|cas`). The spatial effects only change how the
  rendered image is scaled to the window, not the internal resolution, and need
  an SDK built with `patches/linux/rexglue-present-effects.patch`; rebuild the
  SDK after updating (older builds reject the flag). `Off` is the default.
- **Linux** launch settings (⚙) have a Display server choice: `Auto` passes
  nothing and lets SDL choose, `Wayland` and `X11` pass
  `--video_driver=wayland|x11`. It is saved with the other launch settings.
- **Shadow (beta)**: every build includes the Shadow renderer
  ([`experiments/gpu-layer`](../../experiments/gpu-layer/README.md)), a Vulkan
  renderer that draws the game itself. The menu offers it as a Render Path choice
  on Linux and a Renderer choice on Windows for any build that has it with its
  shader converter (XenosRecomp) staged beside the executable; releases from
  before it was built in (beta-1.4) do not have it. It passes
  `--shadow_renderer_only=true`. Because it presents its own picture and draws
  at any resolution directly, choosing it replaces Image Scaling and Internal
  Resolution with **Render Resolution** (`--shadow_render_height`) and **Aspect
  Ratio** (`--shadow_guest_width` / `--shadow_guest_height`), and on Windows
  hides Render Path. Render Resolution offers `Native` (the default: the height
  of the display the launcher is on), `720p` (the game's own), `1080p`, `1440p`,
  `2160p` and `Custom`, which adds a Custom Height box for any value from 360 to
  2880. Aspect Ratio offers `Auto` (the default: the display's shape), `16:9`,
  `16:10` and `21:9`. The game's display keeps 720 lines and widens for wider
  shapes, or keeps 1280 columns and grows taller for narrower ones, within the
  renderer's 2048x1536 limit: shapes wider than about 2.84:1 (32:9) are clamped
  and shown with bars. Native and Auto are worked out when the game starts and
  do not follow the window afterwards. **Dynamic Resolution** (`Off`/`On`,
  `--shadow_dynamic_resolution=true`) lets the renderer drop the 3D resolution
  below Render Resolution, down to the game's own, while the GPU falls behind;
  text and menus stay at the full resolution. The window title shows the frames
  the Shadow renderer actually presents (and the game's own rate beside it) and
  the resolution it is drawing at. Frame Rate, VSync and Display still apply.
  On macOS Shadow is the only renderer: it is always on, and there is no
  Renderer or Render Path row.
- **Windows** offers a Renderer choice. Direct3D 12 keeps ROV/RTV and FSR/CAS.
  Vulkan (`--gpu_backend=vulkan`) shows the same render path, device and Image
  Scaling options as Linux.
- **Play** checks that `Default.xex` matches the selected build (base disc or
  staged update). For update it also checks the four update CPKs and mounts the folder
  as `update:`. It then starts the game and closes the launcher. If the game
  exits during startup, the launcher stays open and shows the error.

## macOS

macOS runs the game through MoltenVK with the Shadow renderer only, so the menu
has no Renderer or Render Path row and every launch passes
`--shadow_renderer_only=true` with the Vulkan options Linux uses (no Display
server). A build without the Shadow renderer (a basic build) does not start
there. Builds come from [`scripts/build_macos.py`](../../scripts/build_macos.py)
(driven by `scripts/build_client.py` from the Build panel) and are found under
`port/out/build/macos-arm64[-update][-plain]-relwithdebinfo` (`macos-x86_64-…`
for a launcher built for Intel).

- **Tools**: the Xcode command line tools (`xcode-select --install`: Apple
  clang 16+, git) and, from Homebrew, `brew install cmake ninja vulkan-loader
  molten-vk`. A launcher opened from Finder starts with a short `PATH`, so it
  also looks in `/opt/homebrew/bin`, `/usr/local/bin` and `/opt/local/bin` and
  hands those folders to the build. Until the command line tools are installed,
  `/usr/bin/git`, `clang` and `python3` are only installer stubs and are not
  counted. The Build panel warns when no `MoltenVK_icd.json` is found under
  `/opt/homebrew/etc/vulkan/icd.d` (or the `/usr/local` and `share` versions).
- **Folders**: saves go to `~/Library/Application Support/souls-of-the-reaper[-update]`,
  settings to `~/Library/Application Support/org.soulsofthereaper.launcher/launcher.json`,
  and fetched sources (when the launcher's own folder is read-only) to
  `~/Library/Application Support/SoulsOfTheReaper/sources`.
- **Packaging**: the release asset (`sotr-launcher-macos-arm64.tar.gz`) is the
  bare executable, not a signed `.app`. Gatekeeper blocks a downloaded copy until
  the quarantine flag is cleared: `xattr -d com.apple.quarantine sotr-launcher`,
  or right-click > Open.

Options are passed as command-line arguments; `diablo3.toml` and F4 keybinds
are never modified. Launch rules mirror `scripts/run_linux.py` and
`scripts/run_windows.ps1`.

## Build the game

In a source checkout, or in a launcher built from a clean pushed commit (see
[Without a checkout](#without-a-checkout)), **Build from disc image…** (in ⚙) runs
[`scripts/build_client.py`](../../scripts/build_client.py). The panel also opens
by itself when no build is found. Choose:

- the USA Ultimate Evil Edition disc image (ISO);
- optionally, the USA Title Update package;
- what to build: **Title Update** (an overlay on the base game files, which are always extracted and never modified; ticking it compiles both the Base and Update executables) and **Optional PC features** (off compiles the `(no extras)` versions). A "Will compile:" line spells out the result;
- in a launcher outside a checkout, the **Version**: Stable or Development (see [Without a checkout](#without-a-checkout)).

The script writes into the game folders from Launch settings (`game/` and
`game/update/` by default; an existing `game-update/` is still used). It runs these steps and skips any that are already done:

1. Check the disc's `Default.xex` before copying anything, then extract the disc.
2. Extract the title update, build the `d3-patch` tool and stage update as a small
   `update/` overlay inside the game folder (about 27 MB: the patched executable and
   the update CPKs; the disc files are not copied).
3. Build the pinned ReXGlue SDK once (`tools/`).
4. Build the Shadow renderer's shader converter once: XenosRecomp at the commit
   pinned in `experiments/gpu-layer/xenosrecomp.lock.json`, with its patch, in
   `tools/XenosRecomp` (about 0.5 GB with its prebuilt DXC libraries).
5. Generate code and compile each selected build, then stage the converter in
   its `xenosrecomp/` folder. Generated code is reused for later builds of the
   same executable.

The launcher embeds the source revision it was built from (`build.rs` reads
`git rev-parse HEAD` and whether the tree has changes) and shows it in the
launch settings. Without git data it reads `unknown`; CI or a source archive
can set `SOTR_COMMIT` and `SOTR_DIRTY=0|1` instead. Release builds should set
`SOTR_REQUIRE_CLEAN=1`, which fails the build for a modified tree or an
unknown revision.

Before it runs anything, the launcher itself (not the Python scripts, since
Windows may have no Python) checks for Python 3, Git, Clang, CMake 3.25+, Ninja
and, on Windows, the Visual Studio C++ tools and `clang-cl` (part of the LLVM
installer; it builds the shader converter). The panel lists each one with a
download link when it is missing, and **Build** stays blocked until they are
found (**Check tools** rechecks after an install). On Windows with winget, the
panel also offers **Install missing tools with winget**, which installs each
missing one in turn (`Python.Python.3.13` per user and on `PATH`, `Git.Git`,
`LLVM.LLVM`, `Kitware.CMake` pinned to 3.31.8 because CMake 4 rejects old
third-party minimums, `Ninja-build.Ninja`, and last
`Microsoft.VisualStudio.2022.BuildTools` with the C++ workload and the Windows
SDK). Its note says that it accepts the packages' license agreements; Windows
asks for administrator approval for most of them. The tool check reads the user
and machine `PATH` from the registry, so tools installed while the launcher is
open are found without a restart. Clang, CMake and Ninja
installed in the usual Visual Studio or LLVM folders are found and passed on to
the build even when they are not on `PATH`. The ReXGlue SDK is not a tool you
install: when it is not built yet, the panel asks before fetching the pinned
commit (`port/base/rexglue.lock.json`) and building it, which takes about 3 GB
and 12+ minutes. The shader converter is handled the same way: its own row in
the tool list (pinned commit, not built yet or out of date), and the same
question covers it. Answering *Not now* downloads nothing; the script itself is
also told not to (`--no-sdk-download`, which refuses both) unless the panel got
a yes. A failed converter build fails the build like an SDK failure.

Progress and output appear in the panel. **Cancel build** stops the script and
the compilers it started, and so does closing the launcher; running it again
continues where it stopped. When the build finishes, Game Version lists the new
builds and selects the best of what was just built: Update over Base, and the
`+ Extras` build over the plain one. With no saved choice (a first start), the
launcher picks the same way among the builds it finds, the update only when its
files are installed.

The Clang found must be new enough (18+ on Linux, 22+ on Windows, Apple clang
16+ on macOS). Warnings that do not block Build are listed too: no Vulkan driver,
no X11 or Wayland development files (Linux), no MoltenVK (macOS), and under 15 GiB
free where the build runs.

The SDK state comes from `scripts/build_client.py --print-sdk-status`, so the
install paths live in one place. An SDK counts as built only when it matches the
pinned commit and the current `patches/` (stamped in `.sotr-sdk.json` in the
install folder). One built from another commit or patch set, or before stamps
existed, shows as out of date and is rebuilt after the same kind of approval.

### Without a checkout

A launcher outside a source checkout (a release download) fetches the sources
it builds. The Build panel's **Version** choice picks which:

- **Stable** (the default) builds the stable release embedded when the launcher is
  built: `SOTR_STABLE_TAG` and `SOTR_STABLE_COMMIT` when both are set, else
  [`launcher/stable.json`](../stable.json) (`tag` and full `commit`). The
  release workflow sets the variables to the release it builds, so a release's
  launcher builds that release; the file is the fallback for other builds. Game Version lists them with the tag, e.g.
  `Update + Extras (beta-1.4)`. Any launcher can build these, including one
  compiled from modified sources.
- **Development** builds the commit the launcher itself was built from, so it
  carries the launcher's newest work. It is tied to the launcher's own commit
  because an older launcher cannot know how to build newer sources. Game
  Version lists these as `Update + Extras (Development)`; they keep their own
  save folders (see [Launch settings](#launch-settings-)). The choice is only
  offered by a launcher compiled from a clean, fully pushed commit
  (`BuildInfo::fetchable`).

A launcher inside a checkout builds the checkout and shows no Version choice.
The Build panel's summary says which version a standalone build compiles.

On Build the launcher asks once to download the sources for that commit (and the
SDK, if needed), then clones them with `git fetch --depth 1` into
`build/sources/<commit>` beside the launcher, where `<commit>` is the first 12
characters of the commit id (a folder named after the full id, from earlier
launchers, is still used). So Stable and Development builds live in two
folders, each with its own SDK (about 3 GB and 12+ minutes to build once) and
shader converter, while both use the same game folders. A launcher in a folder it cannot write to uses
`SoulsOfTheReaper/sources/<commit>` under the per-user data folder instead
(`~/.local/share` on Linux, `%LOCALAPPDATA%` on Windows, `~/Library/Application Support` on macOS). Without git it
downloads GitHub's archive of that commit with `curl` and `tar` instead; git is
still required afterwards, because the build script clones the SDK with it. The
SDK, XenonRecomp, XenosRecomp and `port/out` live inside each folder. Delete a commit's
folder to start that one over.

Stable builds take the same launch options as newer ones: the game ignores
(and logs) options it does not know. Button Icons and the Shadow renderer are
only offered for builds whose sources have them (beta-1.4 has neither).

On Windows the clones turn on `core.longpaths`: the SDK checkout inside the
fetched sources passes the 260-character path limit when the launcher sits in
a deep folder. Keep the launcher's folder path short (under about 60
characters) all the same, since not every compiler tool handles long paths.

The remote comes from `SOTR_REPO` at launcher build time, defaulting to the
`origin` remote, so set it to the canonical repository for releases and make
sure the commit is pushed there.

Requirements: Python 3, git, CMake 3.25+ (choose its path in the panel if it
is not on `PATH`), Ninja and Clang. Linux also needs the SDK's development
packages. On Windows install Visual Studio 2022 with the C++ and CMake
workloads and LLVM Clang 22+; the usual install folders of Clang, CMake and
Ninja are added to `PATH` automatically, and the build runs
`scripts/build_windows.py` (the one-time SDK build and the compile both happen
in the panel). On macOS install the Xcode command line tools and Homebrew's
`cmake ninja vulkan-loader molten-vk`; the build runs `scripts/build_macos.py`
with Apple clang. Allow about 15 GiB of free space for a first build with update. A
release install beside `diablo3` has no sources and no panel.

## Launch settings (⚙)

Game folder, save folders for base, update and Development builds, and the Vulkan device (Linux, macOS, and Windows when the Vulkan renderer is selected). The title update has no game folder of its own: it uses the base game folder, whose `update/` folder holds the update (an older full `game-update/` copy is still used if present).
Empty fields use these defaults:

| | Game folder | Save folder |
| --- | --- | --- |
| Base / extras | `game/` | Linux: `$XDG_DATA_HOME/souls-of-the-reaper`; macOS: `~/Library/Application Support/souls-of-the-reaper`; Windows: `Documents\diablo3` |
| update | same as Base (`game/update/` overlay; or `game-update/` if that older full copy exists) | `…/souls-of-the-reaper-update`; Windows: `Documents\diablo3-update` |
| Development (base) | same as Base | `…/souls-of-the-reaper-development`; Windows: `Documents\diablo3-development` |
| Development (update) | same as update | `…/souls-of-the-reaper-update-development`; Windows: `Documents\diablo3-update-development` |

Development builds (standalone launchers only) never use the normal save
folders, even when those are set to a folder of your own: unfinished work
cannot touch your normal saves. Their own folders can be chosen under
Development builds in this panel. Save folders of the experimental builds
earlier launchers made (`…-experimental`) keep being used while no
`…-development` one exists, and their settings carry over.

The first time a Development build is played, its save folder has no saves
yet. If the matching normal folder (base for base, update for update) has some,
Play asks first: **Copy my saves** copies them over (profiles and in-game
settings; logs and shader caches are left out and rebuilt), **Start fresh**
leaves a `.launcher-started-fresh` marker so the question is not asked again,
and **Cancel** launches nothing. The normal saves are never changed. The copy is
made beside the folder first and moved in at the end, so an interrupted copy
asks again next time.

Installs made before the update was renamed keep working: a `game/tu2/` overlay or
`game-tu2/` folder, `…-tu2` save folders (used while the new-name folder does not
exist) and the old `gameDirTu2`/`userDataRootTu2` settings keys are all still read.
Rebuild once so the executables are named `…-update-…`.

Game folders sit at the repository root for a source checkout, or beside the
launcher for an install. Settings and the last menu choices are saved in
`launcher.json` under Tauri's app config directory
(`~/.config/org.soulsofthereaper.launcher` on Linux, `~/Library/Application Support/org.soulsofthereaper.launcher` on macOS). Settings from the
first prototype move to the update fields when it was set to the update. The game's
output goes to `launcher-stdio.log` in the save folder; on Linux and macOS the game log
is `diablo3.log` there.

## Run and build

Install Rust, Node.js/npm and the
[Tauri prerequisites](https://v2.tauri.app/start/prerequisites/). Linux needs
the GTK 3 and WebKitGTK 4.1 development packages; Windows needs the MSVC build
tools and the WebView2 runtime (included with Edge on Windows 10/11); macOS needs
the Xcode command line tools.

```sh
cd launcher/tauri
npm ci
npm run dev          # or: npm run build (Release, no installer bundle)
```

The debug executable is `src-tauri/target/debug/sotr-launcher` (`.exe` on
Windows); `npm run build` produces `src-tauri/target/release/sotr-launcher`. Run it from
anywhere inside the checkout, or place a Release build beside an installed
`diablo3`. The page is served straight from `src/` (see [Page layout](#page-layout));
`npm run icons` (run by `dev` and `build`) generates the icons. Generated files,
dependencies and build outputs are ignored.

## Page layout

The page is plain HTML, CSS and ES modules, served as they are (no bundler, no
compile step):

| Path | What it holds |
| --- | --- |
| `src/index.html` | Markup: the menu stage, the Launch settings and Build panels |
| `src/css/` | `base.css` (design, menu), `panels.css` (panels, status line) |
| `src/assets/` | The Old Fenris font, the background and the font credit image |
| `src/locales/` | Launcher text, one module per language (see [Localization](#localization)) |
| `src/js/main.js` | `start(window)`: builds the context and wires the modules together |
| `src/js/state.js` | Menu state restored from `SAVED_STATE`, and the rules derived from it |
| `src/js/menu.js`, `remap.js`, `rows.js`, `lang-popup.js` | The main menu, the Remap Controls view, a menu row, the language button |
| `src/js/launch-config.js` | What Play sends (`GameOptions` on the host) |
| `src/js/settings-panel.js`, `build-panel.js` | The two panels |
| `src/js/host.js`, `types.d.ts` | Tauri commands and events; the shared types, including every command's arguments |

Modules export functions and touch no DOM when they load, so the tests import
`main.js` and run `start()` against a jsdom window. The JavaScript carries
`// @ts-check` and JSDoc types: `npm test` runs `tsc` (no output files) first,
which catches unknown message keys, command names and argument shapes.

The Rust host is split the same way: `game/` (build discovery, folders,
validation, game arguments; no Tauri, so it is tested on any host), `tools/`
(the Build panel's tool check and winget installs), `commands/` (the Tauri
commands the page calls), `host.rs` (state and `launcher.json`), `job.rs` (one
build or install at a time), `message.rs` and `sources.rs`.

## Localization

All launcher text lives in `src/locales/<code>.js`, one object of named keys
per language. `en.js` is the source of truth: it defines every key, and any key
another locale leaves out shows in English. The launcher's language follows the
game language chosen with the language button (`ui` in `src/js/languages.js`).

- In JS, `t('key', { name })` returns the text with `{name}` filled in.
- Static markup carries the key instead: `data-i18n="key"` (text),
  `data-i18n-html="key"` (markup, when a text needs it) and
  `data-i18n-attr="title:key;aria-label:key"`. The English text stays in the
  HTML as written.
- The host never sends English. Errors, tool notes and warnings are
  `Message { key, params }` (`msg!("err.…", name = value)` in Rust); the page
  shows them with `t`. Detail nobody can translate (an I/O error, the build
  script's output) is `err.other` with the text in `{detail}`.

To translate, copy keys from `en.js` into the language's file and keep every
`{placeholder}`. To add text, add the key to `en.js` first. The tests check
that every key used by `msg!`, `index.html` and the locales exists in `en.js`,
and that translations keep the English placeholders. Key groups: `menu.*`,
`value.*`, `remap.*` (main screen), `settings.*`, `build.*`, `status.*`,
`common.*` (panels), `err.*`, `note.*`, `warn.*` (from the host).

## Checks

```sh
npm test                                          # tsc type check, then the page tests (jsdom)
cargo test --manifest-path src-tauri/Cargo.toml   # discovery, validation, arguments, message keys
```

The page tests (`test/*.test.mjs`) run the page in jsdom with a stubbed host.
The Rust tests cover every platform's arguments on any host.

## Known gaps

- On Wayland with NVIDIA, the launcher window captured blank in a desktop test
  but rendered correctly under XWayland (`GDK_BACKEND=x11`). On a later setup
  it stayed blank under XWayland too, with WebKitGTK printing `Failed to create
  GBM buffer`; `WEBKIT_DISABLE_DMABUF_RENDERER=1` made it draw.
- Windows: installing the tools by hand (download links, no winget) has not been
  tried on a machine that lacks them, and neither has a machine that already has
  CMake 4. Focus hand-off to the game uses `AllowSetForegroundWindow` only
  (the old PowerShell launcher also polled and raised the game window).
- The Remap Controls screen still does not send its bindings; use the in-game
  F4 menu.
- The Windows installer (`installer/SoulsOfTheReaper.iss`) and `setup.ps1`
  now package and start this launcher (as `SoulsOfTheReaper.exe`); neither has
  been rebuilt and run on Windows since the switch.
- Building needs Python 3 and git, from a source checkout or via the fetched
  sources above. Compiling is automated on Linux, Windows and macOS, and only the
  USA disc and USA update are accepted.
- macOS: the launcher has been built and its tests run on Apple Silicon; a full
  build from the Build panel, a launcher opened from Finder, and fetching sources
  there have not been tried yet. The release asset is unsigned and not notarized.
- Fetching sources has been tried on Linux and Windows. An SDK built before
  stamps existed is rebuilt once.
