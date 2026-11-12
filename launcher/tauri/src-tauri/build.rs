//! Embeds the source revision the launcher is built from (SOTR_COMMIT,
//! SOTR_DIRTY) and its GitHub remote (SOTR_REPO, default: origin) so a release knows which sources to fetch and which build a bug
//! report came from. `SOTR_COMMIT` / `SOTR_DIRTY` in the environment win over
//! git, for CI and source archives without git data. SOTR_REQUIRE_CLEAN=1
//! fails the build when the revision is unknown or the tree has changes.
//! It also embeds the release a standalone launcher builds by default
//! (SOTR_STABLE_TAG, SOTR_STABLE_COMMIT): from those variables when both are
//! set (the release workflow sets them to the release it builds), otherwise
//! from launcher/stable.json.

use std::{env, fs, path::PathBuf, process::Command};

#[path = "src/stable_pin.rs"]
mod stable_pin;

fn git(args: &[&str]) -> Option<String> {
    let output = Command::new("git").args(args).output().ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_string())
}

/// Reruns the script when the checked-out revision or the index moves.
fn watch_git() {
    for path in ["HEAD", "index"] {
        if let Some(file) = git(&["rev-parse", "--git-path", path]) {
            println!("cargo:rerun-if-changed={file}");
        }
    }
    // HEAD only names a branch; the commit it points at lives in the ref.
    if let Some(reference) = git(&["symbolic-ref", "-q", "HEAD"]) {
        if let Some(file) = git(&["rev-parse", "--git-path", &reference]) {
            println!("cargo:rerun-if-changed={file}");
        }
    }
}

fn main() {
    for name in ["SOTR_COMMIT", "SOTR_DIRTY", "SOTR_REPO", "SOTR_REQUIRE_CLEAN"] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    watch_git();
    let commit = env::var("SOTR_COMMIT")
        .ok()
        .filter(|c| !c.trim().is_empty())
        .or_else(|| git(&["rev-parse", "HEAD"]))
        .unwrap_or_else(|| "unknown".into());
    let dirty = match env::var("SOTR_DIRTY") {
        Ok(value) => value == "1",
        // Without git data nothing can be said about local changes.
        Err(_) => git(&["status", "--porcelain"]).is_some_and(|s| !s.is_empty()),
    };
    println!("cargo:rustc-env=SOTR_COMMIT={}", commit.trim());
    println!("cargo:rustc-env=SOTR_DIRTY={}", u8::from(dirty));
    let repository = env::var("SOTR_REPO")
        .ok()
        .filter(|r| !r.trim().is_empty())
        .or_else(|| git(&["remote", "get-url", "origin"]))
        .unwrap_or_default();
    println!("cargo:rustc-env=SOTR_REPO={}", repository.trim());
    if env::var("SOTR_REQUIRE_CLEAN").is_ok_and(|v| v == "1") && (dirty || commit == "unknown") {
        let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap_or_default());
        panic!(
            "SOTR_REQUIRE_CLEAN is set but the build in {} is {}. Commit or push first, or set SOTR_COMMIT.",
            manifest.display(),
            if dirty { "modified" } else { "from an unknown revision" }
        );
    }
    println!("cargo:rerun-if-env-changed=SOTR_STABLE_TAG");
    println!("cargo:rerun-if-env-changed=SOTR_STABLE_COMMIT");
    let pin = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap_or_default()).join("../../stable.json");
    println!("cargo:rerun-if-changed={}", pin.display());
    let given = |name: &str| env::var(name).ok().filter(|v| !v.trim().is_empty());
    let (tag, commit) = match (given("SOTR_STABLE_TAG"), given("SOTR_STABLE_COMMIT")) {
        (Some(tag), Some(commit)) => stable_pin::check(&tag, &commit)
            .unwrap_or_else(|e| panic!("SOTR_STABLE_TAG / SOTR_STABLE_COMMIT: {e}")),
        (None, None) => {
            let text = fs::read_to_string(&pin).unwrap_or_else(|e| panic!("Unable to read {}: {e}", pin.display()));
            stable_pin::parse(&text).unwrap_or_else(|e| panic!("{}: {e}", pin.display()))
        }
        _ => panic!("Set both SOTR_STABLE_TAG and SOTR_STABLE_COMMIT, or neither (launcher/stable.json is used)."),
    };
    println!("cargo:rustc-env=SOTR_STABLE_TAG={tag}");
    println!("cargo:rustc-env=SOTR_STABLE_COMMIT={commit}");
    tauri_build::build();
}
