//! Fixtures shared by the game module tests.

use super::*;
use std::{fs, path::PathBuf};

pub(crate) fn options() -> GameOptions {
    GameOptions {
        build: None,
        fps: 120,
        vsync: false,
        mnk_mouse: false,
        present: "fsr".into(),
        res_scale: 2,
        fullscreen: Some(true),
        lang_id: 1,
        country_id: 103,
        rtp: "rtv".into(),
        vk_path: "fsi".into(),
        glyphs: String::new(),
        backend: String::new(),
        video_driver: String::new(),
        shadow: false,
        shadow_height: 0,
        shadow_native: false,
        shadow_dynamic: false,
        shadow_aspect: String::new(),
        monitor: None,
    }
}

pub(crate) fn request(variants: &[&str]) -> BuildRequest {
    BuildRequest {
        iso: " /isos/d3 usa.iso ".into(),
        title_update: String::new(),
        variants: variants.iter().map(|v| v.to_string()).collect(),
        cmake: String::new(),
        download_sdk: false,
        download_sources: false,
        channel: String::new(),
    }
}

pub(crate) fn build(update: bool, extras: bool) -> Build {
    make_build(update, extras, PathBuf::from("diablo3"), None)
}

pub(crate) fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("sotr-launcher-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    dir
}

pub(crate) fn pair(update: bool, extras: bool) -> Build {
    make_build(update, extras, PathBuf::from("/x"), None)
}
