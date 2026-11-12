//! Local profiles of a build's save folder (game/profiles.rs). Player 1 plays
//! the profile chosen here; players who join pick theirs in game.

use crate::game::{self, Build, Profile, Settings};
use crate::host::Host;
use crate::message::{msg, Message};
use std::path::PathBuf;

fn save_root(host: &Host, settings: &Settings, id: &str) -> Result<PathBuf, Message> {
    let build: Build =
        host.builds.lock().unwrap().iter().find(|b| b.id == id).cloned().ok_or(msg!("err.launch.noBuild"))?;
    Ok(game::folders_for(&build, settings, &host.defaults).1)
}

/// The profiles of `build`'s save folder. Saves made before profiles become
/// profiles here (the oldest one keeps the old player name).
#[tauri::command]
pub fn list_profiles(host: tauri::State<Host>, settings: Settings, build: String) -> Result<Vec<Profile>, Message> {
    let root = save_root(&host, &settings, &build)?;
    Ok(game::list_profiles(&root, &settings.username))
}

#[tauri::command]
pub fn create_profile(host: tauri::State<Host>, settings: Settings, build: String, name: String) -> Result<Profile, Message> {
    let root = save_root(&host, &settings, &build)?;
    std::fs::create_dir_all(&root).map_err(|e| msg!("err.profile.save", detail = e))?;
    game::create_profile(&root, &name, &settings.username)
}

#[tauri::command]
pub fn rename_profile(
    host: tauri::State<Host>,
    settings: Settings,
    build: String,
    xuid: String,
    name: String,
) -> Result<Profile, Message> {
    let root = save_root(&host, &settings, &build)?;
    game::rename_profile(&root, &xuid, &name, &settings.username)
}

/// Deletes a profile and its saves (the page asks first).
#[tauri::command]
pub fn delete_profile(host: tauri::State<Host>, settings: Settings, build: String, xuid: String) -> Result<(), Message> {
    let root = save_root(&host, &settings, &build)?;
    game::delete_profile(&root, &xuid)
}
