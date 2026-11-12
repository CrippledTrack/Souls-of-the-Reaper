//! Saving the settings and starting the game.

use crate::game::{self, GameOptions, Settings};
use crate::host::{write_settings, Host};
use crate::message::{msg, Message};
use std::{
    fs,
    path::Path,
    process::{Command, Stdio},
    time::{Duration, Instant},
};

#[tauri::command]
pub fn save_settings(host: tauri::State<Host>, settings: Settings) -> Result<(), Message> {
    Ok(write_settings(&host.settings_file, &settings)?)
}

/// Grants the game the foreground; Windows otherwise opens it behind the
/// closing launcher.
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
pub async fn launch_game(
    window: tauri::WebviewWindow,
    host: tauri::State<'_, Host>,
    settings: Settings,
    mut options: GameOptions,
) -> Result<(), Message> {
    // A launch setting (Launch settings panel), not a menu choice.
    options.video_driver = settings.video_driver.trim().to_string();
    let build = {
        let builds = host.builds.lock().unwrap();
        match &options.build {
            Some(id) => builds.iter().find(|b| &b.id == id),
            None => builds.first(),
        }
        .ok_or(msg!("err.launch.noBuild"))?
        .clone()
    };
    // The Shadow renderer needs its shader converter staged beside the executable
    // (and releases before it was built in do not have it at all).
    options.shadow = options.shadow && build.shadow;
    // Native resolution and Auto aspect ratio follow the display the launcher is on.
    // The toolkit rounds fractional display scales up, so its size is checked against
    // the displays' native modes.
    options.monitor = window.current_monitor().ok().flatten().map(|m| {
        game::true_monitor_size(
            (m.size().width, m.size().height),
            m.scale_factor(),
            &game::native_display_modes(Path::new("/sys/class/drm")),
        )
    });
    let (game, user) = game::folders_for(&build, &settings, &host.defaults);
    let game = fs::canonicalize(&game)
        .map_err(|e| msg!("err.launch.gameFolder", folder = game.display(), detail = e))?;
    let game = game::plain_path(game);
    game::validate_game_dir(&build, &game)?;
    fs::create_dir_all(&user).map_err(|e| msg!("err.launch.saveFolder", folder = user.display(), detail = e))?;
    let user = game::plain_path(fs::canonicalize(&user).map_err(|e| e.to_string())?);
    let args = game::launch_arguments(
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
        // Player 1's profile; without one ("ingame", or none yet) the game asks for it.
        .env("SOTR_PROFILE", game::launch_profile(&settings.profile))
        .env_remove("SOTR_USERNAME")
        .current_dir(build.executable.parent().unwrap())
        .stdout(Stdio::from(output.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(output))
        .spawn()
        .map_err(|e| msg!("err.launch.start", program = build.executable.display(), detail = e))?;
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
        return Err(msg!("err.launch.exited", status = status, log = stdio_log.display()));
    }
    Ok(window.close().map_err(|e| e.to_string())?)
}
