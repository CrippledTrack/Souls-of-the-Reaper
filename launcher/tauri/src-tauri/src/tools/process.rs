//! Running the build script: its environment and stopping its process tree.

use std::{
    ffi::OsString,
    path::PathBuf,
};

/// Set on every Python child: on Windows it otherwise writes piped output in
/// the ANSI code page, and the launcher reads UTF-8 (paths with accents).
pub const PYTHON_UTF8: [(&str, &str); 2] = [("PYTHONUTF8", "1"), ("PYTHONIOENCODING", "utf-8")];

/// Stops `pid` and the processes it started (CMake and the compilers under the
/// build script, git's transport helpers under git): its process group on Unix,
/// where the child must lead one (`process_group(0)`), its process tree on Windows.
pub fn kill_tree(pid: u32) {
    #[cfg(unix)]
    unsafe {
        libc::kill(-(pid as i32), libc::SIGTERM);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        let _ = Command::new("taskkill")
            .args(["/PID", &pid.to_string(), "/T", "/F"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(0x0800_0000) // CREATE_NO_WINDOW
            .status();
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

#[cfg(test)]
mod tests {
    use super::*;
    #[allow(unused_imports)]
    use crate::tools::test_support::*;

    #[test]
    fn extra_folders_go_ahead_of_path() {
        assert!(path_with(&[]).is_none());
        let joined = path_with(&[PathBuf::from("/opt/llvm/bin")]).unwrap();
        assert_eq!(std::env::split_paths(&joined).next(), Some(PathBuf::from("/opt/llvm/bin")));
    }
}
