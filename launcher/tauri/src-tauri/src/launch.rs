//! Build discovery, game-folder validation and game arguments. Kept free of
//! Tauri so both platforms' launch rules are unit-tested on any host. The
//! rules mirror scripts/run_linux.py and scripts/run_windows.ps1.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    path::{Path, PathBuf},
};

/// Verified guest executables (scripts/title_updates.py).
pub const BASE_HASH: &str = "cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef";
pub const TU2_HASH: &str = "447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f";
/// Update assets a staged TU2 folder must hold (scripts/stage_title_update.py).
pub const TU2_CPKS: [&str; 4] = [
    "Patch.cpk",
    "Patch2.cpk",
    "enUS_Patch.cpk",
    "enUS_Patch2.cpk",
];
/// Only extra-features executables register this cvar.
const EXTRAS_MARKER: &[u8] = b"pc_use_saved_render_scale";

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Platform {
    Windows,
    Linux,
}

impl Platform {
    pub fn current() -> Self {
        if cfg!(target_os = "windows") {
            Platform::Windows
        } else {
            Platform::Linux
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Platform::Windows => "windows",
            Platform::Linux => "linux",
        }
    }
    fn build_prefix(self) -> &'static str {
        match self {
            Platform::Windows => "win-amd64",
            Platform::Linux => "linux-amd64",
        }
    }
    fn executable(self) -> &'static str {
        match self {
            Platform::Windows => "diablo3.exe",
            Platform::Linux => "diablo3",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Build {
    pub id: String,
    pub label: String,
    pub tu2: bool,
    pub extras: bool,
    pub executable: PathBuf,
    /// Release installs pair the executable with the game folder beside it.
    pub game_dir: Option<PathBuf>,
}

fn variant(tu2: bool, extras: bool) -> (&'static str, &'static str) {
    match (tu2, extras) {
        (false, false) => ("base", "Base"),
        (false, true) => ("extras", "Base + Extras"),
        (true, false) => ("tu2", "TU2"),
        (true, true) => ("tu2-extras", "TU2 + Extras"),
    }
}

fn make_build(tu2: bool, extras: bool, executable: PathBuf, game_dir: Option<PathBuf>) -> Build {
    let (id, label) = variant(tu2, extras);
    Build {
        id: id.into(),
        label: label.into(),
        tu2,
        extras,
        executable,
        game_dir,
    }
}

pub fn sha256_file(path: &Path) -> Result<String, String> {
    let data = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(format!("{:x}", Sha256::digest(&data)))
}

fn has_extras_marker(executable: &Path) -> bool {
    fs::read(executable)
        .map(|data| {
            data.windows(EXTRAS_MARKER.len())
                .any(|w| w == EXTRAS_MARKER)
        })
        .unwrap_or(false)
}

/// Finds game builds for `platform`. A release install keeps one executable
/// beside the launcher and its game folder; its TU2/extras flavour is read
/// from that folder's Default.xex and the executable itself. A source
/// checkout is found from `search_from` upwards and offers every built
/// `port/out/build/<platform>-amd64[-tu2][-extras]-<config>` variant.
pub fn discover_builds(
    platform: Platform,
    launcher_dir: &Path,
    search_from: &[PathBuf],
) -> Vec<Build> {
    let mut builds = Vec::new();
    let installed = launcher_dir.join(platform.executable());
    if installed.is_file() {
        let game = launcher_dir.join("game");
        let tu2 = sha256_file(&game.join("Default.xex")).is_ok_and(|h| h == TU2_HASH);
        builds.push(make_build(
            tu2,
            has_extras_marker(&installed),
            installed,
            Some(game),
        ));
    }
    if let Some(root) = repository_root(search_from) {
        for (tu2, extras) in [(false, false), (false, true), (true, false), (true, true)] {
            let mut name = platform.build_prefix().to_string();
            if tu2 {
                name += "-tu2";
            }
            if extras {
                name += "-extras";
            }
            let found = ["relwithdebinfo", "release", "debug"]
                .iter()
                .find_map(|config| {
                    let exe = root
                        .join("port/out/build")
                        .join(format!("{name}-{config}"))
                        .join(platform.executable());
                    exe.is_file().then_some(exe)
                });
            if let Some(exe) = found {
                if !builds
                    .iter()
                    .any(|b: &Build| b.tu2 == tu2 && b.extras == extras)
                {
                    builds.push(make_build(tu2, extras, exe, None));
                }
            }
        }
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

/// Launcher settings persisted in launcher.json. Empty strings use defaults.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    pub game_dir: String,
    pub game_dir_tu2: String,
    pub user_data_root: String,
    pub user_data_root_tu2: String,
    /// Linux only; -1 lets the SDK choose.
    pub vulkan_device: i32,
    pub ui_state: serde_json::Value,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            game_dir: String::new(),
            game_dir_tu2: String::new(),
            user_data_root: String::new(),
            user_data_root_tu2: String::new(),
            vulkan_device: -1,
            ui_state: serde_json::Value::Null,
        }
    }
}

/// Reads launcher.json. The first prototype stored one game/save folder plus a
/// `tu2` flag; a TU2 entry moves to the TU2 fields so its saves stay in use.
pub fn parse_settings(data: &[u8]) -> Settings {
    let Ok(raw) = serde_json::from_slice::<serde_json::Value>(data) else {
        return Settings::default();
    };
    let mut settings: Settings = serde_json::from_value(raw.clone()).unwrap_or_default();
    if raw.get("tu2") == Some(&serde_json::Value::Bool(true)) && raw.get("gameDirTu2").is_none() {
        settings.game_dir_tu2 = std::mem::take(&mut settings.game_dir);
        settings.user_data_root_tu2 = std::mem::take(&mut settings.user_data_root);
    }
    settings
}

/// Folder defaults shown as placeholders and used for empty settings.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Defaults {
    pub game_dir: PathBuf,
    pub game_dir_tu2: PathBuf,
    pub user_data_root: PathBuf,
    pub user_data_root_tu2: PathBuf,
}

/// Linux matches run_linux.py ($XDG_DATA_HOME/souls-of-the-reaper[-tu2]).
/// Windows matches run_windows.ps1 and the SDK default (Documents\diablo3).
pub fn default_folders(platform: Platform, base: &Path, user_base: &Path) -> Defaults {
    let (user, user_tu2) = match platform {
        Platform::Linux => (
            user_base.join("souls-of-the-reaper"),
            user_base.join("souls-of-the-reaper-tu2"),
        ),
        Platform::Windows => (user_base.join("diablo3"), user_base.join("diablo3-tu2")),
    };
    Defaults {
        game_dir: base.join("game"),
        game_dir_tu2: base.join("game-tu2"),
        user_data_root: user,
        user_data_root_tu2: user_tu2,
    }
}

fn choose(setting: &str, default: &Path) -> PathBuf {
    if setting.trim().is_empty() {
        default.to_path_buf()
    } else {
        PathBuf::from(setting.trim())
    }
}

/// Game and save folders for `build`: settings, then the release pairing,
/// then defaults. TU2 uses separate folders so saves never mix.
pub fn folders_for(build: &Build, settings: &Settings, defaults: &Defaults) -> (PathBuf, PathBuf) {
    let (game_setting, game_default, user_setting, user_default) = if build.tu2 {
        (
            &settings.game_dir_tu2,
            &defaults.game_dir_tu2,
            &settings.user_data_root_tu2,
            &defaults.user_data_root_tu2,
        )
    } else {
        (
            &settings.game_dir,
            &defaults.game_dir,
            &settings.user_data_root,
            &defaults.user_data_root,
        )
    };
    let game_default = build.game_dir.as_deref().unwrap_or(game_default);
    (
        choose(game_setting, game_default),
        choose(user_setting, user_default),
    )
}

/// Checks the game folder holds the executable this build was compiled for.
pub fn validate_game_dir(build: &Build, game: &Path) -> Result<(), String> {
    let xex = game.join("Default.xex");
    if !xex.is_file() {
        return Err(format!(
            "{} has no Default.xex. Choose the {} game folder in Launch settings (Linux filenames are case-sensitive).",
            game.display(),
            if build.tu2 { "staged TU2" } else { "extracted base-disc" }
        ));
    }
    let hash = sha256_file(&xex)?;
    let (expected, other, wanted, found) = if build.tu2 {
        (TU2_HASH, BASE_HASH, "the staged TU2", "the base-disc")
    } else {
        (BASE_HASH, TU2_HASH, "the unmodified base-disc", "the TU2")
    };
    if hash != expected {
        return Err(if hash == other {
            format!(
                "{} holds {found} executable; the {} build needs {wanted} folder.",
                game.display(),
                build.label
            )
        } else {
            format!("Default.xex in {} is not the verified USA Ultimate Evil Edition {wanted} executable.", game.display())
        });
    }
    if build.tu2 {
        if let Some(missing) = TU2_CPKS
            .iter()
            .find(|n| !game.join("CPKs").join(n).is_file())
        {
            return Err(format!(
                "Missing TU2 asset CPKs/{missing} in {}.",
                game.display()
            ));
        }
    }
    Ok(())
}

/// Choices from the launcher menu (buildLaunchConfig in the HTML).
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GameOptions {
    pub build: Option<String>,
    pub fps: u32,
    pub vsync: bool,
    pub mnk_mouse: bool,
    pub present: String,
    /// 0 keeps the extras build's saved in-game scale.
    pub res_scale: u32,
    /// None keeps the extras build's saved in-game window mode.
    pub fullscreen: Option<bool>,
    pub lang_id: u32,
    pub country_id: u32,
    pub rtp: String,
    pub vk_path: String,
}

pub fn launch_arguments(
    platform: Platform,
    build: &Build,
    game: &Path,
    user: &Path,
    options: &GameOptions,
    vulkan_device: i32,
) -> Result<Vec<String>, String> {
    if options.res_scale > 3 || (options.res_scale == 0 && !build.extras) {
        return Err("Choose a supported internal resolution.".into());
    }
    if options.fullscreen.is_none() && !build.extras {
        return Err("Choose fullscreen or windowed.".into());
    }
    let mut args = vec![
        format!("--game_data_root={}", game.display()),
        format!("--user_data_root={}", user.display()),
    ];
    if build.tu2 {
        // TU2 reads its patch CPKs through update:\ even with a prepatched XEX.
        args.push(format!("--update_data_root={}", game.display()));
    }
    match platform {
        Platform::Linux => {
            if !["fsi", "host"].contains(&options.vk_path.as_str()) {
                return Err("Unsupported Vulkan render path.".into());
            }
            args.push(format!("--cache_root={}", user.join("cache").display()));
            args.push(format!("--log_file={}", user.join("diablo3.log").display()));
            args.push(format!("--render_target_path_vulkan={}", options.vk_path));
            args.push(format!("--vsync={}", options.vsync));
            if vulkan_device >= 0 {
                args.push(format!("--vulkan_device={vulkan_device}"));
            }
        }
        Platform::Windows => {
            if !["rov", "rtv"].contains(&options.rtp.as_str()) {
                return Err("Unsupported render path.".into());
            }
            if ![30, 60, 120, 144, 240].contains(&options.fps) {
                return Err("Unsupported frame rate.".into());
            }
            if !["fsr", "cas", "bilinear"].contains(&options.present.as_str()) {
                return Err("Unsupported image scaling.".into());
            }
            args.push(format!("--render_target_path_d3d12={}", options.rtp));
            args.push("--rtv_color_depth_ownership_mode=8".into());
            args.push("--execute_unclipped_draw_vs_on_cpu=true".into());
            args.push(format!("--d3_frame_limit={}", options.fps));
            args.push(format!("--vsync={}", options.vsync));
            args.push(format!("--present_effect={}", options.present));
            args.push("--mnk_cursor_visible=false".into());
        }
    }
    args.push("--mnk_mode=true".into());
    args.push(format!("--mnk_mouse={}", options.mnk_mouse));
    args.push(format!("--user_language={}", options.lang_id));
    args.push(format!("--user_country={}", options.country_id));
    if options.res_scale > 0 {
        args.push(format!("--draw_resolution_scale_x={}", options.res_scale));
        args.push(format!("--draw_resolution_scale_y={}", options.res_scale));
        if build.extras {
            args.push("--pc_use_saved_render_scale=false".into());
        }
    }
    if let Some(fullscreen) = options.fullscreen {
        args.push(format!("--fullscreen={fullscreen}"));
        if build.extras {
            args.push("--pc_use_saved_window_mode=false".into());
        }
    }
    Ok(args)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn options() -> GameOptions {
        GameOptions {
            build: None,
            fps: 120,
            vsync: false,
            mnk_mouse: false,
            present: "fsr".into(),
            res_scale: 2,
            fullscreen: Some(true),
            lang_id: 1,
            country_id: 103,
            rtp: "rtv".into(),
            vk_path: "fsi".into(),
        }
    }

    fn build(tu2: bool, extras: bool) -> Build {
        make_build(tu2, extras, PathBuf::from("diablo3"), None)
    }

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("sotr-launcher-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn tu2_mounts_update_root_and_keeps_paths_with_spaces() {
        let game = Path::new("/tmp/game with spaces");
        let args = launch_arguments(
            Platform::Linux,
            &build(true, false),
            game,
            Path::new("/s d"),
            &options(),
            1,
        )
        .unwrap();
        assert!(args.contains(&"--game_data_root=/tmp/game with spaces".into()));
        assert!(args.contains(&"--update_data_root=/tmp/game with spaces".into()));
        assert!(args.contains(&"--log_file=/s d/diablo3.log".into()));
        let base = launch_arguments(
            Platform::Linux,
            &build(false, false),
            game,
            Path::new("/s"),
            &options(),
            1,
        )
        .unwrap();
        assert!(!base.iter().any(|a| a.starts_with("--update_data_root")));
    }

    #[test]
    fn linux_omits_d3d12_and_windows_only_options() {
        let args = launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            -1,
        )
        .unwrap();
        assert!(args.contains(&"--render_target_path_vulkan=fsi".into()));
        assert!(!args.iter().any(|a| a.contains("d3d12")
            || a.contains("d3_frame_limit")
            || a.contains("present_effect")));
        assert!(
            !args.iter().any(|a| a.starts_with("--vulkan_device")),
            "auto device passes nothing"
        );
        let pinned = launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            0,
        )
        .unwrap();
        assert!(pinned.contains(&"--vulkan_device=0".into()));
    }

    #[test]
    fn windows_passes_d3d12_presentation_and_frame_limit() {
        let args = launch_arguments(
            Platform::Windows,
            &build(false, false),
            Path::new("C:\\g"),
            Path::new("C:\\s"),
            &options(),
            1,
        )
        .unwrap();
        for expected in [
            "--render_target_path_d3d12=rtv",
            "--d3_frame_limit=120",
            "--present_effect=fsr",
            "--vsync=false",
        ] {
            assert!(args.contains(&expected.into()), "{expected}");
        }
        assert!(!args.iter().any(|a| a.contains("vulkan")));
    }

    #[test]
    fn extras_in_game_choices_leave_saved_settings_alone() {
        let mut opts = options();
        opts.res_scale = 0;
        opts.fullscreen = None;
        let args = launch_arguments(
            Platform::Linux,
            &build(false, true),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1,
        )
        .unwrap();
        assert!(!args.iter().any(|a| a.contains("resolution_scale")
            || a.contains("pc_use_saved")
            || a.starts_with("--fullscreen")));
        // The same choices are invalid for a normal build.
        assert!(launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
    }

    #[test]
    fn extras_explicit_choices_override_saved_settings() {
        let args = launch_arguments(
            Platform::Windows,
            &build(true, true),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            -1,
        )
        .unwrap();
        for expected in [
            "--draw_resolution_scale_x=2",
            "--pc_use_saved_render_scale=false",
            "--fullscreen=true",
            "--pc_use_saved_window_mode=false",
        ] {
            assert!(args.contains(&expected.into()), "{expected}");
        }
        let normal = launch_arguments(
            Platform::Windows,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            -1,
        )
        .unwrap();
        assert!(
            !normal.iter().any(|a| a.starts_with("--pc_use_saved")),
            "normal builds lack these cvars"
        );
    }

    #[test]
    fn rejects_unsupported_values() {
        let mut opts = options();
        opts.res_scale = 4;
        assert!(launch_arguments(
            Platform::Linux,
            &build(false, true),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
        let mut opts = options();
        opts.vk_path = "rov".into();
        assert!(launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
        let mut opts = options();
        opts.fps = 75;
        assert!(launch_arguments(
            Platform::Windows,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
    }

    #[test]
    fn discovers_source_builds_by_variant_and_prefers_relwithdebinfo() {
        let root = scratch("discover");
        fs::create_dir_all(root.join("port")).unwrap();
        fs::write(root.join("port/CMakeLists.txt"), "").unwrap();
        for dir in [
            "linux-amd64-relwithdebinfo",
            "linux-amd64-tu2-extras-release",
            "linux-amd64-tu2-extras-relwithdebinfo",
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
            &[root.join("launcher/tauri")],
        );
        let ids: Vec<_> = builds.iter().map(|b| b.id.as_str()).collect();
        assert_eq!(ids, ["base", "tu2-extras"]);
        assert!(builds[1]
            .executable
            .ends_with("linux-amd64-tu2-extras-relwithdebinfo/diablo3"));
        let windows = discover_builds(Platform::Windows, &root, std::slice::from_ref(&root));
        assert_eq!(windows.len(), 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn release_install_detects_extras_from_the_executable() {
        let root = scratch("release");
        fs::write(root.join("diablo3"), b"...pc_use_saved_render_scale...").unwrap();
        let builds = discover_builds(Platform::Linux, &root, &[]);
        assert_eq!(builds.len(), 1);
        assert!(builds[0].extras && !builds[0].tu2);
        assert_eq!(
            builds[0].game_dir.as_deref(),
            Some(root.join("game").as_path())
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn validation_names_the_folder_mismatch() {
        let game = scratch("validate");
        assert!(validate_game_dir(&build(false, false), &game)
            .unwrap_err()
            .contains("no Default.xex"));
        fs::write(game.join("Default.xex"), "not a real xex").unwrap();
        assert!(validate_game_dir(&build(true, false), &game)
            .unwrap_err()
            .contains("not the verified"));
        fs::remove_dir_all(game).unwrap();
    }

    #[test]
    fn missing_settings_fields_keep_automatic_vulkan_device() {
        let settings = parse_settings(br#"{"gameDir":"/g","executable":"old prototype field"}"#);
        assert_eq!(
            (settings.game_dir.as_str(), settings.vulkan_device),
            ("/g", -1)
        );
        assert_eq!(parse_settings(b"not json").vulkan_device, -1);
    }

    #[test]
    fn prototype_tu2_settings_move_to_tu2_folders() {
        let settings = parse_settings(
            br#"{"gameDir":"/tu2","userDataRoot":"/saves","tu2":true,"vulkanDevice":1}"#,
        );
        assert_eq!(settings.game_dir_tu2, "/tu2");
        assert_eq!(settings.user_data_root_tu2, "/saves");
        assert!(settings.game_dir.is_empty() && settings.user_data_root.is_empty());
        assert_eq!(settings.vulkan_device, 1);
        // Files written by this launcher are never migrated again.
        let current = parse_settings(br#"{"gameDir":"/base","gameDirTu2":"/tu2","tu2":true}"#);
        assert_eq!(current.game_dir, "/base");
    }

    #[test]
    fn tu2_uses_separate_folders_and_settings_override_defaults() {
        let defaults = default_folders(Platform::Linux, Path::new("/repo"), Path::new("/data"));
        let mut settings = Settings::default();
        let (game, user) = folders_for(&build(true, true), &settings, &defaults);
        assert_eq!(
            (game.as_path(), user.as_path()),
            (
                Path::new("/repo/game-tu2"),
                Path::new("/data/souls-of-the-reaper-tu2")
            )
        );
        settings.game_dir = " /custom ".into();
        let (game, user) = folders_for(&build(false, false), &settings, &defaults);
        assert_eq!(
            (game.as_path(), user.as_path()),
            (Path::new("/custom"), Path::new("/data/souls-of-the-reaper"))
        );
        let windows = default_folders(Platform::Windows, Path::new("/r"), Path::new("/Documents"));
        assert!(
            windows.user_data_root.ends_with("diablo3")
                && windows.user_data_root_tu2.ends_with("diablo3-tu2")
        );
    }
}
