//! Detects the tools the Build panel needs before scripts/build_client.py runs,
//! so a machine without Python or a compiler gets download links instead of a
//! failed build. Kept free of Tauri like the game module. Windows has no Python by
//! default, so none of this can live in the build scripts.

mod report;
mod detect;
mod winget;
mod sdk;
mod process;
#[cfg(test)]
mod test_support;

pub use report::*;
pub use detect::*;
pub use winget::*;
pub use sdk::*;
pub use process::*;
