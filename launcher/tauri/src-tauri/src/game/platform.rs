//! The platform the launcher runs on, and its file and argument conventions.


#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Platform {
    Windows,
    Linux,
}

impl Platform {
    pub fn current() -> Self {
        if cfg!(target_os = "windows") {
            Platform::Windows
        } else {
            Platform::Linux
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Platform::Windows => "windows",
            Platform::Linux => "linux",
        }
    }
    pub(crate) fn build_prefix(self) -> &'static str {
        match self {
            Platform::Windows => "win-amd64",
            Platform::Linux => "linux-amd64",
        }
    }
    pub(crate) fn executable(self) -> &'static str {
        match self {
            Platform::Windows => "diablo3.exe",
            Platform::Linux => "diablo3",
        }
    }
}
