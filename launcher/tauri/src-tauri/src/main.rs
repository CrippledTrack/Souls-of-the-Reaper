#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::Mutex,
};
use tauri::Manager;

const BASE_HASH: &str = "cc918a70940517f974d0fd60d4936c8236e8dc21130cf4a8b8ae915451c289ef";
const TU2_HASH: &str = "447652ffa8abe4c7b8bed590a3887efc23e1181fd836b7a3192b8a2a37ddf80f";

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Settings {
    game_dir: String,
    executable: String,
    user_data_root: String,
    tu2: bool,
    extras: bool,
    vulkan_device: u32,
    #[serde(default)]
    ui_state: serde_json::Value,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct GameOptions {
    fps: u32,
    vsync: bool,
    mnk_mouse: bool,
    present: String,
    res_scale: u32,
    fullscreen: bool,
    lang_id: u32,
    country_id: u32,
    rtp: String,
}

#[derive(Default)]
struct GameProcess(Mutex<Option<Child>>);

fn settings_file(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    app.path()
        .app_config_dir()
        .map(|p| p.join("launcher.json"))
        .map_err(|e| e.to_string())
}

fn repository_root() -> Option<PathBuf> {
    let cwd = std::env::current_dir().ok()?;
    cwd.ancestors()
        .find(|p| p.join("port/CMakeLists.txt").is_file())
        .map(Path::to_path_buf)
}

#[tauri::command]
fn load_settings(app: tauri::AppHandle) -> Result<Settings, String> {
    let path = settings_file(&app)?;
    if path.exists() {
        return serde_json::from_slice(&fs::read(path).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string());
    }
    let root = repository_root().unwrap_or_default();
    let platform = if cfg!(target_os = "windows") {
        "win"
    } else {
        "linux"
    };
    let filename = if cfg!(target_os = "windows") {
        "diablo3.exe"
    } else {
        "diablo3"
    };
    let build = root.join(format!(
        "port/out/build/{platform}-amd64-tu2-extras-relwithdebinfo/{filename}"
    ));
    let state = app
        .path()
        .app_data_dir()
        .map_err(|e| e.to_string())?
        .join("tu2");
    Ok(Settings {
        game_dir: root.join("game-tu2").to_string_lossy().into_owned(),
        executable: build.to_string_lossy().into_owned(),
        user_data_root: state.to_string_lossy().into_owned(),
        tu2: true,
        extras: true,
        vulkan_device: 1,
        ui_state: serde_json::Value::Null,
    })
}

#[tauri::command]
fn save_settings(app: tauri::AppHandle, settings: Settings) -> Result<(), String> {
    let path = settings_file(&app)?;
    fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    fs::write(
        path,
        serde_json::to_vec_pretty(&settings).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())
}

fn launch_arguments(settings: &Settings, options: &GameOptions) -> Result<Vec<String>, String> {
    if !(1..=3).contains(&options.res_scale) || ![30, 60, 120, 144, 165, 240].contains(&options.fps)
    {
        return Err("Choose a supported resolution scale and frame limit.".into());
    }
    if !["fsr", "cas", "bilinear"].contains(&options.present.as_str()) {
        return Err("Unsupported presentation filter.".into());
    }
    let mut args = vec![
        format!("--game_data_root={}", settings.game_dir),
        format!("--user_data_root={}", settings.user_data_root),
        format!("--draw_resolution_scale_x={}", options.res_scale),
        format!("--draw_resolution_scale_y={}", options.res_scale),
        format!("--d3_frame_limit={}", options.fps),
        format!("--vsync={}", options.vsync),
        "--mnk_mode=true".into(),
        format!("--mnk_mouse={}", options.mnk_mouse),
        "--mnk_cursor_visible=false".into(),
        format!("--present_effect={}", options.present),
        format!("--fullscreen={}", options.fullscreen),
        format!("--user_language={}", options.lang_id),
        format!("--user_country={}", options.country_id),
    ];
    if settings.tu2 {
        args.push(format!("--update_data_root={}", settings.game_dir));
    }
    if settings.extras {
        args.push("--pc_use_saved_render_scale=false".into());
    }
    if cfg!(target_os = "linux") {
        args.push(format!("--vulkan_device={}", settings.vulkan_device));
        args.push("--render_target_path_vulkan=fsi".into());
    } else if cfg!(target_os = "windows") {
        if !["rov", "rtv"].contains(&options.rtp.as_str()) {
            return Err("Unsupported rendering path.".into());
        }
        args.push(format!("--render_target_path_d3d12={}", options.rtp));
        args.push("--rtv_color_depth_ownership_mode=8".into());
        args.push("--execute_unclipped_draw_vs_on_cpu=true".into());
    } else {
        return Err("This launcher prototype supports Linux and Windows.".into());
    }
    Ok(args)
}

#[tauri::command]
fn launch_game(
    app: tauri::AppHandle,
    process: tauri::State<GameProcess>,
    settings: Settings,
    options: GameOptions,
) -> Result<u32, String> {
    let mut child = process.0.lock().map_err(|e| e.to_string())?;
    if let Some(existing) = child.as_mut() {
        if existing.try_wait().map_err(|e| e.to_string())?.is_none() {
            return Err("The game is already running.".into());
        }
    }
    let mut settings = settings;
    settings.game_dir = fs::canonicalize(&settings.game_dir)
        .map_err(|e| format!("Game directory: {e}"))?
        .to_string_lossy()
        .into_owned();
    let executable =
        fs::canonicalize(&settings.executable).map_err(|e| format!("Game executable: {e}"))?;
    if !executable.is_file() {
        return Err("Select the diablo3 executable.".into());
    }
    let data = fs::read(Path::new(&settings.game_dir).join("Default.xex"))
        .map_err(|e| format!("Default.xex: {e}"))?;
    let expected = if settings.tu2 { TU2_HASH } else { BASE_HASH };
    if format!("{:x}", Sha256::digest(&data)) != expected {
        return Err("Default.xex does not match the selected base-disc/TU2 build.".into());
    }
    if settings.tu2 {
        for name in [
            "Patch.cpk",
            "Patch2.cpk",
            "enUS_Patch.cpk",
            "enUS_Patch2.cpk",
        ] {
            if !Path::new(&settings.game_dir)
                .join("CPKs")
                .join(name)
                .is_file()
            {
                return Err(format!("Missing TU2 asset: {name}"));
            }
        }
    }
    if settings.user_data_root.trim().is_empty() {
        return Err("Choose a save directory.".into());
    }
    fs::create_dir_all(&settings.user_data_root).map_err(|e| e.to_string())?;
    settings.user_data_root = fs::canonicalize(&settings.user_data_root)
        .map_err(|e| e.to_string())?
        .to_string_lossy()
        .into_owned();
    let mut args = launch_arguments(&settings, &options)?;
    let log_path = Path::new(&settings.user_data_root).join("launcher-game.log");
    args.push(format!("--log_file={}", log_path.display()));
    let output = fs::File::create(Path::new(&settings.user_data_root).join("launcher-stdio.log"))
        .map_err(|e| e.to_string())?;
    save_settings(app, settings)?;
    let game = Command::new(&executable)
        .args(args)
        .current_dir(executable.parent().unwrap())
        .stdout(Stdio::from(output.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(output))
        .spawn()
        .map_err(|e| format!("Unable to start game: {e}"))?;
    let pid = game.id();
    *child = Some(game);
    Ok(pid)
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

#[tauri::command]
fn frontend_report(message: String) {
    eprintln!("Launcher frontend: {message}");
}

fn main() {
    tauri::Builder::default()
        .manage(GameProcess::default())
        .invoke_handler(tauri::generate_handler![
            frontend_report,
            load_settings,
            save_settings,
            launch_game,
            window_action
        ])
        .run(tauri::generate_context!())
        .expect("Unable to run launcher");
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tu2_arguments_preserve_paths_and_explicit_scale() {
        let settings = Settings {
            game_dir: "/tmp/game with spaces".into(),
            executable: "diablo3".into(),
            user_data_root: "/tmp/save with spaces".into(),
            tu2: true,
            extras: true,
            vulkan_device: 1,
            ui_state: serde_json::Value::Null,
        };
        let options = GameOptions {
            fps: 60,
            vsync: true,
            mnk_mouse: false,
            present: "fsr".into(),
            res_scale: 2,
            fullscreen: true,
            lang_id: 1,
            country_id: 103,
            rtp: "rtv".into(),
        };
        let args = launch_arguments(&settings, &options).unwrap();
        assert!(args.contains(&"--update_data_root=/tmp/game with spaces".into()));
        assert!(args.contains(&"--pc_use_saved_render_scale=false".into()));
        assert!(args.contains(&"--draw_resolution_scale_x=2".into()));
        if cfg!(target_os = "linux") {
            assert!(args.contains(&"--vulkan_device=1".into()));
            assert!(!args.iter().any(|a| a.contains("d3d12")));
        }
        let mut invalid = options;
        invalid.res_scale = 4;
        assert!(launch_arguments(&settings, &invalid).is_err());
    }
}
