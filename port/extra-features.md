# Optional PC features

The extra-features build modifies the base game on Windows and Linux, and the
verified USA TU2 build:

- The press-button launch screen appends ` + Extras` to the original version.
- An Options > Video left/right selector, `Render resolution (restart required)`,
  using the same native control as volume and region. Choose `1x`, `2x` or
  `3x`; the selection is saved immediately.
- An Options > Video `Window mode` selector with `Windowed` and
  `Borderless fullscreen` choices. Changes are saved and applied on the UI
  thread without restarting. F11 toggles the current mode without changing
  the saved preference.
- The startup autosave warning replaces English `Xbox 360 console`,
  `Xbox 360 Console`, `Xbox 360` and `Xbox360` wording with `PC`.
  Other localized wording is preserved.

These features are disabled by default. CMake's `SOULS_ENABLE_EXTRA_FEATURES`
option defaults to `OFF`; the extra hooks are compiled and linked only when
it is `ON`. The base disc and TU2 use separate audited address maps with shared hook logic. Normal
builds retain their existing game menus, autosave wording and version label.

## Windows

Use the existing Windows SDK and codegen steps from the main README, including
`port\apply_generated_patches.ps1`. Then build and launch:

```powershell
pwsh -File port\build.ps1 -Config RelWithDebInfo -ExtraFeatures
pwsh -File scripts\run_windows.ps1 -Config RelWithDebInfo -ExtraFeatures
```

`-Precompiled` remains available on the build script. The optional binary is
`port\out\build\win-amd64-extras-relwithdebinfo\diablo3.exe`, separate from
normal output. Debug and Release use `win-amd64-extras-<config>` directories.

The source-build launch script defaults to `game\` and `Documents\diablo3`.
Use `-GameDir` and `-UserDataRoot` to select other directories. `-ResScale 2`
explicitly overrides the saved in-game selection for that launch. The existing
graphical launcher continues to select the normal executable; use the new
script or launch the optional executable directly to test modified builds.

## Linux

After installing the Linux SDK and generating the base-disc sources:

```sh
python3 scripts/build_linux.py --extra-features \
  --game-dir /path/to/extracted/disc
python3 scripts/run_linux.py --extra-features \
  --game-dir /path/to/extracted/disc --vulkan_device=1
```

Use `--sdk-prefix /path/to/installed/sdk` to reuse an existing SDK. Omit
`--skip-codegen` for the first build or after changing codegen inputs.
The optional executable is
`port/out/build/linux-amd64-extras-relwithdebinfo/diablo3`; the normal executable
remains in `port/out/build/linux-amd64-relwithdebinfo/diablo3`.
For direct CMake builds on either platform, use a separate build directory and
pass `-DSOULS_ENABLE_EXTRA_FEATURES=ON` and
`-DSOULS_BASE_XEX=/path/to/extracted/disc/Default.xex`. Generate and patch that
platform's sources first. CMake verifies the executable hash and required
hook function mappings.

Both Linux launch modes use the existing user-data directory selected by
`--state-dir` or `--user_data_root`. The optional launcher reads
`pc-render-scale.txt` there and supplies `--resolution_scale=N` at startup.
The normal launcher ignores this file. A missing optional executable produces
an error rather than launching the normal build.

### Linux TU2

Use the staged TU2 executable and assets with both flags:

```sh
python3 scripts/build_linux.py --title-update tu2 --extra-features \
  --sdk-prefix /path/to/rexglue-install \
  --game-dir /path/to/updated-tu2-disc \
  --cmake /path/to/cmake
python3 scripts/run_linux.py --title-update tu2 --extra-features --vulkan_device=1 \
  --game-dir /path/to/updated-tu2-disc
```

The optional binary is `port/out/build/linux-amd64-tu2-extras-relwithdebinfo/diablo3`.
It shares TU2 generated sources and the TU2 user-data directory with the normal
TU2 build. The normal build ignores the saved render-scale file. For direct
CMake builds, enable both `SOULS_TITLE_UPDATE_2` and `SOULS_ENABLE_EXTRA_FEATURES`
and set `SOULS_BASE_XEX` to the patched TU2 `Default.xex`. Its required SHA-256 is
`447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f`.
Windows TU2 uses the same shared codegen and hooks; Windows build/runtime validation is pending.

## Saved settings and game behavior

The optional game itself reads the selection from its resolved user-data
directory before graphics setup, so direct executable launches work too.
Normal builds omit this startup behavior. On Linux, the optional app loads
the GPU plugin before applying settings, because the plugin registers the
resolution cvars.

The saved setting takes effect after closing and relaunching the optional
build. The GPU caches are initialized at startup; the menu does not change
resolution live. Explicit `--resolution_scale`, `--draw_resolution_scale_x`
or `--draw_resolution_scale_y` arguments passed through the Linux launch script
override the saved setting. Windows uses `-ResScale` for the same purpose.
When launching the optional executable directly, add
`--pc_use_saved_render_scale=false` alongside explicit scale arguments to
override the menu selection. This flag exists only in optional builds.
The tooltip displays the active axis scales and saved selection. A failed
save keeps the previous selection and reports an error.

Settings are written to a temporary file and replaced on success. Windows uses
[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
with replacement enabled and wide paths; Linux uses a same-directory rename.

The autosave override changes the temporary guest string returned by the
existing localization function, using the game's string-assignment API.
It applies only to `ConsoleUI:AutosaveWarningScreenText_XBox360`. Game assets
and console save contents are untouched.

## Window mode

Optional builds save the menu preference as `pc-window-mode.txt` in the resolved
user-data directory (`1` for windowed, `2` for borderless fullscreen). Missing or
invalid files keep the launch mode. A failed save restores the previous menu
selection. Normal builds do not read or write this preference.

The menu uses action ID 101 and the same audited native selector hooks as render
scale. Guest callbacks save and queue changes; the window dialog applies them on
the UI thread. The current mode is synchronized with F11 when initializing the
selector. Fullscreen uses the SDK's borderless window mode; there is no separate
exclusive-fullscreen choice.

On Linux, explicit `--fullscreen=true` or `--fullscreen=false` passed through
`run_linux.py --extra-features` overrides the saved preference for that launch.
On Windows, use `run_windows.ps1 -ExtraFeatures -WindowMode Windowed` or
`-WindowMode Borderless`. Direct executable launches can override the preference
with `--pc_use_saved_window_mode=false --fullscreen=true` (or `false`).

Headless tests cover both selector labels and counts, changes, failed-save
rollback, invalid indices, persistence, and Linux launch overrides. Desktop
switching and Windows runtime validation remain pending.

## Base-disc address audit

The base-disc hooks use the unmodified USA Ultimate Evil Edition executable with
SHA-256 `cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef`,
validated by CMake for optional builds on both platforms. Regenerate Windows
sources from this same base disc before enabling the hooks.

The base-disc generated calls confirm the string and descriptor helpers used
here. The TU2 address audit below records the corresponding updated functions.

| Purpose | Base-disc address |
| --- | --- |
| Options initialization | `0x826FD808` |
| Selector choice count | `0x826FBFF0` |
| Selector initialization | `0x826FC030` |
| Selector value label | `0x826FC798` |
| Selector change handling | `0x826FCE80` |
| Localization | `0x8282F0F0` |
| Descriptor initialization | `0x826FBD68` |
| Descriptor construction | `0x826FBE30` |
| Video vector insertion | `0x82700AD8` |
| String construction | `0x82CDAD58` |
| String assignment | `0x82CDB0F0` |
| String buffer access | `0x82CD9520` |
| String destruction | `0x82CDA018` |

The Video vector remains at owner + 128. The descriptor action ID is at +32;
ID 100 selects the added row. The base-disc options owner pointer is at
`0x83302514`, with its dirty flag at +344. The autosave localization key was
confirmed at `0x820A4868`; matching uses its text rather than its address.

The hooks delegate existing game functions and preserve the guest context
around injected calls. The menu insertion uses the original vector helper
which copies and destroys the temporary descriptor. The descriptor constructor
consumes the temporary key strings. Normal and optional builds share generated functions and
disc inputs on each platform; only the optional host links the menu, selector,
localization and version hooks.

The render row uses descriptor type 1 (the native left/right selector), with
three zero-based choices mapped to scales 1–3. The selector model at owner + 4
stores the current option ID at +4. Its vtable at `0x820AB768` supplies the
choice count and label callbacks. Initialization uses `0x82749C10` without
notification; user changes enter `0x826FCE80`, resolve the control via
`0x8246C520`, and read its selected index at +472. The callback bounds-checks
the active descriptor through owner +296/+12, saves the setting and marks
the model dirty. A failed save restores the prior selector value without
firing another change event. Value-label updates retain the original row
highlighting and set `SelectorTemplateText` at `0x8207F9A4` to `1x`, `2x` or `3x`.
All unrelated option IDs delegate the original game callbacks.

The version hook delegates the original 1024-byte formatter at `0x823BD130`
and appends the suffix only for the start-screen call returning to `0x8247C004`
with the `%s` format at `0x82003ED0`. That call supplies the build string to
`Root.NormalLayer.ConsoleStart_main.LayoutRoot.Version` (handle `0x832ED860`).
It preserves the returned guest context, checks buffer capacity, and leaves
other formatting calls and the game's actual version data untouched.

## TU2 address audit

[`pc_features_tu2.h`](src/features/pc_features_tu2.h) remaps guest functions for
TU2 only. [`extra-features-audit.json`](title_updates/tu2/extra-features-audit.json)
records the instruction-context matches, corroborating generated call sites,
and the unchanged descriptor/control layouts.

| Purpose | Base disc | TU2 |
| --- | --- | --- |
| Options initialization | `0x826FD808` | `0x82703EF0` |
| Selector count / initialization | `0x826FBFF0` / `0x826FC030` | `0x827026D8` / `0x82702718` |
| Selector label / change | `0x826FC798` / `0x826FCE80` | `0x82702E80` / `0x82703568` |
| Localization | `0x8282F0F0` | `0x82835EE0` |
| Options owner | `0x83302514` | `0x8330340C` |
| Selector text path | `0x8207F9A4` | `0x82080A1C` |
| Start-screen formatting return | `0x8247C004` | `0x8247E340` |

The formatter and `%s` string remain at `0x823BD130` and `0x82003ED0`.
CMake validates the selected executable hash and all seven hooks plus their
helper mappings before enabling the feature source.

## Validation

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

Tests cover save/load and failure handling, autosave wording, invalid settings,
command-line precedence, custom user-data directories and normal-build
isolation. The native logic test requires a C++23 compiler. Desktop visual checks and a relaunch at a newly saved scale remain to be
verified. The TU2 device-1 runtime log confirms the option was added, saved 2×
was applied at startup, PC autosave wording was used, and a selection of 1×
was saved during a clean 97-second run.

An SDK integration test verifies applying the saved scale after GPU cvar
registration, explicit override handling and invalid-setting fallback. It needs
an installed SDK, but no disc, generated guest code or graphics device:

```sh
cmake -S tests/sdk -B port/out/tests/pc-startup -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build port/out/tests/pc-startup
ctest --test-dir port/out/tests/pc-startup --output-on-failure
```

When the platform's generated PPC header is available, this project also builds
a headless guest-hook test. It checks the three selector choices, `1x`/`2x`/`3x`
labels, preservation of the returned guest context, delegation for existing
options, and bounds checks for the active descriptor using simulated helpers.

The same CMake test project supports the Windows SDK. Native Windows
compilation and desktop visual checks require a Windows development machine.

To run those SDK tests against TU2 generated hooks, configure a separate test
build with `-DSOULS_TEST_TITLE_UPDATE_2=ON`. Both variants test the version suffix
and English autosave wording in addition to selector behavior.
