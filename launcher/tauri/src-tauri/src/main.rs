#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod launch;

use launch::{Build, BuildRequest, Defaults, GameOptions, Platform, Settings};
use serde::Serialize;
use std::{
    collections::VecDeque,
    fs,
    io::{BufRead, BufReader, Read},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};
use tauri::{Emitter, Manager, RunEvent, WebviewUrl, WebviewWindowBuilder};
use tauri_plugin_dialog::DialogExt;

/// Folder defaults resolved at startup; builds are found again after the
/// Build panel finishes.
struct Host {
    platform: Platform,
    builds: Mutex<Vec<Build>>,
    defaults: Defaults,
    settings_file: PathBuf,
    launcher_dir: PathBuf,
    search: Vec<PathBuf>,
    /// Source checkout, needed to build the game.
    repo: Option<PathBuf>,
    /// Process id of the running scripts/build_client.py.
    build_pid: Mutex<Option<u32>>,
}

/// Injected ahead of the page scripts as window.LAUNCHER_CAPS.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Caps<'a> {
    platform: &'static str,
    builds: &'a [Build],
    defaults: &'a Defaults,
    can_build: bool,
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
    let build = {
        let builds = host.builds.lock().unwrap();
        match &options.build {
            Some(id) => builds.iter().find(|b| &b.id == id),
            None => builds.first(),
        }
        .ok_or("No game build found. Build the game or place the launcher next to diablo3.")?
        .clone()
    };
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

fn python() -> Command {
    Command::new(if cfg!(target_os = "windows") { "python" } else { "python3" })
}

/// Stops the build script and the CMake/Ninja/compiler processes it started.
fn stop_build(host: &Host) {
    let Some(pid) = host.build_pid.lock().unwrap().take() else {
        return;
    };
    #[cfg(unix)]
    unsafe {
        // The script leads its own process group (see start_build).
        libc::kill(-(pid as i32), libc::SIGTERM);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        let _ = Command::new("taskkill")
            .args(["/PID", &pid.to_string(), "/T", "/F"])
            .creation_flags(0x0800_0000) // CREATE_NO_WINDOW
            .status();
    }
}

/// Sends each output line to the page as a build-output event and keeps the
/// last lines for the error message.
fn forward_output(
    app: tauri::AppHandle,
    stream: impl Read + Send + 'static,
    tail: Arc<Mutex<VecDeque<String>>>,
) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        let mut reader = BufReader::new(stream);
        let mut buffer = Vec::new();
        while reader.read_until(b'\n', &mut buffer).is_ok_and(|n| n > 0) {
            let line = String::from_utf8_lossy(&buffer).trim_end().to_string();
            buffer.clear();
            let _ = app.emit("build-output", &line);
            let mut tail = tail.lock().unwrap();
            if tail.len() == 20 {
                tail.pop_front();
            }
            tail.push_back(line);
        }
    })
}

/// Runs scripts/build_client.py into the launcher's game folders, streaming
/// its output, and returns the builds found afterwards.
#[tauri::command]
async fn start_build(
    app: tauri::AppHandle,
    host: tauri::State<'_, Host>,
    settings: Settings,
    request: BuildRequest,
) -> Result<Vec<Build>, String> {
    let repo = host
        .repo
        .clone()
        .ok_or("Building needs the Souls of the Reaper source checkout.")?;
    let (game, game_tu2) = launch::game_folders(&settings, &host.defaults);
    let args = launch::build_client_arguments(host.platform, &repo, &request, &game, &game_tu2)?;
    write_settings(&host.settings_file, &settings)?;

    let mut command = python();
    command
        .args(&args)
        .current_dir(&repo)
        .env("PYTHONUNBUFFERED", "1")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        command.process_group(0);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let mut child = {
        let mut running = host.build_pid.lock().unwrap();
        if running.is_some() {
            return Err("A build is already running.".into());
        }
        let child = command
            .spawn()
            .map_err(|e| format!("Unable to start Python 3: {e}. Install Python 3 to build the game."))?;
        *running = Some(child.id());
        child
    };
    let tail = Arc::new(Mutex::new(VecDeque::new()));
    let readers = [
        forward_output(app.clone(), child.stdout.take().unwrap(), tail.clone()),
        forward_output(app.clone(), child.stderr.take().unwrap(), tail.clone()),
    ];
    let status = tauri::async_runtime::spawn_blocking(move || {
        let status = child.wait();
        for reader in readers {
            let _ = reader.join();
        }
        status
    })
    .await
    .map_err(|e| e.to_string())?
    .map_err(|e| e.to_string());
    let cancelled = host.build_pid.lock().unwrap().take().is_none();
    if cancelled {
        return Err("Build cancelled. Run it again to continue where it stopped.".into());
    }
    if !status?.success() {
        let tail = tail.lock().unwrap();
        let reason = tail
            .iter()
            .rev()
            .find(|l| l.starts_with("Build failed:"))
            .or(tail.back())
            .cloned()
            .unwrap_or_else(|| "Build failed.".into());
        return Err(reason);
    }
    let builds = launch::discover_builds(host.platform, &host.launcher_dir, &host.search);
    *host.builds.lock().unwrap() = builds.clone();
    Ok(builds)
}

#[tauri::command]
fn cancel_build(host: tauri::State<Host>) {
    stop_build(&host);
}

/// Native file picker for the Build panel; None when dismissed.
#[tauri::command]
async fn pick_file(app: tauri::AppHandle, kind: String) -> Result<Option<String>, String> {
    let dialog = app.dialog().file();
    let dialog = match kind.as_str() {
        "iso" => dialog
            .set_title("Select the Diablo III disc image")
            .add_filter("Xbox 360 disc image", &["iso", "xiso"]),
        "titleUpdate" => dialog.set_title("Select the Title Update 2 package (tu00000002_00000000)"),
        "cmake" => dialog.set_title("Select the CMake executable"),
        _ => return Err("Unknown file type.".into()),
    };
    let picked = tauri::async_runtime::spawn_blocking(move || dialog.blocking_pick_file())
        .await
        .map_err(|e| e.to_string())?;
    Ok(picked
        .and_then(|p| p.into_path().ok())
        .map(|p| p.display().to_string()))
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
    let repo = launch::repository_root(&search);
    // Source checkouts keep game folders at the repository root; installs
    // keep them beside the launcher.
    let base = repo
        .clone()
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
        can_build: repo.as_ref().is_some_and(|r| r.join("scripts/build_client.py").is_file()),
    };
    let init = format!(
        "window.LAUNCHER_CAPS={};window.LAUNCHER_SETTINGS={};window.SAVED_STATE={};",
        serde_json::to_string(&caps)?,
        serde_json::to_string(&settings)?,
        serde_json::to_string(&settings.ui_state)?,
    );
    app.manage(Host {
        platform,
        builds: Mutex::new(builds),
        defaults,
        settings_file,
        launcher_dir,
        search,
        repo,
        build_pid: Mutex::new(None),
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
        .plugin(tauri_plugin_dialog::init())
        .setup(setup)
        .invoke_handler(tauri::generate_handler![
            save_settings,
            launch_game,
            start_build,
            cancel_build,
            pick_file,
            window_action
        ])
        .build(tauri::generate_context!())
        .expect("Unable to run launcher")
        .run(|app, event| {
            // Closing the launcher also stops a running build.
            if let (RunEvent::Exit, Some(host)) = (event, app.try_state::<Host>()) {
                stop_build(&host);
            }
        });
}
