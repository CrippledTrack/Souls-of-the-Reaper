//! Window controls, file pickers and external links.

use std::process::{Command, Stdio};
use crate::message::{msg, Message};
use tauri_plugin_dialog::DialogExt;

/// Native file picker for the Build panel; None when dismissed.
#[tauri::command]
pub async fn pick_file(app: tauri::AppHandle, kind: String) -> Result<Option<String>, Message> {
    let dialog = app.dialog().file();
    let dialog = match kind.as_str() {
        "iso" => dialog
            .set_title("Select the Diablo III disc image")
            .add_filter("Xbox 360 disc image", &["iso", "xiso"]),
        "titleUpdate" => dialog.set_title("Select the Title Update package"),
        "cmake" => dialog.set_title("Select the CMake executable"),
        _ => return Err(msg!("err.window.fileType")),
    };
    let picked = tauri::async_runtime::spawn_blocking(move || dialog.blocking_pick_file())
        .await
        .map_err(|e| e.to_string())?;
    Ok(picked
        .and_then(|p| p.into_path().ok())
        .map(|p| p.display().to_string()))
}

/// Opens a page link in the default browser. A Tauri webview ignores
/// target="_blank", so the page asks here.
#[tauri::command]
pub fn open_external(url: String) -> Result<(), Message> {
    if !is_external_url(&url) {
        return Err(msg!("err.window.httpsOnly"));
    }
    let mut command = if cfg!(target_os = "windows") {
        // No cmd.exe: its own parsing would treat & in a URL as a command separator.
        let mut c = Command::new("rundll32");
        c.arg("url.dll,FileProtocolHandler");
        c
    } else if cfg!(target_os = "macos") {
        Command::new("open")
    } else {
        Command::new("xdg-open")
    };
    command
        .arg(&url)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map(|_| ())
        .map_err(|e| msg!("err.window.openLink", detail = e))
}

#[tauri::command]
pub fn window_action(window: tauri::WebviewWindow, action: String) -> Result<(), Message> {
    match action.as_str() {
        "close" => window.close(),
        "minimize" => window.minimize(),
        "dragStart" => window.start_dragging(),
        _ => return Err(msg!("err.window.action")),
    }
    .map_err(|e| e.to_string().into())
}

/// Page links (the font credit under Play) that open in the default browser.
/// Only plain web URLs: no spaces, quotes or shell metacharacters, so the value
/// is safe to hand to the OS opener as a single argument.
pub fn is_external_url(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("https://") else {
        return false;
    };
    !rest.is_empty()
        && url.len() <= 2048
        && url.chars().all(|c| c.is_ascii_alphanumeric() || "-._~:/?#[]@!$&()*+,;=%".contains(c))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_plain_https_links_open_externally() {
        assert!(is_external_url("https://fernandoforeroart.com/old-fenris-diablo-iv-official-font"));
        assert!(!is_external_url("http://example.com"));
        assert!(!is_external_url("file:///C:/Windows/System32/calc.exe"));
        assert!(!is_external_url("javascript:alert(1)"));
        assert!(!is_external_url("https://"));
        assert!(!is_external_url("https://example.com/a b"));
        assert!(!is_external_url("https://example.com/\"&calc.exe"));
        assert!(!is_external_url("https://example.com/a|b"));
    }
}
