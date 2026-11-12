//! What the tool check reports to the Build panel.

use crate::message::Message;
use serde::Serialize;
use std::path::PathBuf;

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Tool {
    pub id: &'static str,
    pub label: String,
    pub found: bool,
    pub path: String,
    /// Where to download it; empty when the system package manager is the way.
    pub url: &'static str,
    /// Shown while the tool is missing: what it is for, or how to get it.
    pub note: Message,
    /// winget package that installs it (Windows); empty when there is none.
    pub winget: &'static str,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Sdk {
    /// Built from the pinned commit and patches, so the build can use it.
    pub installed: bool,
    /// Built, but from another commit or patch set; a rebuild is needed.
    pub stale: bool,
    pub repository: String,
    pub commit: String,
}

/// Repository sources a launcher outside a checkout still has to download.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Sources {
    pub repository: String,
    pub commit: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ToolReport {
    pub tools: Vec<Tool>,
    pub sdk: Sdk,
    /// The Shadow renderer's shader converter (XenosRecomp), a pinned component the
    /// build fetches and builds once like the SDK. None for sources from before it
    /// was part of every build (they do not report it).
    pub converter: Option<Sdk>,
    /// Set by the host when the sources are missing; detect() leaves it empty.
    pub sources: Option<Sources>,
    /// Problems that do not block Build but may make it fail later.
    pub warnings: Vec<Message>,
    /// Every required tool was found.
    pub ready: bool,
    /// winget can install the missing tools (Windows 10 1809+ with App Installer).
    pub winget: bool,
    /// Folders of tools found off PATH (Visual Studio's CMake, LLVM); the build
    /// script's PATH needs them too.
    #[serde(skip)]
    pub path_dirs: Vec<PathBuf>,
}
