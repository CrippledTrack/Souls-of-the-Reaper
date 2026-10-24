//! Fetches the repository sources (scripts, patches, port, lock files) that a
//! launcher outside a source checkout needs to build the game. They are cloned
//! at the commit the launcher was built from into a per-user folder; without
//! git the matching GitHub archive is downloaded instead.

use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

/// Where the sources for `commit` live below `base`.
pub fn managed_root(base: &Path, commit: &str) -> PathBuf {
    base.join("sources").join(commit)
}

/// The folder can be written to (an install under Program Files or /usr cannot).
fn writable(dir: &Path) -> bool {
    let probe = dir.join(".sotr-write-test");
    let ok = fs::File::create(&probe).is_ok();
    let _ = fs::remove_file(probe);
    ok
}

/// Sources, the SDK and build output stay next to the launcher, where its game
/// folders are, so the whole install moves and deletes as one folder. A folder
/// that already holds these sources wins (including the per-user one earlier
/// launchers used), and a launcher in a read-only place uses the per-user folder.
pub fn managed_root_for(launcher_dir: &Path, data_dir: Option<&Path>, commit: &str) -> Option<PathBuf> {
    let beside = launcher_dir.join("build");
    let per_user = data_dir.map(|d| d.join("SoulsOfTheReaper"));
    let candidates: Vec<PathBuf> = std::iter::once(beside.clone()).chain(per_user.clone()).collect();
    let existing = candidates.iter().map(|b| managed_root(b, commit)).find(|r| is_complete(r));
    existing.or_else(|| {
        let base = if writable(launcher_dir) { beside } else { per_user? };
        Some(managed_root(&base, commit))
    })
}

/// The folder holds what scripts/build_client.py needs.
pub fn is_complete(root: &Path) -> bool {
    root.join("port/CMakeLists.txt").is_file() && root.join("scripts/build_client.py").is_file()
}

/// "https://github.com/o/r.git" or "git@github.com:o/r" -> "https://github.com/o/r".
pub fn github_url(remote: &str) -> Option<String> {
    let path = remote
        .trim()
        .strip_prefix("https://github.com/")
        .or_else(|| remote.trim().strip_prefix("git@github.com:"))?
        .trim_end_matches('/');
    let path = path.strip_suffix(".git").unwrap_or(path);
    (path.split('/').count() == 2 && !path.contains(char::is_whitespace))
        .then(|| format!("https://github.com/{path}"))
}

fn run(program: &Path, args: &[&str], dir: &Path, log: &dyn Fn(&str)) -> Result<(), String> {
    let mut command = Command::new(program);
    command.args(args).current_dir(dir).stdin(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    let output = command
        .output()
        .map_err(|e| format!("Unable to run {}: {e}", program.display()))?;
    for line in String::from_utf8_lossy(&output.stderr).lines() {
        log(line);
    }
    if output.status.success() {
        Ok(())
    } else {
        Err(format!("{} {} failed.", program.display(), args.first().unwrap_or(&"")))
    }
}

fn clone_with_git(git: &Path, url: &str, commit: &str, dest: &Path, log: &dyn Fn(&str)) -> Result<(), String> {
    run(git, &["init", "--quiet"], dest, log)?;
    run(git, &["remote", "add", "origin", url], dest, log)?;
    run(git, &["fetch", "--depth", "1", "origin", commit], dest, log)?;
    run(git, &["checkout", "--quiet", "FETCH_HEAD"], dest, log)?;
    let head = Command::new(git)
        .args(["rev-parse", "HEAD"])
        .current_dir(dest)
        .output()
        .map_err(|e| e.to_string())?;
    let head = String::from_utf8_lossy(&head.stdout);
    if head.trim() != commit {
        return Err(format!("Fetched revision {} instead of {commit}.", head.trim()));
    }
    Ok(())
}

/// Downloads GitHub's archive of `commit` with curl and unpacks it with tar
/// (both ship with Windows 10 and later), dropping the top-level folder.
fn download_archive(url: &str, commit: &str, dest: &Path, log: &dyn Fn(&str)) -> Result<(), String> {
    let archive = dest.join("sources.tar.gz");
    let archive_name = archive.to_string_lossy().into_owned();
    run(
        Path::new("curl"),
        &["--fail", "--location", "--silent", "--show-error", "--output", &archive_name, &format!("{url}/archive/{commit}.tar.gz")],
        dest,
        log,
    )?;
    run(Path::new("tar"), &["-xzf", &archive_name, "--strip-components=1"], dest, log)?;
    let _ = fs::remove_file(archive);
    Ok(())
}

/// Makes sure the sources for `commit` are on disk and returns their folder.
/// `git` is the git executable, if one was found. Progress goes to `log`.
pub fn ensure(
    root: &Path,
    remote: &str,
    commit: &str,
    git: Option<&Path>,
    log: &dyn Fn(&str),
) -> Result<PathBuf, String> {
    if is_complete(root) {
        return Ok(root.to_path_buf());
    }
    let url = github_url(remote).ok_or_else(|| format!("Cannot fetch sources from {remote:?}: not a GitHub repository."))?;
    let partial = root.with_extension("partial");
    let _ = fs::remove_dir_all(&partial);
    fs::create_dir_all(&partial).map_err(|e| format!("Unable to create {}: {e}", partial.display()))?;
    let fetched = match git {
        Some(git) => {
            log(&format!("Cloning {url} at {commit}"));
            clone_with_git(git, &url, commit, &partial, log)
        }
        None => {
            log(&format!("git was not found; downloading the source archive for {commit}"));
            download_archive(&url, commit, &partial, log)
        }
    };
    fetched
        .and_then(|()| {
            is_complete(&partial)
                .then_some(())
                .ok_or_else(|| "The fetched sources are missing the build scripts.".to_string())
        })
        .and_then(|()| {
            let _ = fs::remove_dir_all(root);
            fs::rename(&partial, root).map_err(|e| format!("Unable to move sources into place: {e}"))
        })
        .inspect_err(|_| {
            let _ = fs::remove_dir_all(&partial);
        })?;
    Ok(root.to_path_buf())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn github_remotes_are_normalised() {
        let want = Some("https://github.com/CrippledTrack/Souls-of-the-Reaper".to_string());
        assert_eq!(github_url("https://github.com/CrippledTrack/Souls-of-the-Reaper.git"), want);
        assert_eq!(github_url("git@github.com:CrippledTrack/Souls-of-the-Reaper.git\n"), want);
        assert_eq!(github_url("https://github.com/CrippledTrack/Souls-of-the-Reaper/"), want);
        assert_eq!(github_url("https://example.test/a/b.git"), None);
        assert_eq!(github_url("https://github.com/only-owner"), None);
    }

    #[test]
    fn sources_are_keyed_by_commit() {
        let root = managed_root(Path::new("/data"), "abc");
        assert!(root.ends_with("sources/abc"));
    }

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("sotr-root-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn make_complete(root: &Path) {
        fs::create_dir_all(root.join("port")).unwrap();
        fs::create_dir_all(root.join("scripts")).unwrap();
        fs::write(root.join("port/CMakeLists.txt"), "").unwrap();
        fs::write(root.join("scripts/build_client.py"), "").unwrap();
    }

    #[test]
    fn sources_default_to_beside_the_launcher() {
        let (launcher, data) = (scratch("launcher"), scratch("data"));
        let root = managed_root_for(&launcher, Some(&data), "abc").unwrap();
        assert_eq!(root, launcher.join("build/sources/abc"));
        assert!(!launcher.join(".sotr-write-test").exists());
    }

    #[test]
    fn fetched_sources_in_the_per_user_folder_are_still_used() {
        let (launcher, data) = (scratch("launcher2"), scratch("data2"));
        let old = managed_root(&data.join("SoulsOfTheReaper"), "abc");
        make_complete(&old);
        assert_eq!(managed_root_for(&launcher, Some(&data), "abc").unwrap(), old);
        make_complete(&managed_root(&launcher.join("build"), "abc"));
        assert_eq!(managed_root_for(&launcher, Some(&data), "abc").unwrap(), launcher.join("build/sources/abc"));
    }

    #[test]
    fn an_unwritable_launcher_folder_falls_back_to_the_per_user_one() {
        let data = scratch("data3");
        let root = managed_root_for(Path::new("/nonexistent-sotr-launcher-dir"), Some(&data), "abc").unwrap();
        assert_eq!(root, data.join("SoulsOfTheReaper/sources/abc"));
        assert_eq!(managed_root_for(Path::new("/nonexistent-sotr-launcher-dir"), None, "abc"), None);
    }

    #[test]
    fn complete_needs_the_build_scripts() {
        let dir = std::env::temp_dir().join(format!("sotr-sources-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("port")).unwrap();
        fs::create_dir_all(dir.join("scripts")).unwrap();
        assert!(!is_complete(&dir));
        fs::write(dir.join("port/CMakeLists.txt"), "").unwrap();
        assert!(!is_complete(&dir));
        fs::write(dir.join("scripts/build_client.py"), "").unwrap();
        assert!(is_complete(&dir));
        let _ = fs::remove_dir_all(&dir);
    }

    /// Fetches a real pushed commit (SOTR_TEST_COMMIT, from SOTR_TEST_REMOTE).
    /// Run with `cargo test -- --ignored`; needs network access and git, curl
    /// and tar on PATH.
    #[test]
    #[ignore]
    fn fetches_a_pushed_commit_with_git_and_without() {
        let commit = std::env::var("SOTR_TEST_COMMIT").expect("set SOTR_TEST_COMMIT to a pushed commit");
        let remote = std::env::var("SOTR_TEST_REMOTE")
            .unwrap_or_else(|_| "https://github.com/CrippledTrack/Souls-of-the-Reaper.git".into());
        let base = std::env::temp_dir().join(format!("sotr-fetch-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let lines = std::cell::RefCell::new(Vec::new());
        let log = |l: &str| lines.borrow_mut().push(l.to_string());
        for (name, git) in [("git", Some(Path::new("git"))), ("archive", None)] {
            let root = managed_root(&base.join(name), &commit);
            let got = ensure(&root, &remote, &commit, git, &log)
                .unwrap_or_else(|e| panic!("{name}: {e}\n{:?}", lines.borrow()));
            assert!(is_complete(&got), "{name}");
            // A second call reuses the folder.
            let before = lines.borrow().len();
            ensure(&root, &remote, &commit, git, &log).unwrap();
            assert_eq!(lines.borrow().len(), before, "{name} refetched");
            assert!(!root.with_extension("partial").exists());
        }
        assert!(managed_root(&base.join("git"), &commit).join(".git").exists());
        let _ = fs::remove_dir_all(&base);
    }
}
