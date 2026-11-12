//! The platform the launcher runs on, and its file and argument conventions.


#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Platform {
    Windows,
    Linux,
    /// Apple Silicon (or Intel); the game draws with the Shadow renderer only,
    /// through MoltenVK.
    MacOs,
}

impl Platform {
    pub fn current() -> Self {
        if cfg!(target_os = "windows") {
            Platform::Windows
        } else if cfg!(target_os = "macos") {
            Platform::MacOs
        } else {
            Platform::Linux
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Platform::Windows => "windows",
            Platform::Linux => "linux",
            Platform::MacOs => "macos",
        }
    }
    /// Build folder prefix the build scripts use. build_macos.py names its folders
    /// after the architecture (`macos-arm64`, or `macos-x86_64` for a Rosetta build);
    /// the launcher looks for builds of its own architecture.
    pub(crate) fn build_prefix(self) -> &'static str {
        match self {
            Platform::Windows => "win-amd64",
            Platform::Linux => "linux-amd64",
            Platform::MacOs if cfg!(target_arch = "x86_64") => "macos-x86_64",
            Platform::MacOs => "macos-arm64",
        }
    }
    pub(crate) fn executable(self) -> &'static str {
        match self {
            Platform::Windows => "diablo3.exe",
            Platform::Linux | Platform::MacOs => "diablo3",
        }
    }
}
