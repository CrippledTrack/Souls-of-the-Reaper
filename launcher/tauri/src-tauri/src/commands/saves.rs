//! The first launch of a Development build: offer the normal saves.

use crate::game::{self, Build, Settings};
use crate::host::Host;
use crate::message::{msg, Message};
use serde::Serialize;

/// Folders the page names when it asks.
#[derive(Debug, Serialize)]
pub struct SaveOffer {
    from: String,
    to: String,
}

fn find_build(host: &Host, id: &str) -> Result<Build, Message> {
    host.builds.lock().unwrap().iter().find(|b| b.id == id).cloned().ok_or(msg!("err.launch.noBuild"))
}

/// Whether to ask before launching `build`: a Development build whose own save
/// folder has no saves yet, while the matching normal folder has some.
#[tauri::command]
pub fn development_saves(host: tauri::State<Host>, settings: Settings, build: String) -> Result<Option<SaveOffer>, Message> {
    let build = find_build(&host, &build)?;
    Ok(game::copy_offer(&build, &settings, &host.defaults).map(|(from, to)| SaveOffer {
        from: from.display().to_string(),
        to: to.display().to_string(),
    }))
}

/// The user's answer: copy the normal saves (`copy`), or start fresh.
#[tauri::command]
pub async fn prepare_saves(host: tauri::State<'_, Host>, settings: Settings, build: String, copy: bool) -> Result<(), Message> {
    let build = find_build(&host, &build)?;
    let Some((from, to)) = game::copy_offer(&build, &settings, &host.defaults) else {
        return Ok(()); // Answered already (another click, or saves appeared since).
    };
    tauri::async_runtime::spawn_blocking(move || {
        if copy {
            game::copy_saves(&from, &to)
                .map(|_| ())
                .map_err(|e| msg!("err.saves.copy", from = from.display(), to = to.display(), detail = e))
        } else {
            game::start_fresh(&to).map_err(|e| msg!("err.saves.fresh", folder = to.display(), detail = e))
        }
    })
    .await
    .map_err(|e| Message::from(e.to_string()))?
}
