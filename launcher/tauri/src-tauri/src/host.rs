//! State the launcher keeps for its commands, what it injects into the page,
//! and launcher.json.

use crate::build_info;
use crate::game::{self, Build, Defaults, Platform, Settings};
use crate::job::JobSlot;
use crate::message::{msg, Message};
use serde::Serialize;
use std::{
    fs,
    path::{Path, PathBuf},
    sync::Mutex,
};

/// Folder defaults resolved at startup; builds are found again after the
/// Build panel finishes.
pub struct Host {
    pub(crate) platform: Platform,
    pub(crate) builds: Mutex<Vec<Build>>,
    pub(crate) defaults: Defaults,
    pub(crate) settings_file: PathBuf,
    pub(crate) launcher_dir: PathBuf,
    pub(crate) search: Vec<PathBuf>,
    /// Source checkout, needed to build the game.
    pub(crate) repo: Option<PathBuf>,
    /// The sources a launcher outside a checkout fetches and builds in.
    pub(crate) sources: Option<Sources>,
    /// The build or tool install that is running, if any.
    pub(crate) job: JobSlot,
}

/// One fetched source tree: where it lives and the commit it holds.
#[derive(Clone, Debug, PartialEq)]
pub struct Tree {
    pub root: PathBuf,
    pub commit: String,
}

/// A standalone launcher's source trees: the stable release (launcher/stable.json,
/// or the release a release launcher was built for), and Development, the commit the
/// launcher itself was built from. Development is None when that commit cannot be
/// fetched (a launcher built from modified or unknown sources). It is tied to the
/// launcher's own commit because an older launcher cannot know how to build newer sources.
#[derive(Clone, Debug, PartialEq)]
pub struct Sources {
    pub stable: Tree,
    pub stable_tag: String,
    pub development: Option<Tree>,
}

/// Which sources a standalone launcher builds (the Build panel's Version choice).
pub const STABLE: &str = "stable";
pub const DEVELOPMENT: &str = "development";

/// Where the build scripts run: `commit` is None for a source checkout, which
/// builds as it is whatever the channel.
#[derive(Clone, Debug, PartialEq)]
pub struct BuildRoot {
    pub root: PathBuf,
    pub commit: Option<String>,
    /// The release tag of `commit`, when it is the stable release.
    pub tag: Option<String>,
}

impl Host {
    pub(crate) fn build_root(&self, channel: &str) -> Result<BuildRoot, Message> {
        build_root(self.repo.as_deref(), self.sources.as_ref(), channel)
    }

    /// The builds there are now: the checkout's, or the standalone trees' (the stable
    /// release's, labelled with its tag, and the Development ones).
    pub(crate) fn discover(&self) -> Vec<Build> {
        discover(self.platform, &self.launcher_dir, &self.search, self.sources.as_ref())
    }
}

pub(crate) fn build_root(repo: Option<&Path>, sources: Option<&Sources>, channel: &str) -> Result<BuildRoot, Message> {
    if let Some(repo) = repo {
        return Ok(BuildRoot { root: repo.to_path_buf(), commit: None, tag: None });
    }
    let sources = sources.ok_or(msg!("err.build.noSources"))?;
    let tree = match channel.trim() {
        "" | STABLE => &sources.stable,
        DEVELOPMENT => sources.development.as_ref().ok_or(msg!("err.build.developmentUnavailable"))?,
        other => return Err(msg!("err.build.unknownChannel", channel = other)),
    };
    let tag = (tree.commit == sources.stable.commit).then(|| sources.stable_tag.clone());
    Ok(BuildRoot { root: tree.root.clone(), commit: Some(tree.commit.clone()), tag })
}

pub(crate) fn discover(platform: Platform, launcher_dir: &Path, search: &[PathBuf], sources: Option<&Sources>) -> Vec<Build> {
    let stable_label = sources.map(|s| format!(" ({})", s.stable_tag)).unwrap_or_default();
    let mut trees = vec![];
    if let Some(sources) = sources {
        trees.push(game::SourceTree { root: &sources.stable.root, label: &stable_label, development: false });
        // The same folder when the launcher was built from the stable commit: listed once, as stable.
        if let Some(development) = sources.development.as_ref().filter(|d| d.root != sources.stable.root) {
            trees.push(game::SourceTree { root: &development.root, label: " (Development)", development: true });
        }
    }
    game::discover_builds(platform, launcher_dir, search, &trees)
}

/// Injected ahead of the page scripts as window.LAUNCHER_CAPS.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Caps<'a> {
    pub(crate) platform: &'static str,
    /// Renderers the page may offer: both on Windows (one executable holds
    /// them), Vulkan only on Linux and macOS (where only the Shadow renderer draws).
    pub(crate) backends: &'static [&'static str],
    pub(crate) builds: &'a [Build],
    pub(crate) defaults: &'a Defaults,
    pub(crate) can_build: bool,
    /// A standalone launcher can build Development: its own commit can be fetched.
    pub(crate) can_build_development: bool,
    /// The title update is in the game folder (drives the update autoselect and the
    /// Build panel's default variant).
    pub(crate) update_installed: bool,
    /// Source revision the launcher was built from.
    pub(crate) version: build_info::BuildInfo,
    /// The release a standalone launcher builds by default.
    pub(crate) stable: build_info::Release,
    /// Outside a checkout: builds come from `stable`, or Development (`version`).
    pub(crate) standalone: bool,
}

pub(crate) fn load_settings(path: &Path) -> Settings {
    fs::read(path)
        .map(|data| game::parse_settings(&data))
        .unwrap_or_default()
}

pub(crate) fn write_settings(path: &Path, settings: &Settings) -> Result<(), String> {
    fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    let data = serde_json::to_vec_pretty(settings).map_err(|e| e.to_string())?;
    fs::write(path, data).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tree(root: &str, commit: &str) -> Tree {
        Tree { root: PathBuf::from(root), commit: commit.into() }
    }

    #[test]
    fn a_checkout_builds_every_channel_in_place() {
        let root = build_root(Some(Path::new("/repo")), None, DEVELOPMENT).unwrap();
        assert_eq!(root, BuildRoot { root: "/repo".into(), commit: None, tag: None });
    }

    #[test]
    fn standalone_builds_stable_by_default_and_its_own_commit_for_development() {
        let sources = Sources { stable: tree("/s/stable", "aaa"), stable_tag: "beta-1.4".into(), development: Some(tree("/s/dev", "bbb")) };
        for channel in ["", STABLE] {
            let root = build_root(None, Some(&sources), channel).unwrap();
            assert_eq!((root.root, root.commit.as_deref()), (PathBuf::from("/s/stable"), Some("aaa")));
            assert_eq!(root.tag.as_deref(), Some("beta-1.4"), "named for the Extras label");
        }
        let root = build_root(None, Some(&sources), DEVELOPMENT).unwrap();
        assert_eq!((root.root, root.commit.as_deref()), (PathBuf::from("/s/dev"), Some("bbb")));
        assert_eq!(root.tag, None, "not a release");
        assert_eq!(build_root(None, Some(&sources), "nightly").unwrap_err().key, "err.build.unknownChannel");
        let unfetchable = Sources { development: None, ..sources };
        assert_eq!(build_root(None, Some(&unfetchable), DEVELOPMENT).unwrap_err().key, "err.build.developmentUnavailable");
        assert!(build_root(None, Some(&unfetchable), STABLE).is_ok(), "stable still builds");
        assert_eq!(build_root(None, None, "").unwrap_err().key, "err.build.noSources");
    }

    /// An empty executable under `<root>/port/out/build/<dir>`.
    fn fake_build(root: &Path, dir: &str) {
        let folder = root.join("port/out/build").join(dir);
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join(Platform::Linux.executable()), b"").unwrap();
    }

    #[test]
    fn standalone_lists_stable_builds_with_their_tag_and_development_builds_apart() {
        let base = std::env::temp_dir().join(format!("sotr-host-trees-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let (stable, dev) = (base.join("stable"), base.join("dev"));
        fake_build(&stable, "linux-amd64-update-relwithdebinfo");
        fake_build(&dev, "linux-amd64-update-relwithdebinfo");
        fake_build(&dev, "linux-amd64-update-experimental-relwithdebinfo"); // retired profile: not listed
        // Only the newer sources have Button Icons and the Shadow renderer, whose converter is staged.
        fs::create_dir_all(dev.join("port/src/features")).unwrap();
        fs::write(dev.join("port/src/features/pc_features.cpp"), "pc_button_glyphs").unwrap();
        fs::create_dir_all(dev.join("port/src/experiments")).unwrap();
        fs::write(dev.join("port/src/experiments/shadow_renderer.cpp"), "").unwrap();
        let staged = dev.join("port/out/build/linux-amd64-update-relwithdebinfo/xenosrecomp");
        fs::create_dir_all(&staged).unwrap();
        fs::write(staged.join("XenosRecomp"), "").unwrap();
        let sources = Sources {
            stable: Tree { root: stable.clone(), commit: "a".into() },
            stable_tag: "beta-1.4".into(),
            development: Some(Tree { root: dev.clone(), commit: "b".into() }),
        };
        let builds = discover(Platform::Linux, &base.join("launcher"), &[], Some(&sources));
        let listed: Vec<_> = builds.iter().map(|b| (b.id.as_str(), b.label.as_str(), b.glyphs, b.shadow, b.development)).collect();
        assert_eq!(
            listed,
            [
                ("update", "Update (beta-1.4)", false, false, false),
                ("update-extras", "Update + Extras (beta-1.4)", false, false, false),
                ("update-development", "Update (Development)", false, true, true),
                ("update-extras-development", "Update + Extras (Development)", true, true, true),
            ]
        );
        // Built from the stable commit: one tree, listed once as stable.
        let same = Sources { development: Some(Tree { root: stable.clone(), commit: "a".into() }), ..sources };
        assert_eq!(discover(Platform::Linux, &base.join("launcher"), &[], Some(&same)).len(), 2);
        fs::remove_dir_all(base).unwrap();
    }
}
