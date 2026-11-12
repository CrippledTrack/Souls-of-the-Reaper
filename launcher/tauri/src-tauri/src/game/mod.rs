//! Build discovery, game-folder validation and game arguments. Kept free of
//! Tauri so both platforms' launch rules are unit-tested on any host. The
//! rules mirror scripts/run_linux.py and scripts/run_windows.ps1.

mod platform;
mod builds;
mod settings;
mod build_request;
mod display;
mod args;
mod saves;
mod profiles;
#[cfg(test)]
mod test_support;

pub use platform::*;
pub use builds::*;
pub use settings::*;
pub use build_request::*;
pub use display::*;
pub use args::*;
pub use saves::*;
pub use profiles::*;
