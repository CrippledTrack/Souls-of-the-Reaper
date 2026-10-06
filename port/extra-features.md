# Optional PC features

The extra-features build modifies the base game on Windows and Linux:

- The press-button launch screen appends ` + Extras` to the original version.
- An Options > Video left/right selector, `Render resolution (restart required)`,
  using the same native control as volume and region. Choose `1x`, `2x` or
  `3x`; the selection is saved immediately.
- The startup autosave warning replaces English `Xbox 360 console`,
  `Xbox 360 Console`, `Xbox 360` and `Xbox360` wording with `PC`.
  Other localized wording is preserved.

These features are disabled by default. CMake's `SOULS_ENABLE_EXTRA_FEATURES`
option defaults to `OFF`; the extra hooks are compiled and linked only when
it is `ON`. Both platforms use the same audited base-disc guest hooks. Normal
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
python3 scripts/build_linux.py --extra-features --skip-codegen \
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

## Base-disc adaptation

The previous implementation targeted the experimental TU6 executable. This
integration uses the unmodified USA Ultimate Evil Edition executable with
SHA-256 `cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef`,
validated by CMake for optional builds on both platforms. Regenerate Windows
sources from this same base disc before enabling the hooks.

The original options initialization, localization and vector helper
instruction sequences were compared across both images, allowing relocated
call targets and data addresses. The base-disc generated calls confirm the
string and descriptor helpers used here.

| Purpose | Previous address | Base-disc address |
| --- | --- | --- |
| Options initialization | `0x82703EF0` | `0x826FD808` |
| Selector choice count | — | `0x826FBFF0` |
| Selector initialization | — | `0x826FC030` |
| Selector value label | — | `0x826FC798` |
| Selector change handling | — | `0x826FCE80` |
| Localization | `0x82835EE0` | `0x8282F0F0` |
| Descriptor initialization | `0x82702450` | `0x826FBD68` |
| Descriptor construction | `0x82702518` | `0x826FBE30` |
| Video vector insertion | `0x827071C0` | `0x82700AD8` |
| String construction | `0x82CE2E40` | `0x82CDAD58` |
| String assignment | `0x82CE31D8` | `0x82CDB0F0` |
| String buffer access | `0x82CE1608` | `0x82CD9520` |
| String destruction | `0x82CE2100` | `0x82CDA018` |

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

## Validation

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

Tests cover save/load and failure handling, autosave wording, invalid settings,
command-line precedence, custom user-data directories and normal-build
isolation. The native logic test requires a C++23 compiler. Live menu layout,
label refresh, autosave wording and a relaunch at the saved scale still need
verification in a desktop game session.

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
