//! launcher.json settings and the game and save folders they resolve to.

use super::*;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Launcher settings persisted in launcher.json. Empty strings use defaults.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    pub game_dir: String,
    pub game_dir_update: String,
    pub user_data_root: String,
    pub user_data_root_update: String,
    /// Save folders of Development builds (a standalone launcher's own commit), kept
    /// apart from the normal ones so
    /// unfinished features never touch those saves.
    pub user_data_root_development: String,
    pub user_data_root_update_development: String,
    /// The player name from before profiles: the name the oldest save's profile
    /// gets when profiles are first listed (game/profiles.rs).
    pub username: String,
    /// The profile Player 1 plays (its XUID, in the chosen build's save folder);
    /// "ingame" lets the player choose in game, empty means not chosen yet.
    pub profile: String,
    /// Linux only; -1 lets the SDK choose.
    pub vulkan_device: i32,
    /// Linux display server: "" (automatic), "wayland" or "x11".
    pub video_driver: String,
    /// Last inputs of the Build panel (scripts/build_client.py).
    pub build_iso: String,
    pub build_title_update: String,
    pub build_cmake: String,
    pub ui_state: serde_json::Value,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            game_dir: String::new(),
            game_dir_update: String::new(),
            user_data_root: String::new(),
            user_data_root_update: String::new(),
            user_data_root_development: String::new(),
            user_data_root_update_development: String::new(),
            username: String::new(),
            profile: String::new(),
            vulkan_device: -1,
            video_driver: String::new(),
            build_iso: String::new(),
            build_title_update: String::new(),
            build_cmake: String::new(),
            ui_state: serde_json::Value::Null,
        }
    }
}

/// Reads launcher.json. The first prototype stored one game/save folder plus a
/// `tu2` flag; a flagged entry moves to the update fields so its saves stay in use.
/// The update fields were once called `gameDirTu2`/`userDataRootTu2`, and the
/// Development save folders `userDataRoot[Update]Experimental`; those still load.
/// The old `experimental` switch is gone and ignored.
pub fn parse_settings(data: &[u8]) -> Settings {
    let Ok(mut raw) = serde_json::from_slice::<serde_json::Value>(data) else {
        return Settings::default();
    };
    if let Some(fields) = raw.as_object_mut() {
        for (old, new) in [
            ("gameDirTu2", "gameDirUpdate"),
            ("userDataRootTu2", "userDataRootUpdate"),
            ("userDataRootExperimental", "userDataRootDevelopment"),
            ("userDataRootUpdateExperimental", "userDataRootUpdateDevelopment"),
        ] {
            if let Some(value) = fields.remove(old) {
                fields.entry(new).or_insert(value);
            }
        }
    }
    let mut settings: Settings = serde_json::from_value(raw.clone()).unwrap_or_default();
    if raw.get("tu2") == Some(&serde_json::Value::Bool(true)) && raw.get("gameDirUpdate").is_none() {
        settings.game_dir_update = std::mem::take(&mut settings.game_dir);
        settings.user_data_root_update = std::mem::take(&mut settings.user_data_root);
    }
    settings
}

/// Folder defaults shown as placeholders and used for empty settings.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Defaults {
    pub game_dir: PathBuf,
    pub game_dir_update: PathBuf,
    pub user_data_root: PathBuf,
    pub user_data_root_update: PathBuf,
    pub user_data_root_development: PathBuf,
    pub user_data_root_update_development: PathBuf,
}

/// Linux matches run_linux.py ($XDG_DATA_HOME/souls-of-the-reaper[-update]).
/// Windows matches run_windows.ps1 and the SDK default (Documents\diablo3).
/// Development builds add `-development` to either.
pub fn default_folders(platform: Platform, base: &Path, user_base: &Path) -> Defaults {
    let name = match platform {
        Platform::Linux => "souls-of-the-reaper",
        Platform::Windows => "diablo3",
    };
    let folder = |suffix: &str| user_base.join(format!("{name}{suffix}"));
    let (user, user_update, user_legacy) = (folder(""), folder("-update"), folder("-tu2"));
    // Saves created before a folder was renamed stay in use: `-tu2` (now `-update`)
    // and `-experimental` (the builds that are now Development builds).
    let renamed = |new: PathBuf, old: PathBuf| if !new.exists() && old.exists() { old } else { new };
    let user_update = renamed(user_update, user_legacy);
    Defaults {
        game_dir: base.join("game"),
        // The title update is an overlay inside the game folder unless a full staged copy exists.
        game_dir_update: ["game-update", "game-tu2"]
            .iter()
            .map(|name| base.join(name))
            .find(|dir| dir.exists())
            .unwrap_or_else(|| base.join("game")),
        user_data_root: user,
        user_data_root_update: user_update,
        user_data_root_development: renamed(folder("-development"), folder("-experimental")),
        user_data_root_update_development: renamed(folder("-update-development"), folder("-update-experimental")),
    }
}

pub(crate) fn choose(setting: &str, default: &Path) -> PathBuf {
    if setting.trim().is_empty() {
        default.to_path_buf()
    } else {
        PathBuf::from(setting.trim())
    }
}

/// The game folder the update runs from. The title update is normally an overlay (`<game>/update`) inside the
/// base game folder, so that folder is used. An older full staged copy still works: a
/// saved update folder that holds an executable, else a `game-update` default that exists.
pub(crate) fn update_game_folder(settings: &Settings, defaults: &Defaults, base_game: &Path) -> PathBuf {
    let saved = settings.game_dir_update.trim();
    if !saved.is_empty() && Path::new(saved).join("Default.xex").is_file() {
        return PathBuf::from(saved);
    }
    if update_layout(base_game).0 != base_game {
        return base_game.to_path_buf();
    }
    if defaults.game_dir_update != defaults.game_dir {
        return defaults.game_dir_update.clone();
    }
    base_game.to_path_buf()
}

/// Base and update game folders (often the same folder): settings, then defaults.
pub fn game_folders(settings: &Settings, defaults: &Defaults) -> (PathBuf, PathBuf) {
    let base = choose(&settings.game_dir, &defaults.game_dir);
    let update = update_game_folder(settings, defaults, &base);
    (base, update)
}

/// Game and save folders for `build`: settings, then the release pairing,
/// then defaults. The title update and Development builds each use separate save
/// folders, so saves never mix.
pub fn folders_for(build: &Build, settings: &Settings, defaults: &Defaults) -> (PathBuf, PathBuf) {
    let base_default = build.game_dir.as_deref().unwrap_or(&defaults.game_dir);
    let base = choose(&settings.game_dir, base_default);
    let saves = match (build.update, build.development) {
        (false, false) => choose(&settings.user_data_root, &defaults.user_data_root),
        (true, false) => choose(&settings.user_data_root_update, &defaults.user_data_root_update),
        (false, true) => choose(&settings.user_data_root_development, &defaults.user_data_root_development),
        (true, true) => choose(&settings.user_data_root_update_development, &defaults.user_data_root_update_development),
    };
    let game = if build.update { update_game_folder(settings, defaults, &base) } else { base };
    (game, saves)
}

/// Windows `canonicalize` returns `\\?\C:\...` paths, which the Windows API does not
/// normalise: the game appends `update/Default.xex` with a forward slash and then fails to
/// open it. Drop the prefix for plain drive paths.
pub fn plain_path(path: PathBuf) -> PathBuf {
    let text = path.to_string_lossy().into_owned();
    match text.strip_prefix(r"\\?\") {
        Some(rest) if rest.as_bytes().get(1) == Some(&b':') => PathBuf::from(rest),
        _ => path,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::test_support::*;
    use std::fs;

    #[test]
    fn display_server_setting_loads_and_defaults_to_automatic() {
        assert_eq!(parse_settings(br#"{"videoDriver":"x11"}"#).video_driver, "x11");
        assert_eq!(parse_settings(br#"{"gameDir":"/g"}"#).video_driver, "");
    }

    #[test]
    fn old_experimental_settings_load_as_development_ones() {
        // The experimental switch and shadowRenderer toggle are gone and ignored.
        let settings = parse_settings(
            br#"{"experimental":true,"shadowRenderer":true,"userDataRootUpdateExperimental":"/exp"}"#,
        );
        assert_eq!(settings.user_data_root_update_development, "/exp");
        let both = parse_settings(br#"{"userDataRootExperimental":"/old","userDataRootDevelopment":"/new"}"#);
        assert_eq!(both.user_data_root_development, "/new");
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
    fn prototype_update_settings_move_to_update_folders() {
        let settings = parse_settings(
            br#"{"gameDir":"/update","userDataRoot":"/saves","tu2":true,"vulkanDevice":1}"#,
        );
        assert_eq!(settings.game_dir_update, "/update");
        assert_eq!(settings.user_data_root_update, "/saves");
        assert!(settings.game_dir.is_empty() && settings.user_data_root.is_empty());
        assert_eq!(settings.vulkan_device, 1);
        // Files written by this launcher are never migrated again.
        let current = parse_settings(br#"{"gameDir":"/base","gameDirUpdate":"/update","tu2":true}"#);
        assert_eq!(current.game_dir, "/base");
    }

    #[test]
    fn settings_saved_under_the_old_tu2_names_still_load() {
        let settings = parse_settings(
            br#"{"gameDir":"/base","gameDirTu2":"/old","userDataRootTu2":"/old-saves"}"#,
        );
        assert_eq!(settings.game_dir, "/base");
        assert_eq!(settings.game_dir_update, "/old");
        assert_eq!(settings.user_data_root_update, "/old-saves");
        // The new key wins when both are present.
        let both = parse_settings(br#"{"gameDirTu2":"/old","gameDirUpdate":"/new"}"#);
        assert_eq!(both.game_dir_update, "/new");
    }

    #[test]
    fn plain_path_drops_the_verbatim_drive_prefix_only() {
        assert_eq!(
            plain_path(PathBuf::from(r"\\?\C:\Users\me\game")),
            PathBuf::from(r"C:\Users\me\game")
        );
        assert_eq!(
            plain_path(PathBuf::from(r"\\?\UNC\server\share")),
            PathBuf::from(r"\\?\UNC\server\share")
        );
        assert_eq!(plain_path(PathBuf::from("/home/me/game")), PathBuf::from("/home/me/game"));
    }

    #[test]
    fn update_uses_the_base_folder_unless_an_older_staged_copy_exists() {
        let root = scratch("tu2folder");
        let defaults = default_folders(Platform::Linux, &root, Path::new("/data"));
        let mut settings = Settings::default();
        // Overlay or nothing built yet: update shares the base folder.
        assert_eq!(game_folders(&settings, &defaults).1, root.join("game"));
        // A stale saved update folder without an executable is ignored.
        settings.game_dir_update = root.join("gone").display().to_string();
        assert_eq!(game_folders(&settings, &defaults).1, root.join("game"));
        // A saved folder that still holds an executable is honoured.
        let old = root.join("old-update");
        fs::create_dir_all(&old).unwrap();
        fs::write(old.join("Default.xex"), "x").unwrap();
        settings.game_dir_update = old.display().to_string();
        assert_eq!(game_folders(&settings, &defaults).1, old);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn update_uses_separate_folders_and_settings_override_defaults() {
        let defaults = default_folders(Platform::Linux, Path::new("/repo"), Path::new("/data"));
        let mut settings = Settings::default();
        let (game, user) = folders_for(&build(true, true), &settings, &defaults);
        assert_eq!(
            (game.as_path(), user.as_path()),
            (
                Path::new("/repo/game"),
                Path::new("/data/souls-of-the-reaper-update")
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
                && windows.user_data_root_update.ends_with("diablo3-update")
        );
    }

    #[test]
    fn development_builds_keep_their_own_saves() {
        let user = scratch("saves-development");
        let defaults = default_folders(Platform::Linux, Path::new("/r"), &user);
        let settings = Settings { user_data_root: "/mine".into(), ..Settings::default() };
        let saves = |update, development| {
            let mut b = build(update, true);
            b.development = development;
            folders_for(&b, &settings, &defaults).1
        };
        assert_eq!(saves(false, false), PathBuf::from("/mine"));
        assert_eq!(saves(false, true), user.join("souls-of-the-reaper-development"), "not the normal folder");
        assert_eq!(saves(true, true), user.join("souls-of-the-reaper-update-development"));
        let chosen = Settings { user_data_root_update_development: " /dev ".into(), ..Settings::default() };
        let mut b = build(true, true);
        b.development = true;
        assert_eq!(folders_for(&b, &chosen, &defaults).1, PathBuf::from("/dev"));
        // Saves of the builds once called experimental stay in use.
        fs::create_dir_all(user.join("souls-of-the-reaper-update-experimental")).unwrap();
        let defaults = default_folders(Platform::Linux, Path::new("/r"), &user);
        assert_eq!(defaults.user_data_root_update_development, user.join("souls-of-the-reaper-update-experimental"));
        fs::create_dir_all(user.join("souls-of-the-reaper-update-development")).unwrap();
        let defaults = default_folders(Platform::Linux, Path::new("/r"), &user);
        assert_eq!(defaults.user_data_root_update_development, user.join("souls-of-the-reaper-update-development"));
        let windows = default_folders(Platform::Windows, Path::new("C:/r"), Path::new("C:/Docs"));
        assert!(windows.user_data_root_development.ends_with("diablo3-development"));
        fs::remove_dir_all(user).unwrap();
    }

}
