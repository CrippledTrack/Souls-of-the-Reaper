//! Installing missing tools with winget (Windows).

use super::*;
use std::path::Path;

/// Visual Studio Build Tools with the C++ workload; --includeRecommended adds the
/// Windows SDK. --passive shows the Visual Studio installer's progress window.
pub(crate) const VS_BUILD_TOOLS_OVERRIDE: &str =
    "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended";

/// The Python installer leaves PATH alone unless asked; per-user needs no admin.
pub(crate) const PYTHON_OVERRIDE: &str = "/quiet InstallAllUsers=0 PrependPath=1 Include_launcher=1";

/// CMake 4 rejects projects that ask for a minimum below 3.5, which older
/// third-party code in the SDK may do; the builds so far used 3.31.
pub(crate) const CMAKE_VERSION: &str = "3.31.8";

/// winget package for each build tool.
pub fn winget_package(id: &str) -> &'static str {
    match id {
        "python" => "Python.Python.3.13",
        "git" => "Git.Git",
        "msvc" => "Microsoft.VisualStudio.2022.BuildTools",
        "clang" => "LLVM.LLVM",
        "cmake" => "Kitware.CMake",
        "ninja" => "Ninja-build.Ninja",
        _ => "",
    }
}

/// Arguments for `winget` to install `package` without questions. Only used
/// after the user pressed the Build panel's install button, which says that it
/// accepts the packages' license agreements.
pub fn winget_arguments(package: &str) -> Vec<String> {
    let mut args: Vec<String> = [
        "install",
        "--id",
        package,
        "--exact",
        "--source",
        "winget",
        "--silent",
        "--accept-package-agreements",
        "--accept-source-agreements",
        "--disable-interactivity",
    ]
    .map(String::from)
    .into();
    let installer = match package {
        "Microsoft.VisualStudio.2022.BuildTools" => Some(VS_BUILD_TOOLS_OVERRIDE),
        "Python.Python.3.13" => Some(PYTHON_OVERRIDE),
        _ => None,
    };
    if let Some(value) = installer {
        args.extend(["--override".into(), value.into()]);
    }
    if package == "Kitware.CMake" {
        args.extend(["--version".into(), CMAKE_VERSION.into()]);
    }
    args
}

/// winget is installed and runs (it is an app alias, so a file check is not enough).
pub(crate) fn winget_available() -> bool {
    output_of(Path::new("winget"), &["--version"]).is_some()
}

/// Missing tools in the order to install them: Visual Studio last, since it is
/// by far the largest.
pub fn install_order(report: &ToolReport) -> Vec<&Tool> {
    let mut missing: Vec<&Tool> = report.tools.iter().filter(|t| !t.found && !t.winget.is_empty()).collect();
    missing.sort_by_key(|t| t.id == "msvc");
    missing
}

/// One line of winget output for the log: progress redraws with carriage
/// returns, so only the last state of a line is kept, and spinner frames and
/// progress bars are dropped.
pub fn clean_winget_line(line: &str) -> Option<String> {
    let last = line.rsplit('\r').map(str::trim).find(|s| !s.is_empty())?;
    let noise = |c: char| c.is_whitespace() || "-\\|/\u{2588}\u{2592}".contains(c);
    // Bars end in a size or percentage, e.g. "███▒▒▒  12.0 MB / 40.0 MB".
    let bar = last.starts_with(['\u{2588}', '\u{2592}']);
    (last.chars().any(|c| !noise(c)) && !bar).then(|| last.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::Platform;
    #[allow(unused_imports)]
    use crate::tools::test_support::*;

    #[test]
    fn winget_installs_quietly_with_installer_overrides() {
        let git = winget_arguments("Git.Git");
        assert_eq!(&git[..4], ["install", "--id", "Git.Git", "--exact"]);
        assert!(git.iter().any(|a| a == "--accept-package-agreements"));
        assert!(!git.iter().any(|a| a == "--override"));
        let vs = winget_arguments(winget_package("msvc"));
        let value = &vs[vs.iter().position(|a| a == "--override").unwrap() + 1];
        assert!(value.contains("Microsoft.VisualStudio.Workload.VCTools") && value.contains("--includeRecommended"));
        let python = winget_arguments(winget_package("python"));
        assert!(python.last().unwrap().contains("PrependPath=1"));
        assert!(winget_arguments(winget_package("cmake")).ends_with(&["--version".into(), "3.31.8".into()]));
    }

    #[test]
    fn missing_tools_install_with_visual_studio_last() {
        let repo = scratch("order");
        let mut report = detect(Platform::Windows, &repo, "/no/such/cmake");
        for t in &mut report.tools {
            t.found = !matches!(t.id, "msvc" | "cmake" | "git");
        }
        let ids: Vec<_> = install_order(&report).iter().map(|t| t.id).collect();
        assert_eq!(ids, ["git", "cmake", "msvc"]);
        assert!(detect(Platform::Linux, &repo, "").tools.iter().all(|t| t.winget.is_empty()));
    }

    #[test]
    fn winget_progress_is_reduced_to_readable_lines() {
        let found = "Found Git [Git.Git] Version 2.51.0";
        assert_eq!(clean_winget_line(found).as_deref(), Some(found));
        assert_eq!(clean_winget_line("  -\r  \\\r  |\r  /"), None);
        assert_eq!(clean_winget_line("\u{2588}\u{2588}\u{2592}\u{2592}  1.00 MB / 64.0 MB"), None);
        assert_eq!(clean_winget_line("  -\r  Successfully installed").as_deref(), Some("Successfully installed"));
        assert_eq!(clean_winget_line(""), None);
    }
}
