#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod launch;

use launch::{Build, Defaults, GameOptions, Platform, Settings};
use serde::Serialize;
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    time::{Duration, Instant},
};
use tauri::{Manager, WebviewUrl, WebviewWindowBuilder};

/// Builds and folder defaults, resolved once at startup.
struct Host {
    platform: Platform,
    builds: Vec<Build>,
    defaults: Defaults,
    settings_file: PathBuf,
}

/// Injected ahead of the page scripts as window.LAUNCHER_CAPS.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Caps<'a> {
    platform: &'static str,
    builds: &'a [Build],
    defaults: &'a Defaults,
}

fn load_settings(path: &Path) -> Settings {
    fs::read(path)
        .map(|data| launch::parse_settings(&data))
        .unwrap_or_default()
}

fn write_settings(path: &Path, settings: &Settings) -> Result<(), String> {
    fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    let data = serde_json::to_vec_pretty(settings).map_err(|e| e.to_string())?;
    fs::write(path, data).map_err(|e| e.to_string())
}

#[tauri::command]
fn save_settings(host: tauri::State<Host>, settings: Settings) -> Result<(), String> {
    write_settings(&host.settings_file, &settings)
}

/// Grants the game the foreground; Windows otherwise opens it behind the
/// closing launcher (see Launcher.ps1's AllowSetForegroundWindow note).
#[cfg(target_os = "windows")]
fn allow_foreground(pid: u32) {
    #[link(name = "user32")]
    extern "system" {
        fn AllowSetForegroundWindow(process_id: u32) -> i32;
    }
    unsafe {
        AllowSetForegroundWindow(pid);
    }
}

#[cfg(not(target_os = "windows"))]
fn allow_foreground(_pid: u32) {}

/// Validates the selected build and folders, saves the launcher choices and
/// starts the game. Returns once the game has survived startup briefly, so an
/// immediate failure is reported here instead of disappearing with the window.
#[tauri::command]
async fn launch_game(
    window: tauri::WebviewWindow,
    host: tauri::State<'_, Host>,
    settings: Settings,
    options: GameOptions,
) -> Result<(), String> {
    let build = match &options.build {
        Some(id) => host.builds.iter().find(|b| &b.id == id),
        None => host.builds.first(),
    }
    .ok_or("No game build found. Build the game or place the launcher next to diablo3.")?
    .clone();
    let (game, user) = launch::folders_for(&build, &settings, &host.defaults);
    let game =
        fs::canonicalize(&game).map_err(|e| format!("Game folder {}: {e}", game.display()))?;
    launch::validate_game_dir(&build, &game)?;
    fs::create_dir_all(&user).map_err(|e| format!("Save folder {}: {e}", user.display()))?;
    let user = fs::canonicalize(&user).map_err(|e| e.to_string())?;
    let args = launch::launch_arguments(
        host.platform,
        &build,
        &game,
        &user,
        &options,
        settings.vulkan_device,
    )?;
    write_settings(&host.settings_file, &settings)?;

    let stdio_log = user.join("launcher-stdio.log");
    let output = fs::File::create(&stdio_log).map_err(|e| e.to_string())?;
    let mut child = Command::new(&build.executable)
        .args(&args)
        .current_dir(build.executable.parent().unwrap())
        .stdout(Stdio::from(output.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(output))
        .spawn()
        .map_err(|e| format!("Unable to start {}: {e}", build.executable.display()))?;
    allow_foreground(child.id());
    let exited = tauri::async_runtime::spawn_blocking(move || {
        let deadline = Instant::now() + Duration::from_millis(1500);
        while Instant::now() < deadline {
            match child.try_wait() {
                Ok(Some(status)) => return Some(status),
                Ok(None) => std::thread::sleep(Duration::from_millis(100)),
                Err(_) => break,
            }
        }
        None
    })
    .await
    .map_err(|e| e.to_string())?;
    if let Some(status) = exited.filter(|s| !s.success()) {
        return Err(format!(
            "The game exited during startup ({status}). See {}.",
            stdio_log.display()
        ));
    }
    window.close().map_err(|e| e.to_string())
}

#[tauri::command]
fn window_action(window: tauri::WebviewWindow, action: String) -> Result<(), String> {
    match action.as_str() {
        "close" => window.close(),
        "minimize" => window.minimize(),
        "dragStart" => window.start_dragging(),
        _ => return Err("Unknown window action.".into()),
    }
    .map_err(|e| e.to_string())
}

fn setup(app: &mut tauri::App) -> Result<(), Box<dyn std::error::Error>> {
    let platform = Platform::current();
    let launcher_dir = std::env::current_exe()?
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_default();
    let mut search = vec![launcher_dir.clone()];
    search.extend(std::env::current_dir().ok());
    let builds = launch::discover_builds(platform, &launcher_dir, &search);
    // Source checkouts keep game folders at the repository root; installs
    // keep them beside the launcher.
    let base = launch::repository_root(&search)
        .filter(|_| !builds.iter().any(|b| b.game_dir.is_some()))
        .unwrap_or_else(|| launcher_dir.clone());
    let user_base = match platform {
        Platform::Windows => app.path().document_dir()?,
        Platform::Linux => app.path().data_dir()?,
    };
    let defaults = launch::default_folders(platform, &base, &user_base);
    let settings_file = app.path().app_config_dir()?.join("launcher.json");
    let mut settings = load_settings(&settings_file);
    if settings.ui_state.is_null() {
        // Extras builds start on their in-game resolution and window mode.
        settings.ui_state = serde_json::json!({ "res": -1, "display": -1 });
    }

    let caps = Caps {
        platform: platform.name(),
        builds: &builds,
        defaults: &defaults,
    };
    let init = format!(
        "window.LAUNCHER_CAPS={};window.LAUNCHER_SETTINGS={};window.SAVED_STATE={};",
        serde_json::to_string(&caps)?,
        serde_json::to_string(&settings)?,
        serde_json::to_string(&settings.ui_state)?,
    );
    app.manage(Host {
        platform,
        builds,
        defaults,
        settings_file,
    });
    // Sized to the v6 design's 500px stage (aspect 1088/1445), like Launcher.ps1.
    WebviewWindowBuilder::new(app, "main", WebviewUrl::default())
        .title("Souls of the Reaper")
        .inner_size(500.0, 664.0)
        .resizable(false)
        .decorations(false)
        .center()
        .initialization_script(&init)
        .build()?;
    Ok(())
}

fn main() {
    tauri::Builder::default()
        .setup(setup)
        .invoke_handler(tauri::generate_handler![
            save_settings,
            launch_game,
            window_action
        ])
        .run(tauri::generate_context!())
        .expect("Unable to run launcher");
}
