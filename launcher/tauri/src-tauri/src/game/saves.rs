//! Bringing saves into a Development build's own save folder. Development builds
//! (a standalone launcher's own commit) never share the normal save folders
//! (settings.rs `folders_for`), so on their first launch the page offers to copy
//! the matching normal folder's saves (base to base, update to update), or to
//! start fresh.

use super::*;
use std::{
    fs, io,
    path::{Path, PathBuf},
};

/// Left behind when the user chose to start fresh, so the offer is not repeated.
pub const STARTED_FRESH: &str = ".launcher-started-fresh";

/// What a save folder may hold that is not a save: the game's logs and caches,
/// rebuilt by whichever build runs (the Shadow renderer converts its own shaders).
fn regenerated(relative: &Path) -> bool {
    let name = relative.file_name().and_then(|n| n.to_str()).unwrap_or_default();
    name.ends_with(".log")
        || relative == Path::new("shader_cache")
        || relative == Path::new("cache").join("shaders")
}

/// The folder holds saves (or the start-fresh marker): anything besides logs and caches.
pub fn has_saves(dir: &Path) -> bool {
    fs::read_dir(dir).is_ok_and(|entries| {
        entries.flatten().any(|entry| {
            let name = PathBuf::from(entry.file_name());
            !regenerated(&name) && !(name == Path::new("cache") && !has_kept_files(&entry.path(), &name))
        })
    })
}

/// A `cache` folder counts only for what is kept from it (not its shaders).
fn has_kept_files(dir: &Path, relative: &Path) -> bool {
    fs::read_dir(dir).is_ok_and(|entries| entries.flatten().any(|e| !regenerated(&relative.join(e.file_name()))))
}

/// Folders of a first Development launch: (normal saves to copy from, the
/// Development build's own). None when there is nothing to offer: a normal
/// build, a Development folder that already has saves, or no normal saves.
pub fn copy_offer(build: &Build, settings: &Settings, defaults: &Defaults) -> Option<(PathBuf, PathBuf)> {
    if !build.development {
        return None;
    }
    let (_, own) = folders_for(build, settings, defaults);
    let normal = Build { development: false, ..build.clone() };
    let (_, from) = folders_for(&normal, settings, defaults);
    (from != own && !has_saves(&own) && has_saves(&from)).then_some((from, own))
}

/// Copies the saves in `from` into `to` (logs and shader caches left out). The
/// copy is made beside `to` first and moved in at the end, so an interrupted copy
/// leaves `to` without saves and the offer comes back. Refuses a `to` that
/// already has saves. Returns the number of files copied.
pub fn copy_saves(from: &Path, to: &Path) -> io::Result<u64> {
    if has_saves(to) {
        return Err(io::Error::new(io::ErrorKind::AlreadyExists, format!("{} already holds saves", to.display())));
    }
    let staging = to.with_extension("copying");
    let _ = fs::remove_dir_all(&staging);
    fs::create_dir_all(&staging)?;
    let copied = copy_tree(from, &staging, Path::new(""));
    let moved = copied.and_then(|count| {
        fs::create_dir_all(to)?;
        for entry in fs::read_dir(&staging)? {
            let entry = entry?;
            let target = to.join(entry.file_name());
            if target.is_dir() {
                // A cache folder the game made already: merge what was copied into it.
                copy_tree(&entry.path(), &target, Path::new(""))?;
            } else {
                fs::rename(entry.path(), target)?;
            }
        }
        Ok(count)
    });
    let _ = fs::remove_dir_all(&staging);
    moved
}

fn copy_tree(from: &Path, to: &Path, relative: &Path) -> io::Result<u64> {
    let mut count = 0;
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let rel = relative.join(entry.file_name());
        if regenerated(&rel) {
            continue;
        }
        let target = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            fs::create_dir_all(&target)?;
            count += copy_tree(&entry.path(), &target, &rel)?;
        } else {
            fs::copy(entry.path(), &target)?;
            count += 1;
        }
    }
    Ok(count)
}

/// Records that the Development folder starts without the normal saves.
pub fn start_fresh(to: &Path) -> io::Result<()> {
    fs::create_dir_all(to)?;
    fs::write(to.join(STARTED_FRESH), b"The launcher asked to copy saves here; the user chose to start fresh.\n")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::test_support::*;

    /// A normal save folder as the game leaves it: a profile, settings, logs, caches.
    fn normal_saves(dir: &Path) {
        let save = dir.join("B13EBABEBABEBABE/394F07D4/00000001/d3save");
        fs::create_dir_all(save.join("heroes")).unwrap();
        fs::write(save.join("account.dat"), "account").unwrap();
        fs::write(save.join("heroes/hero.dat"), "hero").unwrap();
        fs::write(dir.join("pc-window-mode.txt"), "1").unwrap();
        fs::write(dir.join("diablo3.log"), "log").unwrap();
        fs::write(dir.join("diablo3.3.log"), "log").unwrap();
        fs::create_dir_all(dir.join("cache/shaders/shareable")).unwrap();
        fs::write(dir.join("cache/shaders/shareable/x.xsh"), "shader").unwrap();
        fs::create_dir_all(dir.join("cache/title_cache")).unwrap();
        fs::write(dir.join("cache/title_cache/t"), "t").unwrap();
        fs::create_dir_all(dir.join("shader_cache")).unwrap();
        fs::write(dir.join("shader_cache/s"), "s").unwrap();
    }

    #[test]
    fn logs_and_caches_are_not_saves() {
        let dir = scratch("saves-empty");
        assert!(!has_saves(&dir.join("missing")));
        fs::write(dir.join("launcher-stdio.log"), "x").unwrap();
        fs::create_dir_all(dir.join("cache/shaders")).unwrap();
        fs::create_dir_all(dir.join("shader_cache")).unwrap();
        assert!(!has_saves(&dir), "a failed first launch leaves only logs and caches");
        fs::write(dir.join("pc-render-scale.txt"), "2").unwrap();
        assert!(has_saves(&dir));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn saves_are_copied_without_logs_or_shader_caches() {
        let dir = scratch("saves-copy");
        let (from, to) = (dir.join("normal"), dir.join("development"));
        normal_saves(&from);
        // The game already ran once and made a cache folder and a log.
        fs::create_dir_all(to.join("cache/shaders")).unwrap();
        fs::write(to.join("launcher-stdio.log"), "x").unwrap();
        assert_eq!(copy_saves(&from, &to).unwrap(), 4);
        assert_eq!(fs::read_to_string(to.join("B13EBABEBABEBABE/394F07D4/00000001/d3save/heroes/hero.dat")).unwrap(), "hero");
        assert!(to.join("pc-window-mode.txt").is_file());
        assert!(to.join("cache/title_cache/t").is_file());
        assert!(!to.join("diablo3.log").exists() && !to.join("diablo3.3.log").exists());
        assert!(!to.join("shader_cache").exists() && !to.join("cache/shaders/shareable").exists());
        assert!(!to.with_extension("copying").exists());
        assert_eq!(copy_saves(&from, &to).unwrap_err().kind(), io::ErrorKind::AlreadyExists, "never over existing saves");
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn the_offer_is_made_once_for_development_builds_with_normal_saves() {
        let dir = scratch("saves-offer");
        let defaults = default_folders(Platform::Linux, &dir, &dir);
        let settings = Settings::default();
        let mut development = build(true, true);
        development.development = true;
        assert_eq!(copy_offer(&development, &settings, &defaults), None, "no normal saves yet");
        normal_saves(&defaults.user_data_root_update);
        assert_eq!(
            copy_offer(&development, &settings, &defaults),
            Some((defaults.user_data_root_update.clone(), defaults.user_data_root_update_development.clone())),
            "update copies from update"
        );
        assert_eq!(copy_offer(&build(true, true), &settings, &defaults), None, "normal builds use the normal folder");
        start_fresh(&defaults.user_data_root_update_development).unwrap();
        assert_eq!(copy_offer(&development, &settings, &defaults), None, "not asked again after starting fresh");
        fs::remove_dir_all(dir).unwrap();
    }
}
