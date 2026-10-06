# Tauri launcher prototype

This test launcher reuses the existing HTML artwork and option controls with a
Tauri 2 Rust host. The original Windows launcher remains available. The Linux
prototype has been built and its frontend initialized on a desktop; Windows
and a complete game launch through this prototype have not been validated.

## Run from the repository

Install Rust, Node.js/npm and Tauri's platform prerequisites. Linux requires
GTK 3 and WebKitGTK 4.1 development packages; see the
[Tauri prerequisites](https://v2.tauri.app/start/prerequisites/).

```sh
cd launcher/tauri
npm ci
npm run dev
```

Open **Launch settings** and choose:

- The staged game directory containing the matching `Default.xex` and CPKs.
- The game executable for your platform, title update and extras selection.
- A save directory. The prototype defaults to its own TU2 directory.
- Title Update 2 and extra features as appropriate for that executable.
- Vulkan device (Linux only). The default is **1**.

For the shared TU2 extras development build, the Linux executable is
`port/out/build/linux-amd64-tu2-extras-relwithdebinfo/diablo3`. A Release build
can be selected directly using its executable path.

**Play** validates the executable's guest XEX identity, checks that TU2 CPKs
exist, saves launcher choices and starts the selected game executable. Game
arguments are passed directly, preserving paths containing spaces. TU2 mounts
the staged directory as both the game and update directories. Linux uses the
validated Vulkan FSI path and hides the D3D12 ROV/RTV choice.

The resolution selection explicitly overrides the extras build's saved scale
for that launch. Existing game configuration and keybindings are preserved;
launcher-managed choices are passed as runtime arguments. Use the game's F4
menu for key bindings. Launcher settings are stored in `launcher.json` under
Tauri's application configuration directory. `launcher-game.log` and
`launcher-stdio.log` are written to the selected save directory.

## Build and checks

```sh
npm run prepare-ui
npm test
cargo test --offline --manifest-path src-tauri/Cargo.toml
cargo build --offline --manifest-path src-tauri/Cargo.toml
```

The debug executable is `src-tauri/target/debug/sotr-launcher` on Linux.
After dependencies have been downloaded, these Cargo checks can run offline.
For a Release executable, use `npm run build`; installer bundles are disabled
for this prototype.

`prepare-ui.mjs` generates `dist/index.html` from the existing launcher HTML
and the Tauri adapter. It also generates platform icons from `icon.svg`.
Generated frontend assets, icons, dependencies and build outputs are ignored;
the npm and Cargo lockfiles are versioned.

Rust tests cover TU2 asset-mount arguments, explicit scale precedence, Vulkan
device selection, path handling and invalid scale rejection. Frontend bridge
checks cover forwarding launch choices and displaying launch failures.
The desktop check confirms frontend initialization, not a full gameplay test.
