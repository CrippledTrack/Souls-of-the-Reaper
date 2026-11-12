//! State of the pinned components the build fetches and builds once, the ReXGlue
//! SDK and the shader converter (XenosRecomp), as the build script reports them.

use super::*;
use crate::game::Platform;
use std::{
    fs,
    path::Path,
    process::{Command, Stdio},
};

/// Asks scripts/build_client.py, which owns the install paths and the stamp of
/// the commit and patches an SDK was built from. None when it cannot be run.
pub fn sdk_status_from_script(python: &Path, repo: &Path) -> Option<(Sdk, Option<Sdk>)> {
    let script = repo.join("scripts/build_client.py");
    if !script.is_file() {
        return None;
    }
    let mut command = Command::new(python);
    command
        .arg(script)
        .arg("--print-sdk-status")
        .current_dir(repo)
        .envs(PYTHON_UTF8)
        .stdin(Stdio::null())
        .stderr(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let output = command.output().ok().filter(|o| o.status.success())?;
    let text = String::from_utf8_lossy(&output.stdout);
    Some((parse_sdk_status(&text)?, parse_converter_status(&text)))
}

pub(crate) fn parse_sdk_status(text: &str) -> Option<Sdk> {
    let status: serde_json::Value = serde_json::from_str(text.trim()).ok()?;
    component(&status)
}

/// The shader converter from the same status line; None for sources from before
/// it was a regular component (they do not report it).
pub(crate) fn parse_converter_status(text: &str) -> Option<Sdk> {
    let status: serde_json::Value = serde_json::from_str(text.trim()).ok()?;
    component(&status["converter"])
}

/// {state, repository, commit} of a pinned component.
fn component(status: &serde_json::Value) -> Option<Sdk> {
    let state = status["state"].as_str()?;
    let text = |key: &str| status[key].as_str().unwrap_or_default().to_string();
    Some(Sdk {
        installed: state == "current",
        stale: state == "stale",
        repository: text("repository"),
        commit: text("commit"),
    })
}

/// Whether the patched SDK is built where scripts/build_client.py looks for it
/// (tools/rexglue-install-* and, on Windows, its codegen tool), and the pinned
/// revision it would fetch otherwise (port/base/rexglue.lock.json).
pub fn sdk_status(platform: Platform, repo: &Path) -> Sdk {
    let lock: serde_json::Value = fs::read(repo.join("port/base/rexglue.lock.json"))
        .ok()
        .and_then(|d| serde_json::from_slice(&d).ok())
        .unwrap_or_default();
    let text = |key: &str| lock[key].as_str().unwrap_or_default().to_string();
    let tools = repo.join("tools");
    let installed = match platform {
        Platform::Windows => {
            tools.join("rexglue-install-win/lib/cmake/rexglue").is_dir()
                && tools.join("rexglue-sdk-win/out/win-amd64/RelWithDebInfo/rexgluerd.exe").is_file()
        }
        Platform::Linux => tools.join("rexglue-install-linux/bin/rexglue").is_file(),
        // build_macos.py: a Rosetta (x86_64) launcher looks for the Intel SDK install.
        Platform::MacOs if cfg!(target_arch = "x86_64") => {
            tools.join("rexglue-install-macos-x86_64/bin/rexglue").is_file()
        }
        Platform::MacOs => tools.join("rexglue-install-macos/bin/rexglue").is_file(),
    };
    Sdk {
        installed,
        stale: false,
        repository: text("repository"),
        commit: text("commit"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[allow(unused_imports)]
    use crate::tools::test_support::*;

    #[test]
    fn sdk_status_reads_the_pinned_revision_and_install_state() {
        let repo = scratch("sdk");
        fs::create_dir_all(repo.join("port/base")).unwrap();
        fs::write(
            repo.join("port/base/rexglue.lock.json"),
            r#"{"repository":"https://example.test/sdk.git","commit":"abc123"}"#,
        )
        .unwrap();
        let sdk = sdk_status(Platform::Linux, &repo);
        assert!(!sdk.installed);
        assert_eq!((sdk.repository.as_str(), sdk.commit.as_str()), ("https://example.test/sdk.git", "abc123"));
        fs::create_dir_all(repo.join("tools/rexglue-install-linux/bin")).unwrap();
        fs::write(repo.join("tools/rexglue-install-linux/bin/rexglue"), "").unwrap();
        assert!(sdk_status(Platform::Linux, &repo).installed);
        assert!(!sdk_status(Platform::Windows, &repo).installed);
    }

    #[test]
    fn script_sdk_status_maps_its_state() {
        let line = r#"{"state": "stale", "repository": "r", "commit": "c"}"#;
        let sdk = parse_sdk_status(line).unwrap();
        assert!(!sdk.installed && sdk.stale);
        assert_eq!((sdk.repository.as_str(), sdk.commit.as_str()), ("r", "c"));
        assert!(parse_sdk_status(r#"{"state": "current"}"#).unwrap().installed);
        assert!(!parse_sdk_status(r#"{"state": "missing"}"#).unwrap().stale);
        assert!(parse_sdk_status("not json").is_none());
    }

    #[test]
    fn script_status_reports_the_shader_converter() {
        let line = r#"{"state": "current", "repository": "r", "commit": "c",
            "converter": {"state": "missing", "repository": "https://github.com/hedge-dev/XenosRecomp.git", "commit": "990d03b"}}"#;
        let converter = parse_converter_status(line).unwrap();
        assert!(!converter.installed && !converter.stale);
        assert_eq!(converter.commit, "990d03b");
        assert!(parse_sdk_status(line).unwrap().installed, "the SDK is read apart");
        assert!(parse_converter_status(r#"{"state": "current"}"#).is_none(), "older sources do not report one");
    }
}
