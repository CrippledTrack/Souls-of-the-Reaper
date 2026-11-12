//! Finds the build tools (Python, Git, Clang, CMake, Ninja, Visual Studio) and
//! warns about what may fail later.

use super::*;
use crate::message::{msg, Message};
use crate::game::Platform;
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

pub(crate) const MIN_CMAKE: (u32, u32) = (3, 25);

/// Oldest Clang the SDK and port build with (docs/building-the-game.md).
pub(crate) const MIN_CLANG_LINUX: u32 = 18;

pub(crate) const MIN_CLANG_WINDOWS: u32 = 22;

/// Apple numbers its Clang by Xcode release (Xcode 16's Apple clang 16 is LLVM 17
/// based); build_macos.py always compiles with /usr/bin/clang.
pub(crate) const MIN_CLANG_MACOS: u32 = 16;

/// Where Homebrew (Apple Silicon, then Intel) and MacPorts install. A launcher
/// opened from Finder gets a PATH without them.
pub(crate) const MACOS_TOOL_DIRS: [&str; 3] = ["/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"];

/// Folders the MoltenVK driver file (MoltenVK_icd.json) is installed to by
/// Homebrew and the LunarG Vulkan SDK.
pub(crate) const MACOS_ICD_DIRS: [&str; 4] = [
    "/opt/homebrew/etc/vulkan/icd.d",
    "/opt/homebrew/share/vulkan/icd.d",
    "/usr/local/etc/vulkan/icd.d",
    "/usr/local/share/vulkan/icd.d",
];

/// Free space a first build wants: SDK, two variants and codegen, with headroom
/// (scripts/build_client.py computes the exact need and stops if it is short).
pub(crate) const WARN_FREE_BYTES: u64 = 15 << 30;

pub(crate) const VS_COMPONENT: &str = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";

pub(crate) fn exe(name: &str, platform: Platform) -> String {
    match platform {
        Platform::Windows => format!("{name}.exe"),
        Platform::Linux | Platform::MacOs => name.to_string(),
    }
}

/// On a Mac without the command line tools, /usr/bin/git, clang and python3 are
/// stubs that only offer to install them (and open a dialog when run).
pub(crate) fn is_apple_stub_dir(dir: &Path) -> bool {
    dir == Path::new("/usr/bin")
}

/// `xcode-select -p` succeeds once the command line tools (or Xcode) are installed.
pub(crate) fn command_line_tools_installed() -> bool {
    output_of(Path::new("/usr/bin/xcode-select"), &["-p"]).is_some_and(|p| Path::new(p.trim()).is_dir())
}

/// First `name` found in `dirs`.
pub fn find_program(name: &str, platform: Platform, dirs: &[PathBuf]) -> Option<PathBuf> {
    let file = exe(name, platform);
    dirs.iter().map(|d| d.join(&file)).find(|p| p.is_file())
}

/// Stdout of `program args`, or None when it cannot run or fails (this also
/// rejects the Microsoft Store `python.exe` stub on Windows). Python and the
/// `py` launcher are run through here, so the UTF-8 settings go on every call.
pub(crate) fn output_of(program: &Path, args: &[&str]) -> Option<String> {
    let mut command = Command::new(program);
    command
        .args(args)
        .envs(PYTHON_UTF8)
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

/// Python 3: the first interpreter in `dirs` that runs and reports 3.x (the
/// Microsoft Store `python.exe` stub on Windows does not), else on Windows the
/// interpreter the `py` launcher picks, which needs no PATH entry.
pub(crate) fn find_python(platform: Platform, dirs: &[PathBuf]) -> Option<PathBuf> {
    let is_python3 = |p: &Path| {
        output_of(p, &["--version"])
            .and_then(|v| parse_version(&v))
            .is_some_and(|v| v.0 >= 3)
    };
    let name = exe(if platform == Platform::Windows { "python" } else { "python3" }, platform);
    let on_path = dirs.iter().map(|d| d.join(&name)).find(|p| p.is_file() && is_python3(p));
    on_path.or_else(|| {
        let py = find_program("py", platform, dirs).filter(|_| platform == Platform::Windows)?;
        let found = output_of(&py, &["-3", "-c", "import sys; print(sys.executable)"])?;
        Some(PathBuf::from(found.trim())).filter(|p| p.is_file() && is_python3(p))
    })
}

/// Visual Studio install that has the C++ x64 tools, via vswhere.
pub(crate) fn visual_studio() -> Option<PathBuf> {
    let files = std::env::var_os("ProgramFiles(x86)").map(PathBuf::from)?;
    let vswhere = files.join("Microsoft Visual Studio/Installer/vswhere.exe");
    let found = output_of(
        &vswhere,
        &["-latest", "-products", "*", "-requires", VS_COMPONENT, "-property", "installationPath"],
    )?;
    let path = PathBuf::from(found.trim());
    (!found.trim().is_empty() && path.is_dir()).then_some(path)
}

pub(crate) fn tool(id: &'static str, label: &str, path: Option<&Path>, url: &'static str, note: Message) -> Tool {
    Tool {
        id,
        label: label.into(),
        found: path.is_some(),
        path: path.map(|p| p.display().to_string()).unwrap_or_default(),
        url,
        note,
        winget: "",
    }
}

/// Folders in the user and machine PATH as saved in the registry. Installers
/// change those, not the PATH this process started with, so tools installed
/// while the launcher is open are only found this way.
#[cfg(windows)]
pub(crate) fn registry_path() -> Vec<PathBuf> {
    let keys = [r"HKCU\Environment", r"HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment"];
    keys.iter()
        .filter_map(|key| output_of(Path::new("reg"), &["query", key, "/v", "Path"]))
        .filter_map(|text| parse_reg_value(&text))
        .flat_map(|value| std::env::split_paths(&expand_env(&value)).collect::<Vec<_>>())
        .filter(|d| !d.as_os_str().is_empty())
        .collect()
}

#[cfg(not(windows))]
pub(crate) fn registry_path() -> Vec<PathBuf> {
    Vec::new()
}

/// The data of a `reg query /v` line: "    Path    REG_EXPAND_SZ    C:\a;%X%".
/// Only the Windows registry lookup uses it; the tests run on every host.
#[cfg(any(windows, test))]
pub(crate) fn parse_reg_value(text: &str) -> Option<String> {
    text.lines().find_map(|line| {
        let (_, rest) = line.split_once("REG_EXPAND_SZ").or_else(|| line.split_once("REG_SZ"))?;
        Some(rest.trim().to_string())
    })
}

/// Replaces %NAME% with the environment variable; unknown names stay as written.
#[cfg(any(windows, test))]
pub(crate) fn expand_env(text: &str) -> String {
    let mut out = String::new();
    let mut rest = text;
    while let Some(start) = rest.find('%') {
        out.push_str(&rest[..start]);
        let after = &rest[start + 1..];
        let Some(end) = after.find('%') else {
            out.push_str(&rest[start..]);
            return out;
        };
        let name = &after[..end];
        match std::env::var(name).ok().filter(|_| !name.is_empty()) {
            Some(value) => out.push_str(&value),
            None => out.push_str(&format!("%{name}%")),
        }
        rest = &after[end + 1..];
    }
    out.push_str(rest);
    out
}

pub(crate) fn package_hint(packages: &str) -> Message {
    msg!("note.packageHint", packages = packages)
}

/// Status of every tool a build needs. `cmake_choice` is the Build panel's
/// CMake field (blank = look it up).
pub fn detect(platform: Platform, repo: &Path, cmake_choice: &str) -> ToolReport {
    // The PATH the build script inherits, then the registry's (tools installed
    // since the launcher started).
    let inherited: Vec<PathBuf> = std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).collect())
        .unwrap_or_default();
    let mut dirs = inherited.clone();
    if platform == Platform::Windows {
        for dir in registry_path() {
            if !dirs.contains(&dir) {
                dirs.push(dir);
            }
        }
    }
    let apple_tools = platform == Platform::MacOs && command_line_tools_installed();
    if platform == Platform::MacOs && !apple_tools {
        dirs.retain(|d| !is_apple_stub_dir(d));
    }
    let mut extra = Vec::new();
    if platform == Platform::MacOs {
        extra.extend(MACOS_TOOL_DIRS.map(PathBuf::from));
    }
    let studio = (platform == Platform::Windows).then(visual_studio).flatten();
    if platform == Platform::Windows {
        let program_files = std::env::var_os("ProgramFiles")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(r"C:\Program Files"));
        extra.push(program_files.join("LLVM/bin"));
        // Where the winget packages install when they leave PATH alone.
        extra.push(program_files.join("CMake/bin"));
        extra.push(program_files.join("Git/cmd"));
        if let Some(local) = std::env::var_os("LOCALAPPDATA") {
            extra.push(PathBuf::from(local).join("Microsoft/WinGet/Links"));
        }
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

    let python = find_python(platform, &dirs).or_else(|| find_python(platform, &extra));
    let git = lookup("git");
    let min_clang = match platform {
        Platform::Windows => MIN_CLANG_WINDOWS,
        Platform::Linux => MIN_CLANG_LINUX,
        Platform::MacOs => MIN_CLANG_MACOS,
    };
    // clang-cl (part of the LLVM installer) builds the shader converter on Windows.
    let clang_cl = platform != Platform::Windows || lookup("clang-cl").is_some();
    let clang_found = if platform == Platform::MacOs {
        // build_macos.py compiles with Apple's clang, whatever else is on PATH.
        Some(PathBuf::from("/usr/bin/clang++")).filter(|p| apple_tools && p.is_file())
    } else {
        lookup("clang").zip(lookup("clang++")).map(|(_, plus)| plus).filter(|_| clang_cl)
    };
    let clang_version = clang_found
        .as_deref()
        .and_then(|p| output_of(p, &["--version"]))
        .and_then(|v| parse_version(&v));
    // A clang whose version cannot be read is let through; the build reports it.
    let clang = clang_found.filter(|_| clang_version.is_none_or(|(major, _)| major >= min_clang));
    let cmake = match cmake_choice.trim() {
        "" => lookup("cmake"),
        chosen => Some(PathBuf::from(chosen)).filter(|p| p.is_file()),
    };
    let cmake_ok = cmake
        .as_deref()
        .filter(|p| output_of(p, &["--version"]).and_then(|v| parse_version(&v)).is_some_and(|v| v >= MIN_CMAKE));
    let ninja = lookup("ninja");

    let (linux, mac) = (platform == Platform::Linux, platform == Platform::MacOs);
    let mut tools = vec![
        tool(
            "python",
            "Python 3",
            python.as_deref(),
            "https://www.python.org/downloads/",
            match platform {
                Platform::Windows => msg!("note.python.windows"),
                Platform::Linux => msg!("note.python"),
                Platform::MacOs => msg!("note.python.macos"),
            },
        ),
        tool(
            "git",
            "Git",
            git.as_deref(),
            "https://git-scm.com/downloads",
            if mac { msg!("note.git.macos") } else { msg!("note.git") },
        ),
    ];
    if platform == Platform::Windows {
        tools.push(tool(
            "msvc",
            "Visual Studio 2022 C++ tools",
            studio.as_deref(),
            "https://visualstudio.microsoft.com/downloads/",
            msg!("note.msvc"),
        ));
    }
    tools.push(tool(
        "clang",
        match platform {
            Platform::Windows => "LLVM Clang 22+",
            Platform::Linux => "Clang 18+",
            Platform::MacOs => "Xcode command line tools (Apple clang 16+)",
        },
        clang.as_deref(),
        if platform == Platform::Windows { "https://github.com/llvm/llvm-project/releases/latest" } else { "" },
        match (clang_version, clang.is_some()) {
            (Some((major, _)), false) => msg!("note.clang.old", found = major, min = min_clang),
            _ if linux => package_hint("apt install clang, pacman -S clang"),
            _ if mac => msg!("note.clang.macos"),
            _ => msg!("note.clang.windows"),
        },
    ));
    let cmake_note = match (&cmake, &cmake_ok) {
        (Some(_), None) => msg!("note.cmake.old"),
        _ if linux => package_hint("apt install cmake, pacman -S cmake"),
        _ if mac => package_hint("brew install cmake"),
        _ => msg!("note.visualStudioCmake"),
    };
    tools.push(tool(
        "cmake",
        "CMake 3.25+",
        cmake_ok,
        "https://cmake.org/download/",
        cmake_note,
    ));
    tools.push(tool(
        "ninja",
        "Ninja",
        ninja.as_deref(),
        "https://github.com/ninja-build/ninja/releases/latest",
        match platform {
            Platform::Linux => package_hint("apt install ninja-build, pacman -S ninja"),
            Platform::MacOs => package_hint("brew install ninja"),
            Platform::Windows => msg!("note.visualStudioCmake"),
        },
    ));

    if platform == Platform::Windows {
        for t in &mut tools {
            t.winget = winget_package(t.id);
        }
    }

    // Only folders the inherited PATH lacks need adding for the build script,
    // which runs git and the compilers by name.
    let mut path_dirs = Vec::new();
    for found in [&python, &git, &clang, &cmake, &ninja].into_iter().flatten() {
        if let Some(dir) = found.parent() {
            if !inherited.iter().any(|d| d == dir) && !path_dirs.iter().any(|d: &PathBuf| d == dir) {
                path_dirs.push(dir.to_path_buf());
            }
        }
    }

    let mut warnings = Vec::new();
    if linux {
        warnings.extend(linux_warnings(Path::new("/usr/share/vulkan/icd.d"), Path::new("/etc/vulkan/icd.d"), &dirs));
    }
    if mac {
        warnings.extend(macos_warnings(&MACOS_ICD_DIRS.map(PathBuf::from)));
    }
    if let Some(free) = free_bytes(repo).filter(|free| *free < WARN_FREE_BYTES) {
        warnings.push(msg!("warn.diskSpace", free = format!("{:.1}", free as f64 / (1u64 << 30) as f64)));
    }

    let (sdk, converter) = python
        .as_deref()
        .and_then(|p| sdk_status_from_script(p, repo))
        .unwrap_or_else(|| (sdk_status(platform, repo), None));
    let ready = tools.iter().all(|t| t.found);
    ToolReport {
        warnings,
        ready,
        winget: platform == Platform::Windows && tools.iter().any(|t| !t.found) && winget_available(),
        tools,
        sdk,
        converter,
        sources: None,
        path_dirs,
    }
}

/// Linux packages the tool list cannot see. SDL builds for X11 or Wayland and
/// needs their development files; playing needs a Vulkan driver (an ICD file).
pub(crate) fn linux_warnings(icd_dir: &Path, etc_icd_dir: &Path, path: &[PathBuf]) -> Vec<Message> {
    let mut warnings = Vec::new();
    let has_icd = |dir: &Path| fs::read_dir(dir).is_ok_and(|mut d| d.next().is_some());
    if !has_icd(icd_dir) && !has_icd(etc_icd_dir) {
        warnings.push(msg!("warn.vulkanDriver"));
    }
    let pkg_config = find_program("pkg-config", Platform::Linux, path);
    let exists = |module: &str| {
        pkg_config.as_deref().is_some_and(|p| {
            Command::new(p)
                .args(["--exists", module])
                .stdin(Stdio::null())
                .stderr(Stdio::null())
                .status()
                .is_ok_and(|s| s.success())
        })
    };
    if !exists("x11") && !exists("wayland-client") {
        warnings.push(msg!("warn.windowingPackages"));
    }
    warnings
}

/// The game draws through MoltenVK; without its driver file the Vulkan loader
/// finds no device.
pub(crate) fn macos_warnings(icd_dirs: &[PathBuf]) -> Vec<Message> {
    let has_moltenvk = icd_dirs.iter().any(|dir| {
        fs::read_dir(dir).is_ok_and(|entries| {
            entries.flatten().any(|e| e.file_name().to_string_lossy().to_ascii_lowercase().contains("moltenvk"))
        })
    });
    if has_moltenvk { vec![] } else { vec![msg!("warn.moltenvk")] }
}

/// Free bytes on the volume holding `path` (or its nearest existing parent).
#[cfg(unix)]
pub(crate) fn free_bytes(path: &Path) -> Option<u64> {
    use std::os::unix::ffi::OsStrExt;
    let existing = path.ancestors().find(|p| p.exists())?;
    let c_path = std::ffi::CString::new(existing.as_os_str().as_bytes()).ok()?;
    let mut stat: libc::statvfs = unsafe { std::mem::zeroed() };
    // SAFETY: c_path is a valid NUL-terminated string and stat a valid out-pointer.
    (unsafe { libc::statvfs(c_path.as_ptr(), &mut stat) } == 0)
        .then(|| stat.f_bavail as u64 * stat.f_frsize as u64)
}

#[cfg(windows)]
pub(crate) fn free_bytes(path: &Path) -> Option<u64> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetDiskFreeSpaceExW(dir: *const u16, free_to_caller: *mut u64, total: *mut u64, total_free: *mut u64) -> i32;
    }
    let existing = path.ancestors().find(|p| p.exists())?;
    let wide: Vec<u16> = existing.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut free = 0u64;
    // SAFETY: wide is NUL-terminated and free a valid out-pointer; the others may be null.
    (unsafe { GetDiskFreeSpaceExW(wide.as_ptr(), &mut free, std::ptr::null_mut(), std::ptr::null_mut()) } != 0)
        .then_some(free)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[allow(unused_imports)]
    use crate::tools::test_support::*;

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
    fn free_space_is_read_for_missing_folders_too() {
        let dir = scratch("free");
        assert!(free_bytes(&dir.join("not/created/yet")).is_some_and(|b| b > 0));
    }

    #[test]
    fn linux_warnings_flag_a_missing_vulkan_driver() {
        let dir = scratch("icd");
        let none = linux_warnings(&dir.join("a"), &dir.join("b"), &[]);
        assert!(none.iter().any(|w| w.key == "warn.vulkanDriver"));
        fs::write(dir.join("radeon.json"), "{}").unwrap();
        let some = linux_warnings(&dir, &dir.join("b"), &[]);
        assert!(!some.iter().any(|w| w.key == "warn.vulkanDriver"));
        // Without pkg-config the windowing libraries cannot be confirmed.
        assert!(some.iter().any(|w| w.key == "warn.windowingPackages"));
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
    fn macos_lists_the_apple_toolchain_and_needs_moltenvk() {
        let repo = scratch("report-macos");
        let mac = detect(Platform::MacOs, &repo, "/no/such/cmake");
        let ids: Vec<_> = mac.tools.iter().map(|t| t.id).collect();
        assert_eq!(ids, ["python", "git", "clang", "cmake", "ninja"]);
        assert!(mac.tools.iter().all(|t| t.winget.is_empty()) && !mac.winget);
        assert!(!mac.warnings.iter().any(|w| w.key == "warn.vulkanDriver" || w.key == "warn.windowingPackages"));
        let dir = scratch("icd-macos");
        assert_eq!(macos_warnings(std::slice::from_ref(&dir))[0].key, "warn.moltenvk");
        fs::write(dir.join("MoltenVK_icd.json"), "{}").unwrap();
        assert!(macos_warnings(&[dir.join("missing"), dir]).is_empty());
    }

    #[test]
    fn only_usr_bin_holds_the_apple_install_stubs() {
        assert!(is_apple_stub_dir(Path::new("/usr/bin")));
        assert!(!is_apple_stub_dir(Path::new("/opt/homebrew/bin")));
        assert!(!is_apple_stub_dir(Path::new("/usr/local/bin")));
    }

    #[test]
    fn registry_path_values_are_parsed_and_expanded() {
        let text = "\r\nHKEY_CURRENT_USER\\Environment\r\n    Path    REG_EXPAND_SZ    %SOTR_TEST_HOME%\\bin;C:\\x\r\n\r\n";
        let value = parse_reg_value(text).unwrap();
        assert_eq!(value, r"%SOTR_TEST_HOME%\bin;C:\x");
        std::env::set_var("SOTR_TEST_HOME", r"D:\home");
        assert_eq!(expand_env(&value), r"D:\home\bin;C:\x");
        assert_eq!(expand_env("%SOTR_NOT_SET_ANYWHERE%;50%"), "%SOTR_NOT_SET_ANYWHERE%;50%");
        assert_eq!(parse_reg_value("ERROR: not found"), None);
    }
}
