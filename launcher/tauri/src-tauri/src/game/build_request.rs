//! The Build panel's request and the scripts/build_client.py arguments it becomes.

use super::*;
use crate::message::{msg, Message};
use serde::Deserialize;
use std::path::Path;

/// Build ids scripts/build_client.py accepts.
/// `-plain` compiles without the optional PC features.
pub const BUILD_VARIANTS: [&str; 4] = ["base", "update", "base-plain", "update-plain"];

/// Inputs of the Build panel.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BuildRequest {
    pub iso: String,
    pub title_update: String,
    pub variants: Vec<String>,
    pub cmake: String,
    /// The user agreed to fetch and build the pinned ReXGlue SDK when it is missing.
    #[serde(default)]
    pub download_sdk: bool,
    /// The user agreed to fetch the repository sources when the launcher has no checkout.
    #[serde(default)]
    pub download_sources: bool,
    /// Which sources a standalone launcher builds: "stable" (also when empty) or
    /// "development" (host.rs). A source checkout builds itself either way.
    #[serde(default)]
    pub channel: String,
}

/// Arguments for scripts/build_client.py, writing into the launcher's game
/// folders. The script compiles with build_linux.py or build_windows.py,
/// whichever matches the host.
pub fn build_client_arguments(
    _platform: Platform,
    repo: &Path,
    request: &BuildRequest,
    game: &Path,
    game_update: &Path,
) -> Result<Vec<String>, Message> {
    let mut args = vec![
        repo.join("scripts/build_client.py").display().to_string(),
        format!("--game-dir={}", game.display()),
    ];
    // One folder for both means update is built as an overlay inside it.
    if game_update != game {
        args.push(format!("--game-dir-update={}", game_update.display()));
    }
    for (flag, value) in [
        ("--iso", &request.iso),
        ("--title-update", &request.title_update),
        ("--cmake", &request.cmake),
    ] {
        if !value.trim().is_empty() {
            args.push(format!("{flag}={}", value.trim()));
        }
    }
    if let Some(bad) = request
        .variants
        .iter()
        .find(|v| !BUILD_VARIANTS.contains(&v.as_str()))
    {
        return Err(msg!("err.build.unknownVariant", variant = bad));
    }
    if request.variants.is_empty() {
        return Err(msg!("err.build.noVariant"));
    }
    args.push(format!("--variants={}", request.variants.join(",")));
    if !request.download_sdk {
        args.push("--no-sdk-download".into());
    }
    Ok(args)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::test_support::*;

    #[test]
    fn build_arguments_target_launcher_game_folders() {
        let args = build_client_arguments(
            Platform::Linux,
            Path::new("/repo"),
            &request(&["base", "update-plain"]),
            Path::new("/g/base"),
            Path::new("/g/update"),
        )
        .unwrap();
        let script = Path::new("/repo").join("scripts/build_client.py");
        assert_eq!(
            args,
            [
                script.to_str().unwrap(),
                "--game-dir=/g/base",
                "--game-dir-update=/g/update",
                "--iso=/isos/d3 usa.iso",
                "--variants=base,update-plain",
                "--no-sdk-download",
            ]
        );
        let mut approved = request(&["base"]);
        approved.download_sdk = true;
        let args = build_client_arguments(
            Platform::Linux,
            Path::new("/repo"),
            &approved,
            Path::new("/g"),
            Path::new("/g"),
        )
        .unwrap();
        assert!(!args.contains(&"--no-sdk-download".to_string()));
        assert!(!args.iter().any(|a| a.starts_with("--profile")), "every build is the normal one");
    }

    #[test]
    fn build_arguments_reject_unknown_or_missing_builds() {
        let folders = (Path::new("/g"), Path::new("/t"));
        let run = |platform, variants: &[&str]| {
            build_client_arguments(platform, Path::new("/r"), &request(variants), folders.0, folders.1)
        };
        for platform in [Platform::Linux, Platform::Windows, Platform::MacOs] {
            assert!(run(platform, &[]).is_err());
            assert!(run(platform, &["base;rm"]).is_err());
        }
        let windows = run(Platform::Windows, &["base", "update"]).unwrap();
        assert!(windows.contains(&"--variants=base,update".to_string()));
        assert!(!windows.contains(&"--data-only".to_string()));
    }
}
