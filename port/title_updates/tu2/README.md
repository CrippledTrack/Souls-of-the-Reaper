# USA Title Update 2

This build targets the verified TU2 for title `394F07D4`, media `38E299CD`.
The package version is `2`; the executable changes from `0x2` to `0x202`
(displayed by ReXGlue as `0.0.2.2`). Native compilation does not establish
gameplay compatibility. The recorded runtime checks used `--vulkan_device=1`
to select the discrete NVIDIA GPU on the validation system.

| Input | SHA-256 |
| --- | --- |
| Original `Default.xex` | `cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef` |
| TU2 package | `faf9feaa28e75035e61a960231d2c27f0bed3b7676db65747fa25352f19bddf3` |
| Patched `Default.xex` | `447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f` |

## Game folder layout

By default TU2 is a small overlay inside the base game folder: `game/tu2/` holds
the patched `Default.xex`, the four update CPKs and `applied-update-manifest.json`
(about 27 MB). `game/` stays the unmodified disc and is the game root; `game/tu2`
is mounted as `update:`, and TU2 builds load `game:\tu2\Default.xex` when it
exists. Create it with `scripts/build_client.py`, or with
`scripts/stage_title_update.py --base game --update ... --patcher ...` (no
`--output`). Pass `--game-dir game` wherever a TU2 command takes `--game-dir`.

The older layout, a complete separately staged folder (`--output`, or
`build_client.py --game-dir-tu2`), is still detected and works unchanged.
Saves stay separate either way.

## Linux build and launch

Stage the extracted TU2 into a separate disc directory containing the patched
executable and all four update CPKs, using the staging command below. The
original disc and TU package remain intact. `applied-update-manifest.json` records input and
output hashes and verified source compatibility.

Build and run from the Souls of the Reaper repository:

```sh
python3 scripts/build_linux.py --title-update tu2 \
  --sdk-prefix /path/to/rexglue-install \
  --game-dir /path/to/updated-tu2-disc \
  --cmake /path/to/cmake
python3 scripts/run_linux.py --title-update tu2 --vulkan_device=1 \
  --game-dir /path/to/updated-tu2-disc
```

After codegen, `--skip-codegen` reuses TU2 sources only if their recorded
executable hash matches. Output is `port/out/build/linux-amd64-tu2-relwithdebinfo/diablo3`;
generated sources are in `port/generated/tu2`. The launcher's default
user-data directory is `$XDG_DATA_HOME/souls-of-the-reaper-tu2` or
`~/.local/share/souls-of-the-reaper-tu2`; `--state-dir` can override it.
Both launch modes verify the executable hash to prevent using one build with
the other executable. Update files are already merged into the staged disc. The launcher mounts that
folder as both `game:` and `update:` so the guest can find the patch CPKs. The
staged folder contains no XEXP, so the executable is not patched a second time.

Keyboard input and F11 work through the shared application/SDK controls.
Mouse input remains opt-in with `--mnk_mouse=true`. Add `--extra-features` to the launch command for the Video
render-resolution selector, PC autosave wording, and ` + Extras` version suffix.
The code is built into the normal `linux-amd64-tu2-relwithdebinfo/diablo3` under
`port/out/build`; `--no-extra-features` at build time produces
`linux-amd64-tu2-plain-relwithdebinfo` without it. Both use the same TU2 user-data directory. See the
[extra-feature audit](extra-features-audit.json) and
[feature documentation](../../extra-features.md). Windows builds use the shared TU2 codegen and hooks; native Windows validation is pending.

## Reproduce staging

`scripts/build_client.py --iso ... --title-update tu00000002_00000000` runs
every step below. It is the same as the launcher's Build panel.

The old external `d3-patch` wrapper compared `digestSource` with the unrelated
`headerDigest` field. The correct comparison is SHA-1 of the base executable's
256-byte RSA signature. The fix is tracked in
[`patches/d3-patch-signature.patch`](../../../patches/d3-patch-signature.patch).
The corrected source is now in [`scripts/d3-patch`](../../../scripts/d3-patch).
`build_client.py` builds it against the XenonRecomp revision pinned in
`xenonrecomp.lock.json`, into `tools/d3-patch/`.
No mismatch override is used for TU2.

`scripts/extract_update.py` extracts the STFS package. It checks each block's
SHA-1 and writes `update-manifest.json`:

```sh
python3 scripts/extract_update.py tu00000002_00000000 /path/to/extracted-update
```

For another copy of the staged assets, choose a new output directory. Base
files are hardlinked when possible; replaced files are unlinked first, so the
base disc is never modified:

```sh
python3 scripts/stage_title_update.py \
  --base /path/to/base-disc \
  --update /path/to/extracted-update \
  --patcher /path/to/d3-patch \
  --output /path/to/new/updated-tu2-disc
```

The staging script checks the original executable, extraction manifest, every
update file, source identity/version/signature, and patched executable hash
before publishing the staged directory. Existing outputs are not overwritten.

## Address changes

The shared guest fixes move as follows:

| Hook | Base disc | TU2 |
| --- | --- | --- |
| setjmp | `0x831583B0` | `0x83161AD0` |
| longjmp | `0x83158680` | `0x83161DA0` |
| menu Exit | `0x82632E00` | `0x826374E8` |

[`relocations.json`](relocations.json) records the relocated function hints.
Code-context matching locates most entries; special cases are identified in
the record. All leaf hints have code-pointer references in the TU2 read-only
data. This is static corroboration, not a runtime validation of every entry.
The TU2-specific leaf extent at `0x82517CA0` includes its shared return block
at `0x82517CB4`, resolving the analyzer's backward-branch validation failure.

## Validation

Source compatibility, the patched executable, and all four update CPK hashes
were verified. TU2 codegen validation and the RelWithDebInfo native build pass;
24 regression tests pass, including source-signature rejection and relocated
hook patching. The device-1 runtime check selected the NVIDIA RTX 5070 Ti,
produced guest draws and swaps, and closed cleanly after approximately 52 seconds
without an unhandled guest fault. It was configured for 120 seconds and ended
before the timeout. Full gameplay and save/load behavior remain unverified.
See [`validation.json`](validation.json) for the recorded scope. Runtime checks
used the existing external investigation SDK with the Linux keyboard patch;
a fresh minimal SDK build has not been tested with TU2.

The TU2 extras build also passes both SDK hook/startup tests; the base-disc
variant passes the same two tests. Its device-1 desktop run closed cleanly
after approximately 97 seconds of a configured 120 seconds. Logs confirm
applying a saved 2× scale, adding the Video resolution option, PC autosave
wording, and saving a selection of 1×. A desktop relaunch at that newly saved
scale has not been performed; startup application is covered by SDK tests.

## Windows and shared configuration

TU2 manifests, address audits, generated sources (`port/generated/tu2`), and
optional hooks are shared across host platforms. The SDK must support the
manifest's 0.10 API. TU2 supplies its own UI-thread exit handler on every host; it does not depend
on the base-disc SDK exit patch.

Generate and build on Windows with a matching SDK codegen executable:

```powershell
python scripts/codegen_title_update.py --game-dir C:/games/updated-tu2-disc --rexglue C:/sdk/bin/rexglue.exe
cmake --preset win-amd64-relwithdebinfo -S port -B port/out/build/win-amd64-tu2-relwithdebinfo -DSOULS_TITLE_UPDATE_2=ON -DSOULS_ENABLE_EXTRA_FEATURES=ON -DSOULS_BASE_XEX=C:/games/updated-tu2-disc/Default.xex -DCMAKE_PREFIX_PATH=C:/sdk
cmake --build port/out/build/win-amd64-tu2-relwithdebinfo
pwsh -File scripts/run_windows.ps1 -TitleUpdate tu2 -ExtraFeatures -GameDir C:/games/updated-tu2-disc
```

Alternatively, `port/build.ps1 -TitleUpdate tu2 -GameDir ... [-Plain]`
runs shared codegen before building (the SDK's codegen executable must already
be built; `-Rexglue C:/sdk/bin/rexglue.exe` overrides its location). TU2 binaries and default save directories remain separate from base
builds on both platforms. Native Windows build/runtime testing is pending.

