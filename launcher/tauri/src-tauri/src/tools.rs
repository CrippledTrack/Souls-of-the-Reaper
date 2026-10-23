//! Detects the tools the Build panel needs before scripts/build_client.py runs,
//! so a machine without Python or a compiler gets download links instead of a
//! failed build. Kept free of Tauri like launch.rs. Windows has no Python by
//! default, so none of this can live in the build scripts.

use crate::launch::Platform;
use serde::Serialize;
use std::{
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

const MIN_CMAKE: (u32, u32) = (3, 25);
const VS_COMPONENT: &str = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Tool {
    pub id: &'static str,
    pub label: String,
    pub found: bool,
    pub path: String,
    /// Where to download it; empty when the system package manager is the way.
    pub url: &'static str,
    pub note: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Sdk {
    pub installed: bool,
    pub repository: String,
    pub commit: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ToolReport {
    pub tools: Vec<Tool>,
    pub sdk: Sdk,
    /// Every required tool was found.
    pub ready: bool,
    /// Folders of tools found off PATH (Visual Studio's CMake, LLVM); the build
    /// script's PATH needs them too.
    #[serde(skip)]
    pub path_dirs: Vec<PathBuf>,
}

fn exe(name: &str, platform: Platform) -> String {
    match platform {
        Platform::Windows => format!("{name}.exe"),
        Platform::Linux => name.to_string(),
    }
}

/// First `name` found in `dirs`.
pub fn find_program(name: &str, platform: Platform, dirs: &[PathBuf]) -> Option<PathBuf> {
    let file = exe(name, platform);
    dirs.iter().map(|d| d.join(&file)).find(|p| p.is_file())
}

/// Stdout of `program args`, or None when it cannot run or fails (this also
/// rejects the Microsoft Store `python.exe` stub on Windows).
fn output_of(program: &Path, args: &[&str]) -> Option<String> {
    let mut command = Command::new(program);
    command
        .args(args)
        .stdin(Stdio::null())
        .stderr(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let output = command.output().ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).into_owned())
}

/// "cmake version 3.31.6" -> (3, 31).
pub fn parse_version(text: &str) -> Option<(u32, u32)> {
    let digits = text.split(|c: char| !(c.is_ascii_digit() || c == '.'));
    digits.filter(|w| w.contains('.')).find_map(|word| {
        let mut parts = word.split('.');
        Some((parts.next()?.parse().ok()?, parts.next()?.parse().ok()?))
    })
}

/// Visual Studio install that has the C++ x64 tools, via vswhere.
fn visual_studio() -> Option<PathBuf> {
    let files = std::env::var_os("ProgramFiles(x86)").map(PathBuf::from)?;
    let vswhere = files.join("Microsoft Visual Studio/Installer/vswhere.exe");
    let found = output_of(
        &vswhere,
        &["-latest", "-products", "*", "-requires", VS_COMPONENT, "-property", "installationPath"],
    )?;
    let path = PathBuf::from(found.trim());
    (!found.trim().is_empty() && path.is_dir()).then_some(path)
}

fn tool(id: &'static str, label: &str, path: Option<&Path>, url: &'static str, note: &str) -> Tool {
    Tool {
        id,
        label: label.into(),
        found: path.is_some(),
        path: path.map(|p| p.display().to_string()).unwrap_or_default(),
        url,
        note: note.into(),
    }
}

fn package_hint(packages: &str) -> String {
    format!("Install it with your package manager (for example: {packages}).")
}

/// Status of every tool a build needs. `cmake_choice` is the Build panel's
/// CMake field (blank = look it up).
pub fn detect(platform: Platform, repo: &Path, cmake_choice: &str) -> ToolReport {
    let dirs: Vec<PathBuf> = std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).collect())
        .unwrap_or_default();
    let mut extra = Vec::new();
    let studio = (platform == Platform::Windows).then(visual_studio).flatten();
    if platform == Platform::Windows {
        let program_files = std::env::var_os("ProgramFiles")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(r"C:\Program Files"));
        extra.push(program_files.join("LLVM/bin"));
        let mut roots = vec![program_files.join("Microsoft Visual Studio/2022/Community")];
        roots.extend(studio.clone());
        for root in roots {
            let cmake_dir = root.join("Common7/IDE/CommonExtensions/Microsoft/CMake");
            extra.push(cmake_dir.join("CMake/bin"));
            extra.push(cmake_dir.join("Ninja"));
        }
    }
    let lookup = |name: &str| {
        find_program(name, platform, &dirs).or_else(|| find_program(name, platform, &extra))
    };

    let python = lookup(if platform == Platform::Windows { "python" } else { "python3" })
        .filter(|p| output_of(p, &["--version"]).and_then(|v| parse_version(&v)).is_some_and(|v| v.0 >= 3));
    let git = lookup("git");
    let clang = lookup("clang").zip(lookup("clang++")).map(|(_, plus)| plus);
    let cmake = match cmake_choice.trim() {
        "" => lookup("cmake"),
        chosen => Some(PathBuf::from(chosen)).filter(|p| p.is_file()),
    };
    let cmake_ok = cmake
        .as_deref()
        .filter(|p| output_of(p, &["--version"]).and_then(|v| parse_version(&v)).is_some_and(|v| v >= MIN_CMAKE));
    let ninja = lookup("ninja");

    let linux = platform == Platform::Linux;
    let mut tools = vec![
        tool(
            "python",
            "Python 3",
            python.as_deref(),
            "https://www.python.org/downloads/",
            if linux {
                "Runs the build scripts."
            } else {
                "Runs the build scripts. Tick 'Add python.exe to PATH' in the installer."
            },
        ),
        tool(
            "git",
            "Git",
            git.as_deref(),
            "https://git-scm.com/downloads",
            "Fetches the ReXGlue SDK and the update patch tool.",
        ),
    ];
    if platform == Platform::Windows {
        tools.push(tool(
            "msvc",
            "Visual Studio 2022 C++ tools",
            studio.as_deref(),
            "https://visualstudio.microsoft.com/downloads/",
            "Free Community edition; install the 'Desktop development with C++' workload.",
        ));
    }
    tools.push(tool(
        "clang",
        if linux { "Clang 18+" } else { "LLVM Clang 22+" },
        clang.as_deref(),
        if linux { "" } else { "https://github.com/llvm/llvm-project/releases/latest" },
        &if linux { package_hint("apt install clang, pacman -S clang") } else { "Installer: LLVM-<version>-win64.exe.".into() },
    ));
    let cmake_note = match (&cmake, &cmake_ok) {
        (Some(_), None) => "Found, but older than CMake 3.25.".to_string(),
        _ if linux => package_hint("apt install cmake, pacman -S cmake"),
        _ => "Also included with the Visual Studio 'C++ CMake tools' component.".into(),
    };
    tools.push(tool(
        "cmake",
        "CMake 3.25+",
        cmake_ok,
        "https://cmake.org/download/",
        &cmake_note,
    ));
    tools.push(tool(
        "ninja",
        "Ninja",
        ninja.as_deref(),
        "https://github.com/ninja-build/ninja/releases/latest",
        &if linux { package_hint("apt install ninja-build, pacman -S ninja") } else { "Also included with the Visual Studio 'C++ CMake tools' component.".into() },
    ));

    // Only folders the machine's own PATH lacks need adding for the build script.
    let mut path_dirs = Vec::new();
    for found in [&clang, &cmake, &ninja].into_iter().flatten() {
        if let Some(dir) = found.parent() {
            if !dirs.iter().any(|d| d == dir) && !path_dirs.iter().any(|d: &PathBuf| d == dir) {
                path_dirs.push(dir.to_path_buf());
            }
        }
    }

    ToolReport {
        ready: tools.iter().all(|t| t.found),
        tools,
        sdk: sdk_status(platform, repo),
        path_dirs,
    }
}

/// `PATH` for the build script: `extra` ahead of the inherited one.
pub fn path_with(extra: &[PathBuf]) -> Option<OsString> {
    if extra.is_empty() {
        return None;
    }
    let inherited = std::env::var_os("PATH").unwrap_or_default();
    let all = extra.iter().cloned().chain(std::env::split_paths(&inherited));
    std::env::join_paths(all).ok()
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
    };
    Sdk {
        installed,
        repository: text("repository"),
        commit: text("commit"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("sotr-tools-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn parses_tool_versions() {
        assert_eq!(parse_version("cmake version 3.31.6\n\nCMake suite"), Some((3, 31)));
        assert_eq!(parse_version("Python 3.12.1"), Some((3, 12)));
        assert_eq!(parse_version("clang version 22.1.0"), Some((22, 1)));
        assert_eq!(parse_version("no digits"), None);
        assert!((3, 31) >= MIN_CMAKE && (3, 24) < MIN_CMAKE);
    }

    #[test]
    fn finds_programs_with_the_platform_suffix() {
        let dir = scratch("find");
        fs::write(dir.join("ninja"), "").unwrap();
        fs::write(dir.join("cmake.exe"), "").unwrap();
        let dirs = [dir.clone()];
        assert_eq!(find_program("ninja", Platform::Linux, &dirs), Some(dir.join("ninja")));
        assert_eq!(find_program("cmake", Platform::Windows, &dirs), Some(dir.join("cmake.exe")));
        assert_eq!(find_program("cmake", Platform::Linux, &dirs), None);
    }

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
    fn report_lists_the_platform_tools_and_links_the_missing_ones() {
        let repo = scratch("report");
        let linux = detect(Platform::Linux, &repo, "/no/such/cmake");
        let ids: Vec<_> = linux.tools.iter().map(|t| t.id).collect();
        assert_eq!(ids, ["python", "git", "clang", "cmake", "ninja"]);
        let cmake = linux.tools.iter().find(|t| t.id == "cmake").unwrap();
        assert!(!cmake.found && !cmake.url.is_empty());
        assert!(!linux.ready);
        let windows = detect(Platform::Windows, &repo, "");
        assert!(windows.tools.iter().any(|t| t.id == "msvc" && t.url.starts_with("https://")));
    }

    #[test]
    fn extra_folders_go_ahead_of_path() {
        assert!(path_with(&[]).is_none());
        let joined = path_with(&[PathBuf::from("/opt/llvm/bin")]).unwrap();
        assert_eq!(std::env::split_paths(&joined).next(), Some(PathBuf::from("/opt/llvm/bin")));
    }
}
