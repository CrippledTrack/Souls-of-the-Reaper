//! The Build panel: tool checks, tool installs (winget) and running
//! scripts/build_client.py.

use crate::build_info;
use crate::game::{self, Build, BuildRequest, Platform, Settings};
use crate::host::{write_settings, Host};
use crate::job::cancelled;
use crate::message::{msg, Message};
use crate::{sources, tools};
use std::{
    collections::VecDeque,
    io::{BufRead, BufReader, Read},
    path::PathBuf,
    process::{Command, Stdio},
    sync::{atomic::Ordering, Arc, Mutex},
};
use tauri::Emitter;

/// Cancels the build: a source fetch stops and the script is not started, or
/// the running script stops with the CMake/Ninja/compiler processes it started.
pub fn stop_build(host: &Host) {
    if let Some(pid) = host.job.cancel() {
        // The script leads its own process group on Unix (see start_build).
        tools::kill_tree(pid);
    }
}

/// Sends each output line to the page as a build-output event and keeps the
/// last lines for the error message. `clean` rewrites a line or drops it.
fn forward_output(
    app: tauri::AppHandle,
    stream: impl Read + Send + 'static,
    tail: Arc<Mutex<VecDeque<String>>>,
    clean: fn(&str) -> Option<String>,
) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        let mut reader = BufReader::new(stream);
        let mut buffer = Vec::new();
        while reader.read_until(b'\n', &mut buffer).is_ok_and(|n| n > 0) {
            let line = clean(String::from_utf8_lossy(&buffer).trim_end());
            buffer.clear();
            let Some(line) = line else { continue };
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
pub async fn start_build(
    app: tauri::AppHandle,
    host: tauri::State<'_, Host>,
    settings: Settings,
    request: BuildRequest,
) -> Result<Vec<Build>, Message> {
    // Held until this returns. A second build is turned away here, before the
    // tool check and the source fetch, not when the script would start.
    let claim = host.inner().job.claim(true)?;
    // Outside a checkout, the stable release's sources, or the launcher's own for Development.
    let target = host.build_root(&request.channel)?;
    let repo = target.root.clone();
    let (game, game_update) = game::game_folders(&settings, &host.defaults);
    let args = game::build_client_arguments(host.platform, &repo, &request, &game, &game_update)?;
    // Checked before downloading anything, so a missing tool costs no download.
    let report = tools::detect(host.platform, &repo, &request.cmake);
    if !report.ready {
        let missing: Vec<_> = report.tools.iter().filter(|t| !t.found).map(|t| t.label.as_str()).collect();
        return Err(msg!("err.build.missingTools", tools = missing.join(", ")));
    }
    let tool_path = |id: &str| report.tools.iter().find(|t| t.id == id).map(|t| PathBuf::from(&t.path));
    let fetch = target.commit.is_some() && !sources::is_complete(&repo);
    if let Some(commit) = target.commit.clone().filter(|_| fetch) {
        if !request.download_sources {
            return Err(msg!("err.build.sourcesNotApproved"));
        }
        let git = tool_path("git");
        let (root, app, cancel) = (repo.clone(), app.clone(), claim.cancel.clone());
        let fetched = tauri::async_runtime::spawn_blocking(move || {
            sources::ensure(
                &root,
                build_info::BuildInfo::repository(),
                &commit,
                git.as_deref(),
                &|line| {
                    let _ = app.emit("build-output", line);
                },
                &|| cancel.load(Ordering::SeqCst),
            )
        })
        .await
        .map_err(|e| e.to_string())?;
        if claim.cancelled() {
            return Err(cancelled());
        }
        fetched?;
    }
    // A shallow fetch brings no tags: name the stable release for the game's version label.
    if let (Some(tag), Some(git)) = (target.tag.as_deref(), tool_path("git")) {
        sources::tag_release(&repo, &git, tag);
    }
    // The SDK state comes from the sources' build script, which only now exists.
    let python = tool_path("python").unwrap_or_default();
    let report = if fetch { tools::detect(host.platform, &repo, &request.cmake) } else { report };
    // The SDK and the shader converter are downloaded under the same approval.
    let converter_missing = report.converter.as_ref().is_some_and(|c| !c.installed);
    if (!report.sdk.installed || converter_missing) && !request.download_sdk {
        return Err(msg!("err.build.sdkNotApproved"));
    }
    if claim.cancelled() {
        return Err(cancelled());
    }
    write_settings(&host.settings_file, &settings)?;

    // The interpreter the tool check accepted, not whatever "python" resolves to.
    let mut command = Command::new(&python);
    command
        .args(&args)
        .current_dir(&repo)
        .env("PYTHONUNBUFFERED", "1")
        .envs(tools::PYTHON_UTF8)
        // The SDK checkout and the update patch tool are fetched with git too.
        .envs(sources::GIT_NO_PROMPT)
        .envs(tools::path_with(&report.path_dirs).map(|p| ("PATH", p)))
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
    let mut child = command
        .spawn()
        .map_err(|e| msg!("err.build.python", detail = e))?;
    if !claim.started(child.id()) {
        tools::kill_tree(child.id());
        let _ = child.wait();
        return Err(cancelled());
    }
    let tail = Arc::new(Mutex::new(VecDeque::new()));
    let keep = |line: &str| Some(line.to_string());
    let readers = [
        forward_output(app.clone(), child.stdout.take().unwrap(), tail.clone(), keep),
        forward_output(app.clone(), child.stderr.take().unwrap(), tail.clone(), keep),
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
    if claim.cancelled() {
        return Err(cancelled());
    }
    if !status?.success() {
        let tail = tail.lock().unwrap();
        // The script's own reason (its output, shown as written), else its last line.
        let reason = tail.iter().rev().find(|l| l.starts_with("Build failed:")).or(tail.back()).cloned();
        return Err(reason.map_or_else(|| msg!("err.build.failed"), Message::from));
    }
    let builds = host.discover();
    *host.builds.lock().unwrap() = builds.clone();
    Ok(builds)
}

/// Build tools and SDK state for the Build panel; `cmake` is its CMake field.
#[tauri::command]
pub async fn check_build_tools(
    host: tauri::State<'_, Host>,
    cmake: String,
    channel: Option<String>,
) -> Result<tools::ToolReport, Message> {
    // The sources the panel is set to build (standalone): stable or Development.
    let target = host.build_root(channel.as_deref().unwrap_or_default())?;
    let repo = target.root.clone();
    let platform = host.platform;
    // Fetched sources that are not on disk yet: the panel asks before fetching them.
    let missing = target.commit.filter(|_| !sources::is_complete(&repo));
    tauri::async_runtime::spawn_blocking(move || {
        let mut report = tools::detect(platform, &repo, &cmake);
        if let Some(commit) = missing {
            report.sources = Some(tools::Sources {
                repository: build_info::BuildInfo::repository().into(),
                commit,
            });
        }
        report
    })
    .await
    .map_err(|e| e.to_string().into())
}

/// Installs the missing build tools with winget, one after another, streaming
/// its output like a build, and returns the tool report afterwards. The page
/// only calls this after the user pressed the install button, whose text says
/// that it accepts the packages' license agreements.
#[tauri::command]
pub async fn install_tools(
    app: tauri::AppHandle,
    host: tauri::State<'_, Host>,
    cmake: String,
) -> Result<tools::ToolReport, Message> {
    if host.platform != Platform::Windows {
        return Err(msg!("err.tools.windowsOnly"));
    }
    // The tools are the same for every build; the folder only decides where free
    // space and the SDK state are read.
    let repo = host.build_root("")?.root;
    // Held through the tool check at the end, so two installs never overlap
    // (the second would fail with MSI error 1618) and no build starts meanwhile.
    let _claim = host.inner().job.claim(false)?;
    let platform = host.platform;
    let (dir, app_handle, cmake_choice) = (repo.clone(), app.clone(), cmake.clone());
    let failed = tauri::async_runtime::spawn_blocking(move || {
        let report = tools::detect(platform, &dir, &cmake);
        let missing = tools::install_order(&report);
        let mut failed = Vec::new();
        for (index, tool) in missing.iter().enumerate() {
            let _ = app_handle.emit(
                "build-output",
                format!("::step::{}/{}::Installing {}", index + 1, missing.len(), tool.label),
            );
            let mut command = Command::new("winget");
            command
                .args(tools::winget_arguments(tool.winget))
                .stdin(Stdio::null())
                .stdout(Stdio::piped())
                .stderr(Stdio::piped());
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
            }
            let status = command.spawn().and_then(|mut child| {
                let tail = Arc::new(Mutex::new(VecDeque::new()));
                let readers = [
                    forward_output(app_handle.clone(), child.stdout.take().unwrap(), tail.clone(), tools::clean_winget_line),
                    forward_output(app_handle.clone(), child.stderr.take().unwrap(), tail, tools::clean_winget_line),
                ];
                let status = child.wait();
                for reader in readers {
                    let _ = reader.join();
                }
                status
            });
            // winget reports "already installed" and "restart needed" as failures too;
            // the tool check below decides what is really missing.
            if !status.is_ok_and(|s| s.success()) {
                failed.push(tool.label.clone());
            }
        }
        failed
    })
    .await
    .map_err(|e| e.to_string())?;
    if !failed.is_empty() {
        let _ = app.emit("build-output", format!("winget did not finish cleanly for: {}.", failed.join(", ")));
    }
    check_build_tools(host, cmake_choice, None).await
}

#[tauri::command]
pub fn cancel_build(host: tauri::State<Host>) {
    stop_build(&host);
}
