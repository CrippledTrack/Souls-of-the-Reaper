#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod build_info;
mod commands;
mod game;
mod host;
mod job;
mod message;
mod sources;
#[cfg(test)]
mod stable_pin;
mod tools;

use commands::{build, launch, profiles, saves, window};
use game::Platform;
use host::{load_settings, Caps, Host, Sources, Tree};
use job::JobSlot;
use std::{path::Path, sync::Mutex};
use tauri::{Manager, RunEvent, WebviewUrl, WebviewWindowBuilder};

fn setup(app: &mut tauri::App) -> Result<(), Box<dyn std::error::Error>> {
    let platform = Platform::current();
    let launcher_dir = std::env::current_exe()?
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_default();
    let mut search = vec![launcher_dir.clone()];
    search.extend(std::env::current_dir().ok());
    let repo = game::repository_root(&search);
    let info = build_info::BuildInfo::current();
    let stable = build_info::stable();
    // Outside a checkout the launcher fetches what it builds: the stable release for
    // builds, and its own commit (when that can be fetched) for Development builds.
    // Local, not roaming, app data on Windows: a build takes about 15 GB.
    let data_dir = app.path().local_data_dir().ok();
    let tree = |commit: &str| {
        sources::managed_root_for(&launcher_dir, data_dir.as_deref(), commit)
            .map(|root| Tree { root, commit: commit.to_string() })
    };
    let fetched = repo
        .is_none()
        .then(|| tree(&stable.commit))
        .flatten()
        .map(|stable_tree| Sources {
            stable: stable_tree,
            stable_tag: stable.tag.clone(),
            development: info.fetchable().then(|| tree(&info.commit)).flatten(),
        });
    let builds = host::discover(platform, &launcher_dir, &search, fetched.as_ref());
    // Source checkouts keep game folders at the repository root; installs
    // keep them beside the launcher, or with the fetched sources when those
    // went to the per-user folder because the launcher's is read-only. Both
    // fetched trees share these game folders.
    let base = repo
        .clone()
        .filter(|_| !builds.iter().any(|b| b.game_dir.is_some()))
        .or_else(|| fetched.as_ref().map(|f| sources::game_base(&launcher_dir, &f.stable.root)))
        .unwrap_or_else(|| launcher_dir.clone());
    let user_base = match platform {
        Platform::Windows => app.path().document_dir()?,
        Platform::Linux | Platform::MacOs => app.path().data_dir()?,
    };
    let defaults = game::default_folders(platform, &base, &user_base);
    let settings_file = app.path().app_config_dir()?.join("launcher.json");
    let mut settings = load_settings(&settings_file);
    if settings.ui_state.is_null() {
        // Extras builds start on their in-game resolution and window mode.
        settings.ui_state = serde_json::json!({ "res": -1, "display": -1 });
    }

    // The title update is an overlay in the base game folder: once its files are there, open on it.
    let update = game::update_installed(&builds, &settings, &defaults);
    game::apply_default_build(&mut settings.ui_state, &builds, update);

    let caps = Caps {
        platform: platform.name(),
        backends: match platform {
            Platform::Windows => &["d3d12", "vulkan"],
            Platform::Linux | Platform::MacOs => &["vulkan"],
        },
        builds: &builds,
        defaults: &defaults,
        can_build: repo.as_ref().is_some_and(|r| r.join("scripts/build_client.py").is_file())
            || (repo.is_none() && fetched.is_some()),
        can_build_development: fetched.as_ref().is_some_and(|f| f.development.is_some()),
        update_installed: update,
        version: build_info::BuildInfo::current(),
        stable,
        standalone: repo.is_none(),
    };
    // The page shows its Player Name row from the name reported here.
    let mut saved_state = settings.ui_state.clone();
    if let Some(state) = saved_state.as_object_mut() {
        state.insert("username".into(), settings.username.clone().into());
    }
    let init = format!(
        "window.LAUNCHER_CAPS={};window.LAUNCHER_SETTINGS={};window.SAVED_STATE={};",
        serde_json::to_string(&caps)?,
        serde_json::to_string(&settings)?,
        serde_json::to_string(&saved_state)?,
    );
    app.manage(Host {
        platform,
        builds: Mutex::new(builds),
        defaults,
        settings_file,
        launcher_dir,
        search,
        repo,
        sources: fetched,
        job: JobSlot::default(),
    });
    // Sized to the design's 500px stage (aspect 1088/1445).
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
            launch::save_settings,
            launch::launch_game,
            build::start_build,
            build::cancel_build,
            build::check_build_tools,
            build::install_tools,
            window::pick_file,
            window::open_external,
            window::window_action,
            saves::development_saves,
            saves::prepare_saves,
            profiles::list_profiles,
            profiles::create_profile,
            profiles::rename_profile,
            profiles::delete_profile
        ])
        .build(tauri::generate_context!())
        .expect("Unable to run launcher")
        .run(|app, event| {
            // Closing the launcher also stops a running build.
            if let (RunEvent::Exit, Some(host)) = (event, app.try_state::<Host>()) {
                build::stop_build(&host);
            }
        });
}
