//! Game builds: discovery (source checkouts and release installs), the
//! update overlay layout and game-folder validation.

use super::*;
use crate::message::{msg, Message};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    fs,
    path::{Path, PathBuf},
};

/// Verified guest executables (scripts/title_updates.py).
pub const BASE_HASH: &str = "cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef";

pub const USA_UPDATE_HASH: &str = "447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f";

/// Update assets a staged update folder must hold (scripts/stage_title_update.py).
pub const UPDATE_CPKS: [&str; 4] = [
    "Patch.cpk",
    "Patch2.cpk",
    "enUS_Patch.cpk",
    "enUS_Patch2.cpk",
];

/// Only executables compiled with the optional PC features register this cvar.
pub(crate) const EXTRAS_MARKER: &[u8] = b"pc_use_saved_render_scale";

/// Only executables with the Shadow renderer register this cvar (every build since
/// it became part of the normal build; none from releases before it).
pub(crate) const SHADOW_MARKER: &[u8] = b"shadow_renderer_only";

/// Source file of the Shadow renderer, to tell from a source tree.
const SHADOW_SOURCE: &str = "port/src/experiments/shadow_renderer.cpp";

/// Only executables with the Button Icons option (`--pc_button_glyphs`) register
/// this cvar; releases before it do not.
pub(crate) const GLYPHS_MARKER: &[u8] = b"pc_button_glyphs";

/// Source file that defines that cvar, to tell from a source tree without
/// reading its executables.
const GLYPHS_SOURCE: &str = "port/src/features/pc_features.cpp";

/// Id suffix of builds from a standalone launcher's Development sources.
pub(crate) const DEVELOPMENT_SUFFIX: &str = "-development";

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Build {
    pub id: String,
    pub label: String,
    pub update: bool,
    /// Launch with `--extra_features`. An executable that carries the features
    /// is offered twice, with and without this, so the plain game stays one click away.
    pub extras: bool,
    /// Built by a standalone launcher from its Development sources (its own commit)
    /// rather than the stable release. These keep their own save folders.
    pub development: bool,
    /// The Shadow renderer can be chosen: the executable has it and its shader
    /// converter (XenosRecomp) is staged beside it.
    pub shadow: bool,
    /// The executable takes `--pc_button_glyphs` (extras builds from releases
    /// before Button Icons do not).
    pub glyphs: bool,
    pub executable: PathBuf,
    /// Release installs pair the executable with the game folder beside it.
    pub game_dir: Option<PathBuf>,
}

pub(crate) fn variant(update: bool, extras: bool) -> (&'static str, &'static str) {
    match (update, extras) {
        (false, false) => ("base", "Base"),
        (false, true) => ("extras", "Base + Extras"),
        (true, false) => ("update", "Update"),
        (true, true) => ("update-extras", "Update + Extras"),
    }
}

pub(crate) fn make_build(update: bool, extras: bool, executable: PathBuf, game_dir: Option<PathBuf>) -> Build {
    let (id, label) = variant(update, extras);
    Build {
        id: id.into(),
        label: label.into(),
        update,
        extras,
        development: false,
        shadow: false,
        glyphs: false,
        executable,
        game_dir,
    }
}

/// Folder names the overlay is staged under, newest first.
pub(crate) const OVERLAY_NAMES: [&str; 2] = ["update", "tu2"];

/// Where an update game folder keeps its files: `(update root, executable)`. The overlay
/// layout puts the patched executable and update CPKs in `<game>/update` beside the
/// unmodified disc; otherwise the folder is a complete staged copy.
pub fn update_layout(game: &Path) -> (PathBuf, PathBuf) {
    // The overlay was staged as `tu2` before it was renamed; that folder still works.
    for name in OVERLAY_NAMES {
        let overlay = game.join(name);
        if overlay.join("Default.xex").is_file() {
            return (overlay.clone(), overlay.join("Default.xex"));
        }
    }
    (game.to_path_buf(), game.join("Default.xex"))
}

pub fn sha256_file(path: &Path) -> Result<String, String> {
    let data = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(format!("{:x}", Sha256::digest(&data)))
}

/// What an executable was compiled with, as read from the executable itself or,
/// for a source tree, from its sources.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct Markers {
    pub extras: bool,
    /// Has the Shadow renderer (whether its converter is staged is checked apart).
    pub shadow: bool,
    pub glyphs: bool,
}

pub(crate) fn executable_markers(executable: &Path) -> Markers {
    let has = |data: &[u8], marker: &[u8]| data.windows(marker.len()).any(|w| w == marker);
    fs::read(executable)
        .map(|data| Markers {
            extras: has(&data, EXTRAS_MARKER),
            shadow: has(&data, SHADOW_MARKER),
            glyphs: has(&data, GLYPHS_MARKER),
        })
        .unwrap_or_default()
}

/// The tree's sources define the Button Icons option.
fn tree_has_glyphs(root: &Path) -> bool {
    fs::read_to_string(root.join(GLYPHS_SOURCE)).is_ok_and(|text| text.contains("pc_button_glyphs"))
}

/// The shader converter the Shadow renderer runs, staged beside the executable
/// (scripts/shader_converter.py).
pub(crate) fn converter_staged(executable: &Path, platform: Platform) -> bool {
    let name = match platform {
        Platform::Windows => "XenosRecomp.exe",
        Platform::Linux | Platform::MacOs => "XenosRecomp",
    };
    executable.parent().is_some_and(|dir| dir.join("xenosrecomp").join(name).is_file())
}

/// Adds the builds an executable offers: plain, and with extras when it was
/// compiled with the optional PC features. Earlier entries win a duplicate.
/// `label` is appended to the variant's label, e.g. " (beta-1.4)"; `development`
/// marks builds from a standalone launcher's Development sources (own ids).
pub(crate) fn push_builds(
    builds: &mut Vec<Build>,
    update: bool,
    markers: Markers,
    executable: &Path,
    game_dir: Option<&Path>,
    label: &str,
    development: bool,
) {
    for extras in [false, true] {
        if (extras && !markers.extras)
            || builds.iter().any(|b| b.update == update && b.extras == extras && b.development == development)
        {
            continue;
        }
        let mut build = make_build(
            update,
            extras,
            executable.to_path_buf(),
            game_dir.map(Path::to_path_buf),
        );
        build.label += label;
        build.glyphs = extras && markers.glyphs;
        build.shadow = markers.shadow;
        if development {
            build.id += DEVELOPMENT_SUFFIX;
            build.development = true;
        }
        builds.push(build);
    }
}

/// A fetched source tree a standalone launcher builds in.
pub struct SourceTree<'a> {
    pub root: &'a Path,
    /// Appended to the labels of its builds: " (beta-1.4)", " (Development)".
    pub label: &'a str,
    /// The Development sources (the launcher's own commit).
    pub development: bool,
}

/// The builds compiled in one source tree: every
/// `port/out/build/<platform>-amd64[-update][-plain]-<config>` variant (`-plain` was
/// compiled without the extras). Folders of the retired `-experimental` profile are
/// not listed: every build has the Shadow renderer now.
fn discover_tree(builds: &mut Vec<Build>, platform: Platform, root: &Path, label: &str, development: bool) {
    let glyphs = tree_has_glyphs(root);
    let shadow_source = root.join(SHADOW_SOURCE).is_file();
    let variants = [(false, false), (true, false), (false, true), (true, true)];
    for (update, plain) in variants {
        // Update builds compiled before the rename live in `…-tu2-…` directories.
        let flavours: &[&str] = if update { &["-update", "-tu2"] } else { &[""] };
        let found = flavours.iter().find_map(|flavour| {
            let mut name = platform.build_prefix().to_string() + flavour;
            if plain {
                name += "-plain";
            }
            ["relwithdebinfo", "release", "debug"].iter().find_map(|config| {
                let exe = root
                    .join("port/out/build")
                    .join(format!("{name}-{config}"))
                    .join(platform.executable());
                exe.is_file().then_some(exe)
            })
        });
        if let Some(exe) = found {
            let shadow = shadow_source && converter_staged(&exe, platform);
            let markers = Markers { extras: !plain, shadow, glyphs };
            push_builds(builds, update, markers, &exe, None, label, development);
        }
    }
}

/// Finds game builds for `platform`. A release install keeps one executable
/// beside the launcher and its game folder; its update flavour is read from that
/// folder's Default.xex and what it carries from the executable itself. A source
/// checkout is found from `search_from` upwards and offers every build in it. A
/// standalone launcher lists what `trees` say from each fetched source tree.
pub fn discover_builds(
    platform: Platform,
    launcher_dir: &Path,
    search_from: &[PathBuf],
    trees: &[SourceTree],
) -> Vec<Build> {
    let mut builds = Vec::new();
    let installed = launcher_dir.join(platform.executable());
    if installed.is_file() {
        let game = launcher_dir.join("game");
        let update = sha256_file(&update_layout(&game).1).is_ok_and(|h| h == USA_UPDATE_HASH);
        let mut markers = executable_markers(&installed);
        markers.shadow &= converter_staged(&installed, platform);
        push_builds(&mut builds, update, markers, &installed, Some(&game), "", false);
    }
    if let Some(root) = repository_root(search_from) {
        discover_tree(&mut builds, platform, &root, "", false);
    }
    for tree in trees {
        discover_tree(&mut builds, platform, tree.root, tree.label, tree.development);
    }
    builds
}

pub fn repository_root(search_from: &[PathBuf]) -> Option<PathBuf> {
    search_from.iter().find_map(|start| {
        start
            .ancestors()
            .find(|p| p.join("port/CMakeLists.txt").is_file())
            .map(Path::to_path_buf)
    })
}

/// Checks the game folder holds the executable this build was compiled for.
pub fn validate_game_dir(build: &Build, game: &Path) -> Result<(), Message> {
    let (update_root, update_xex) = update_layout(game);
    let xex = if build.update { update_xex } else { game.join("Default.xex") };
    let folder = game.display();
    if !xex.is_file() {
        return Err(if build.update {
            msg!("err.gameDir.noXex.update", folder = folder)
        } else {
            msg!("err.gameDir.noXex.base", folder = folder)
        });
    }
    let hash = sha256_file(&xex)?;
    let (expected, other) = if build.update { (USA_UPDATE_HASH, BASE_HASH) } else { (BASE_HASH, USA_UPDATE_HASH) };
    if hash != expected {
        if build.update && hash == other && xex.parent() == Some(game) {
            return Err(msg!("err.gameDir.updateNotInstalled", folder = folder));
        }
        return Err(match (hash == other, build.update) {
            // The folder holds the other executable.
            (true, true) => msg!("err.gameDir.holdsBase", folder = folder, build = &build.label),
            (true, false) => msg!("err.gameDir.holdsUpdate", folder = folder, build = &build.label),
            (false, true) => msg!("err.gameDir.unverified.update", folder = folder),
            (false, false) => msg!("err.gameDir.unverified.base", folder = folder),
        });
    }
    if build.update {
        if let Some(missing) = UPDATE_CPKS
            .iter()
            .find(|n| !update_root.join("CPKs").join(n).is_file())
        {
            return Err(msg!("err.gameDir.missingCpk", file = missing, folder = update_root.display()));
        }
    }
    Ok(())
}

/// True when an update build exists and its game folder holds the verified update
/// executable and CPKs. The title update is an overlay in the base game folder, so providing
/// the update is what makes the update builds usable.
pub fn update_installed(builds: &[Build], settings: &Settings, defaults: &Defaults) -> bool {
    builds.iter().filter(|b| b.update).any(|b| {
        let (game, _) = folders_for(b, settings, defaults);
        validate_game_dir(b, &game).is_ok()
    })
}

/// Seeds the menu's build choice when none was saved (or the saved one is gone):
/// Update + Extras when the update is installed, else the best there is (update over
/// base, extras over plain, stable over Development). An explicit saved choice is
/// never overridden.
pub fn apply_default_build(
    ui_state: &mut serde_json::Value,
    builds: &[Build],
    update_installed: bool,
) {
    let saved = ui_state
        .get("build")
        .and_then(|v| v.as_str())
        .is_some_and(|id| builds.iter().any(|b| b.id == id));
    if saved {
        return;
    }
    // The update over the base game (when its files are installed), + Extras over the
    // plain build, and the stable or checkout builds over Development ones: the same
    // preference as the Build panel's choice after a build.
    let score = |b: &Build| (!b.development, b.update && update_installed, b.extras);
    let pick = builds.iter().filter(|b| update_installed || !b.update).max_by_key(|b| score(b));
    if let (Some(build), Some(state)) = (pick, ui_state.as_object_mut()) {
        state.insert("build".into(), build.id.clone().into());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::test_support::*;

    #[test]
    fn update_becomes_the_default_build_when_it_is_installed() {
        let builds = [pair(false, false), pair(false, true), pair(true, false), pair(true, true)];
        let mut state = serde_json::json!({ "res": -1 });
        apply_default_build(&mut state, &builds, true);
        assert_eq!(state["build"], "update-extras", "the update, with the extras");
        assert_eq!(state["res"], -1, "other saved menu state is kept");

        // Extras builds first: the update default keeps extras.
        let extras_first = [pair(false, true), pair(true, true)];
        let mut state = serde_json::Value::Null;
        state = if state.is_null() { serde_json::json!({}) } else { state };
        apply_default_build(&mut state, &extras_first, true);
        assert_eq!(state["build"], "update-extras");
    }

    #[test]
    fn default_build_respects_saved_choice_and_missing_update() {
        let builds = [pair(false, false), pair(true, false)];
        let mut saved = serde_json::json!({ "build": "base" });
        apply_default_build(&mut saved, &builds, true);
        assert_eq!(saved["build"], "base", "explicit choice wins");
        let mut none = serde_json::json!({});
        apply_default_build(&mut none, &builds, false);
        assert_eq!(none["build"], "base", "no update files: the base game");
        let mut stale = serde_json::json!({ "build": "update-plain" });
        apply_default_build(&mut stale, &builds, true);
        assert_eq!(stale["build"], "update", "a saved build that no longer exists is replaced");
        let mut only_base = serde_json::json!({});
        apply_default_build(&mut only_base, &[pair(false, false), pair(false, true)], true);
        assert_eq!(only_base["build"], "extras", "no update build: the base game with the extras");
        let mut development = pair(true, true);
        development.development = true;
        development.id += "-development";
        let mut both = serde_json::json!({});
        apply_default_build(&mut both, &[pair(false, true), development], true);
        assert_eq!(both["build"], "extras", "stable builds over Development ones");
    }

    #[test]
    fn discovers_source_builds_by_variant_and_prefers_relwithdebinfo() {
        let root = scratch("discover");
        fs::create_dir_all(root.join("port")).unwrap();
        fs::write(root.join("port/CMakeLists.txt"), "").unwrap();
        for dir in [
            "linux-amd64-relwithdebinfo",
            "linux-amd64-update-plain-release",
            "linux-amd64-update-plain-relwithdebinfo",
            "win-amd64-relwithdebinfo",
        ] {
            let path = root.join("port/out/build").join(dir);
            fs::create_dir_all(&path).unwrap();
            fs::write(
                path.join(if dir.starts_with("win") {
                    "diablo3.exe"
                } else {
                    "diablo3"
                }),
                "",
            )
            .unwrap();
        }
        let builds = discover_builds(
            Platform::Linux,
            &root.join("launcher"),
            &[root.join("launcher/tauri")], &[]);
        let ids: Vec<_> = builds.iter().map(|b| b.id.as_str()).collect();
        // The full build is offered with and without extras; the plain update build only without.
        assert_eq!(ids, ["base", "extras", "update"]);
        assert_eq!(builds[0].executable, builds[1].executable);
        assert!(builds[2]
            .executable
            .ends_with("linux-amd64-update-plain-relwithdebinfo/diablo3"));
        let windows = discover_builds(Platform::Windows, &root, std::slice::from_ref(&root), &[]);
        assert_eq!(windows.len(), 2);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn macos_finds_its_architecture_folders_with_the_shadow_converter() {
        let root = scratch("discover-macos");
        fs::create_dir_all(root.join("port/src/experiments")).unwrap();
        fs::write(root.join("port/CMakeLists.txt"), "").unwrap();
        fs::write(root.join("port/src/experiments/shadow_renderer.cpp"), "").unwrap();
        let prefix = Platform::MacOs.build_prefix();
        assert!(prefix == "macos-arm64" || prefix == "macos-x86_64");
        for name in [format!("{prefix}-update-relwithdebinfo"), "linux-amd64-relwithdebinfo".into()] {
            let dir = root.join("port/out/build").join(name);
            fs::create_dir_all(dir.join("xenosrecomp")).unwrap();
            fs::write(dir.join("diablo3"), "").unwrap();
            fs::write(dir.join("xenosrecomp/XenosRecomp"), "").unwrap();
        }
        let builds = discover_builds(Platform::MacOs, &root, std::slice::from_ref(&root), &[]);
        let listed: Vec<_> = builds.iter().map(|b| (b.id.as_str(), b.shadow)).collect();
        assert_eq!(listed, [("update", true), ("update-extras", true)], "Linux folders are not listed");
        assert!(builds[0].executable.ends_with(format!("{prefix}-update-relwithdebinfo/diablo3")));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn update_builds_from_before_the_rename_are_still_discovered() {
        let root = scratch("discover-legacy");
        fs::create_dir_all(root.join("port")).unwrap();
        fs::write(root.join("port/CMakeLists.txt"), "").unwrap();
        let old = root.join("port/out/build/linux-amd64-tu2-relwithdebinfo");
        fs::create_dir_all(&old).unwrap();
        fs::write(old.join("diablo3"), "").unwrap();
        let builds = discover_builds(Platform::Linux, &root, std::slice::from_ref(&root), &[]);
        assert_eq!(builds.len(), 2, "update, and update with extras");
        assert!(builds[0].update && builds[0].executable.starts_with(&old));
        // A rebuilt update directory wins over the old one.
        let new = root.join("port/out/build/linux-amd64-update-relwithdebinfo");
        fs::create_dir_all(&new).unwrap();
        fs::write(new.join("diablo3"), "").unwrap();
        let builds = discover_builds(Platform::Linux, &root, std::slice::from_ref(&root), &[]);
        assert!(builds[0].executable.starts_with(&new));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn shadow_is_offered_where_its_converter_is_staged_and_old_experimental_builds_are_gone() {
        let root = scratch("discover-shadow");
        fs::create_dir_all(root.join("port/src/experiments")).unwrap();
        fs::write(root.join("port/CMakeLists.txt"), "").unwrap();
        fs::write(root.join("port/src/experiments/shadow_renderer.cpp"), "").unwrap();
        for name in ["linux-amd64-update-relwithdebinfo", "linux-amd64-relwithdebinfo", "linux-amd64-update-experimental-relwithdebinfo"] {
            let dir = root.join("port/out/build").join(name);
            fs::create_dir_all(&dir).unwrap();
            fs::write(dir.join("diablo3"), "").unwrap();
        }
        // Only the update build has its converter beside it.
        let staged = root.join("port/out/build/linux-amd64-update-relwithdebinfo/xenosrecomp");
        fs::create_dir_all(&staged).unwrap();
        fs::write(staged.join("XenosRecomp"), "").unwrap();
        let builds = discover_builds(Platform::Linux, &root, std::slice::from_ref(&root), &[]);
        let listed: Vec<_> = builds.iter().map(|b| (b.id.as_str(), b.shadow, b.development)).collect();
        assert_eq!(
            listed,
            [("base", false, false), ("extras", false, false), ("update", true, false), ("update-extras", true, false)],
            "the old -experimental folder is not listed"
        );
        fs::remove_dir_all(&root).unwrap();
        // A release install says so itself, and needs the converter beside it too.
        let root = scratch("release-shadow");
        fs::write(root.join("diablo3"), b"..shadow_renderer_only..").unwrap();
        assert!(discover_builds(Platform::Linux, &root, &[], &[]).iter().all(|b| !b.shadow));
        fs::create_dir_all(root.join("xenosrecomp")).unwrap();
        fs::write(root.join("xenosrecomp/XenosRecomp"), "").unwrap();
        assert!(discover_builds(Platform::Linux, &root, &[], &[]).iter().all(|b| b.shadow));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn release_install_detects_extras_from_the_executable() {
        let root = scratch("release");
        fs::write(root.join("diablo3"), b"...pc_use_saved_render_scale...").unwrap();
        let builds = discover_builds(Platform::Linux, &root, &[], &[]);
        assert_eq!(builds.len(), 2);
        assert!(!builds[0].extras && builds[1].extras && !builds[1].update);
        assert!(!builds[1].glyphs, "a release from before Button Icons");
        fs::write(root.join("diablo3"), b"...pc_use_saved_render_scale...pc_button_glyphs...").unwrap();
        let builds = discover_builds(Platform::Linux, &root, &[], &[]);
        assert!(!builds[0].glyphs && builds[1].glyphs, "only the extras build offers Button Icons");
        assert_eq!(
            builds[0].game_dir.as_deref(),
            Some(root.join("game").as_path())
        );
        fs::write(root.join("diablo3"), b"plain").unwrap();
        let builds = discover_builds(Platform::Linux, &root, &[], &[]);
        assert_eq!(builds.len(), 1);
        assert!(!builds[0].extras);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn validation_names_the_folder_mismatch() {
        let game = scratch("validate");
        assert_eq!(validate_game_dir(&build(false, false), &game).unwrap_err().key, "err.gameDir.noXex.base");
        fs::write(game.join("Default.xex"), "not a real xex").unwrap();
        assert_eq!(validate_game_dir(&build(true, false), &game).unwrap_err().key, "err.gameDir.unverified.update");
        fs::remove_dir_all(game).unwrap();
    }

    #[test]
    fn legacy_tu2_overlay_saves_and_staged_folder_are_still_found() {
        let root = scratch("legacy-tu2");
        let game = root.join("game");
        fs::create_dir_all(game.join("tu2")).unwrap();
        fs::write(game.join("tu2").join("Default.xex"), b"x").unwrap();
        assert_eq!(update_layout(&game).0, game.join("tu2"));
        fs::create_dir_all(game.join("update")).unwrap();
        fs::write(game.join("update").join("Default.xex"), b"x").unwrap();
        assert_eq!(update_layout(&game).0, game.join("update"), "the new name wins");

        let data = root.join("data");
        fs::create_dir_all(data.join("souls-of-the-reaper-tu2")).unwrap();
        let old = default_folders(Platform::Linux, &root, &data);
        assert!(old.user_data_root_update.ends_with("souls-of-the-reaper-tu2"));
        fs::create_dir_all(data.join("souls-of-the-reaper-update")).unwrap();
        let new = default_folders(Platform::Linux, &root, &data);
        assert!(new.user_data_root_update.ends_with("souls-of-the-reaper-update"));
        fs::create_dir_all(root.join("game-tu2")).unwrap();
        assert_eq!(default_folders(Platform::Linux, &root, &data).game_dir_update, root.join("game-tu2"));
    }
}
