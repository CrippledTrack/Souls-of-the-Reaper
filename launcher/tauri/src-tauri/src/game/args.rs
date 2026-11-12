//! Menu choices (GameOptions) and the game arguments they become. The rules
//! mirror scripts/run_linux.py and scripts/run_windows.ps1.

use super::*;
use crate::message::{msg, Message};
use serde::Deserialize;
use std::path::Path;

/// Choices from the launcher menu (src/js/launch-config.js, `LaunchConfig` in src/js/types.d.ts).
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GameOptions {
    pub build: Option<String>,
    pub fps: u32,
    pub vsync: bool,
    pub mnk_mouse: bool,
    pub present: String,
    /// 0 keeps the extras build's saved in-game scale.
    pub res_scale: u32,
    /// None keeps the extras build's saved in-game window mode.
    pub fullscreen: Option<bool>,
    pub lang_id: u32,
    pub country_id: u32,
    pub rtp: String,
    pub vk_path: String,
    /// "playstation" shows PlayStation button prompts (extras builds only);
    /// "xbox" or absent keeps the game's Xbox prompts.
    #[serde(default)]
    pub glyphs: String,
    /// Windows renderer: "d3d12" (also when absent) or "vulkan". Linux is always Vulkan.
    #[serde(default)]
    pub backend: String,
    /// Linux display server, filled from the saved settings (the page does not send it).
    #[serde(default)]
    pub video_driver: String,
    /// The Shadow renderer (`--shadow_renderer_only`, beta), a menu choice. The host
    /// clears it unless the build has the Shadow renderer with its converter staged.
    #[serde(default)]
    pub shadow: bool,
    /// Lines the Shadow renderer draws at (a preset or a typed value); 0 is the game's
    /// own 720. Ignored with `shadow_native`.
    #[serde(default)]
    pub shadow_height: u32,
    /// Draw at the display's own resolution (`monitor`).
    #[serde(default)]
    pub shadow_native: bool,
    /// Dynamic resolution: the height becomes the most the renderer draws at.
    #[serde(default)]
    pub shadow_dynamic: bool,
    /// Shape of the game's picture under the Shadow renderer: "" or "16:9", "16:10",
    /// "21:9", or "auto" for the display's own (`monitor`).
    #[serde(default)]
    pub shadow_aspect: String,
    /// The display's size in pixels, filled by the host (the page does not send it);
    /// None when it is unknown, which leaves Native and Auto at the game's own 720p, 16:9.
    #[serde(skip)]
    pub monitor: Option<(u32, u32)>,
}

/// Image scaling for the Vulkan presenter. The effect is only compiled into
/// SDKs built with patches/linux/rexglue-present-effects.patch (or Windows').
pub(crate) fn vulkan_present(options: &GameOptions) -> Result<&str, Message> {
    match options.present.as_str() {
        // Older saved settings send "fsr" by default; the UI
        // always sends an explicit choice, so pass it through unchanged.
        present @ ("bilinear" | "fsr" | "cas") => Ok(present),
        _ => Err(msg!("err.option.scaling")),
    }
}

pub fn launch_arguments(
    platform: Platform,
    build: &Build,
    game: &Path,
    user: &Path,
    options: &GameOptions,
    vulkan_device: i32,
) -> Result<Vec<String>, Message> {
    let shadow = options.shadow.then(|| shadow_display(options)).transpose()?;
    if shadow.is_some() {
    } else if options.res_scale > 3 || (options.res_scale == 0 && !build.extras) {
        return Err(msg!("err.option.resolution"));
    }
    if options.fullscreen.is_none() && !build.extras {
        return Err(msg!("err.option.display"));
    }
    if !["", "xbox", "playstation", "auto"].contains(&options.glyphs.as_str()) {
        return Err(msg!("err.option.glyphs"));
    }
    let mut args = vec![
        format!("--game_data_root={}", game.display()),
        format!("--user_data_root={}", user.display()),
    ];
    if build.extras {
        args.push("--extra_features=true".into());
        if ["playstation", "auto"].contains(&options.glyphs.as_str()) {
            args.push(format!("--pc_button_glyphs={}", options.glyphs));
        }
    }
    if build.update {
        // The update reads its patch CPKs through update:\ even with a prepatched XEX.
        args.push(format!("--update_data_root={}", update_layout(game).0.display()));
    }
    // The frame limiter and the 60 Hz guest timing (vsync) live in the shared GPU
    // command processor, so they apply to either renderer on every platform.
    if ![30, 60, 120, 144, 240].contains(&options.fps) {
        return Err(msg!("err.option.fps"));
    }
    args.push(format!("--d3_frame_limit={}", options.fps));
    args.push(format!("--vsync={}", options.vsync));
    match platform {
        Platform::Linux => {
            if !["fsi", "host"].contains(&options.vk_path.as_str()) {
                return Err(msg!("err.option.vulkanPath"));
            }
            args.push(format!("--cache_root={}", user.join("cache").display()));
            args.push(format!("--log_file={}", user.join("diablo3.log").display()));
            args.push(format!("--render_target_path_vulkan={}", options.vk_path));
            args.push(format!("--present_effect={}", vulkan_present(options)?));
            if vulkan_device >= 0 {
                args.push(format!("--vulkan_device={vulkan_device}"));
            }
            match options.video_driver.as_str() {
                "" => {}
                driver @ ("wayland" | "x11") => args.push(format!("--video_driver={driver}")),
                _ => return Err(msg!("err.option.displayServer")),
            }
        }
        Platform::Windows => {
            // One executable holds both renderers; --gpu_backend picks one.
            let backend = match options.backend.as_str() {
                "" | "d3d12" => "d3d12",
                "vulkan" => "vulkan",
                _ => return Err(msg!("err.option.renderer")),
            };
            args.push(format!("--gpu_backend={backend}"));
            if backend == "vulkan" {
                // Same rules as Linux: the Vulkan backend has no D3D12 render
                // path, but shares the FSR/CAS presenter.
                if !["fsi", "host"].contains(&options.vk_path.as_str()) {
                    return Err(msg!("err.option.vulkanPath"));
                }
                args.push(format!("--render_target_path_vulkan={}", options.vk_path));
                args.push(format!("--present_effect={}", vulkan_present(options)?));
                if vulkan_device >= 0 {
                    args.push(format!("--vulkan_device={vulkan_device}"));
                }
            } else {
                if !["rov", "rtv"].contains(&options.rtp.as_str()) {
                    return Err(msg!("err.option.renderPath"));
                }
                if !["fsr", "cas", "bilinear"].contains(&options.present.as_str()) {
                    return Err(msg!("err.option.scaling"));
                }
                args.push(format!("--render_target_path_d3d12={}", options.rtp));
                args.push("--rtv_color_depth_ownership_mode=8".into());
                args.push("--execute_unclipped_draw_vs_on_cpu=true".into());
                args.push(format!("--present_effect={}", options.present));
            }
            args.push("--mnk_cursor_visible=false".into());
        }
    }
    if options.shadow {
        // The game's own Vulkan renderer takes the window; the SDK's backend chosen
        // above keeps running the game's GPU commands without drawing.
        args.push("--shadow_renderer_only=true".into());
        // It honours the Vulkan device setting itself; Windows only passed it above for
        // the SDK's Vulkan backend.
        if vulkan_device >= 0 && !args.iter().any(|a| a.starts_with("--vulkan_device=")) {
            args.push(format!("--vulkan_device={vulkan_device}"));
        }
        // The game's own display (1280x720) reshaped; the render height is the lines
        // actually drawn, whatever that display's height.
        let ((width, height), lines) = shadow.unwrap_or((SHADOW_GUEST, 0));
        if width != SHADOW_GUEST.0 {
            args.push(format!("--shadow_guest_width={width}"));
        }
        if height != SHADOW_GUEST.1 {
            args.push(format!("--shadow_guest_height={height}"));
        }
        if lines > 0 && lines != height {
            args.push(format!("--shadow_render_height={lines}"));
        }
        // Nothing to give up at the game's own resolution, which is also its floor.
        if options.shadow_dynamic && lines > height {
            args.push("--shadow_dynamic_resolution=true".into());
        }
    }
    args.push("--mnk_mode=true".into());
    args.push(format!("--mnk_mouse={}", options.mnk_mouse));
    args.push(format!("--user_language={}", options.lang_id));
    args.push(format!("--user_country={}", options.country_id));
    // The Shadow renderer draws at its own resolution; the SDK's scale would do nothing.
    if options.res_scale > 0 && !options.shadow {
        args.push(format!("--draw_resolution_scale_x={}", options.res_scale));
        args.push(format!("--draw_resolution_scale_y={}", options.res_scale));
        if build.extras {
            args.push("--pc_use_saved_render_scale=false".into());
        }
    }
    if let Some(fullscreen) = options.fullscreen {
        args.push(format!("--fullscreen={fullscreen}"));
        if build.extras {
            args.push("--pc_use_saved_window_mode=false".into());
        }
    }
    Ok(args)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::test_support::*;
    use std::fs;

    #[test]
    fn update_mounts_update_root_and_keeps_paths_with_spaces() {
        let game = Path::new("/tmp/game with spaces");
        let args = launch_arguments(
            Platform::Linux,
            &build(true, false),
            game,
            Path::new("/s d"),
            &options(),
            1,
        )
        .unwrap();
        assert!(args.contains(&"--game_data_root=/tmp/game with spaces".into()));
        assert!(args.contains(&"--update_data_root=/tmp/game with spaces".into()));
        let log = Path::new("/s d").join("diablo3.log");
        assert!(args.contains(&format!("--log_file={}", log.display())));
        let base = launch_arguments(
            Platform::Linux,
            &build(false, false),
            game,
            Path::new("/s"),
            &options(),
            1,
        )
        .unwrap();
        assert!(!base.iter().any(|a| a.starts_with("--update_data_root")));
    }

    #[test]
    fn shadow_renderer_is_passed_only_when_enabled() {
        for platform in [Platform::Linux, Platform::Windows] {
            let run = |on| {
                let mut o = options();
                o.shadow = on;
                launch_arguments(platform, &build(false, false), Path::new("/g"), Path::new("/s"), &o, -1).unwrap()
            };
            let on = run(true);
            assert!(on.contains(&"--shadow_renderer_only=true".to_string()));
            assert!(!on.iter().any(|a| a.starts_with("--draw_resolution_scale") || a.starts_with("--shadow_render_height")
                || a.starts_with("--shadow_guest")), "the game's own 720p, 16:9");
            let off = run(false);
            assert!(!off.iter().any(|a| a.starts_with("--shadow_")));
            assert!(off.iter().any(|a| a.starts_with("--draw_resolution_scale_x")));
        }
    }

    #[test]
    fn shadow_renderer_resolution_and_aspect_are_passed_and_checked() {
        let run = |height, aspect: &str| {
            let mut o = options();
            o.shadow = true;
            o.shadow_height = height;
            o.shadow_aspect = aspect.into();
            o.res_scale = 0; // "In-game" or anything else: not used by this renderer
            launch_arguments(Platform::Linux, &build(false, false), Path::new("/g"), Path::new("/s"), &o, -1)
        };
        let wide = run(1440, "21:9").unwrap();
        assert!(wide.contains(&"--shadow_render_height=1440".to_string()));
        assert!(wide.contains(&"--shadow_guest_width=1680".to_string()));
        assert!(run(2160, "16:10").unwrap().contains(&"--shadow_guest_height=800".to_string()));
        // The chosen Vulkan device reaches the renderer on both platforms, once.
        for platform in [Platform::Linux, Platform::Windows] {
            let mut o = options();
            o.shadow = true;
            let args = launch_arguments(platform, &build(false, false), Path::new("/g"), Path::new("/s"), &o, 1).unwrap();
            assert_eq!(args.iter().filter(|a| *a == "--vulkan_device=1").count(), 1);
        }
        // Dynamic resolution is passed with a height above the game's own.
        for (height, expected) in [(1440, true), (0, false)] {
            let mut o = options();
            o.shadow = true;
            o.shadow_dynamic = true;
            o.shadow_height = height;
            let args = launch_arguments(Platform::Linux, &build(false, false), Path::new("/g"), Path::new("/s"), &o, -1).unwrap();
            assert_eq!(args.contains(&"--shadow_dynamic_resolution=true".to_string()), expected);
        }
        assert!(run(1000, "16:9").unwrap().contains(&"--shadow_render_height=1000".to_string()), "any typed height");
        assert!(run(200, "16:9").is_err());
        assert!(run(3000, "16:9").is_err());
        assert!(run(1080, "4:3").is_err());
    }

    #[test]
    fn linux_display_server_is_passed_only_when_chosen() {
        let run = |platform, driver: &str| {
            let mut o = options();
            o.video_driver = driver.into();
            launch_arguments(platform, &build(false, false), Path::new("/g"), Path::new("/s"), &o, -1)
        };
        let auto = run(Platform::Linux, "").unwrap();
        assert!(!auto.iter().any(|a| a.starts_with("--video_driver")));
        assert!(run(Platform::Linux, "wayland").unwrap().contains(&"--video_driver=wayland".into()));
        assert!(run(Platform::Linux, "x11").unwrap().contains(&"--video_driver=x11".into()));
        assert!(run(Platform::Linux, "mir").is_err());
        let windows = run(Platform::Windows, "wayland").unwrap();
        assert!(!windows.iter().any(|a| a.starts_with("--video_driver")), "Windows ignores it");
    }

    #[test]
    fn linux_omits_d3d12_and_windows_only_options() {
        let args = launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            -1,
        )
        .unwrap();
        assert!(args.contains(&"--render_target_path_vulkan=fsi".into()));
        assert!(!args.iter().any(|a| a.contains("d3d12")));
        // The frame limiter and guest timing are shared by every renderer.
        assert!(args.contains(&"--d3_frame_limit=120".into()));
        assert!(args.iter().any(|a| a.starts_with("--vsync=")));
        let mut bad = options();
        bad.fps = 75;
        assert!(launch_arguments(Platform::Linux, &build(false, false), Path::new("/g"), Path::new("/s"), &bad, -1)
            .is_err());
        assert!(args.contains(&"--present_effect=fsr".into()));
        let mut off = options();
        off.present = "bilinear".into();
        let off_args = launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &off,
            -1,
        )
        .unwrap();
        assert!(off_args.contains(&"--present_effect=bilinear".into()));
        off.present = "fsr2".into();
        assert!(launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &off,
            -1
        )
        .is_err());
        assert!(
            !args.iter().any(|a| a.starts_with("--vulkan_device")),
            "auto device passes nothing"
        );
        let pinned = launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            0,
        )
        .unwrap();
        assert!(pinned.contains(&"--vulkan_device=0".into()));
    }

    #[test]
    fn windows_passes_d3d12_presentation_and_frame_limit() {
        let args = launch_arguments(
            Platform::Windows,
            &build(false, false),
            Path::new("C:\\g"),
            Path::new("C:\\s"),
            &options(),
            1,
        )
        .unwrap();
        for expected in [
            "--render_target_path_d3d12=rtv",
            "--d3_frame_limit=120",
            "--present_effect=fsr",
            "--vsync=false",
        ] {
            assert!(args.contains(&expected.into()), "{expected}");
        }
        assert!(args.contains(&"--gpu_backend=d3d12".into()));
        assert!(!args.iter().any(|a| a.contains("vulkan")));
    }

    #[test]
    fn windows_vulkan_uses_vulkan_options_not_d3d12_ones() {
        let mut opts = options();
        opts.backend = "vulkan".into();
        opts.vsync = true;
        let args = launch_arguments(
            Platform::Windows,
            &build(false, true),
            Path::new("C:/g"),
            Path::new("C:/s"),
            &opts,
            1,
        )
        .unwrap();
        for expected in [
            "--gpu_backend=vulkan",
            "--render_target_path_vulkan=fsi",
            "--vsync=true",
            "--d3_frame_limit=120",
            "--vulkan_device=1",
            "--present_effect=fsr",
            "--mnk_cursor_visible=false",
        ] {
            assert!(args.contains(&expected.into()), "{expected}");
        }
        assert!(!args.iter().any(|a| a.contains("d3d12") || a.contains("rtv_color_depth")));
        opts.backend = "metal".into();
        assert!(launch_arguments(
            Platform::Windows,
            &build(false, true),
            Path::new("C:/g"),
            Path::new("C:/s"),
            &opts,
            -1,
        )
        .is_err());
    }

    #[test]
    fn extras_in_game_choices_leave_saved_settings_alone() {
        let mut opts = options();
        opts.res_scale = 0;
        opts.fullscreen = None;
        let args = launch_arguments(
            Platform::Linux,
            &build(false, true),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1,
        )
        .unwrap();
        assert!(!args.iter().any(|a| a.contains("resolution_scale")
            || a.contains("pc_use_saved")
            || a.starts_with("--fullscreen")));
        // The same choices are invalid for a normal build.
        assert!(launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
    }

    #[test]
    fn extras_flag_is_only_passed_when_chosen() {
        let run = |extras| {
            launch_arguments(
                Platform::Linux,
                &build(false, extras),
                Path::new("/g"),
                Path::new("/s"),
                &options(),
                -1,
            )
            .unwrap()
        };
        assert!(run(true).contains(&"--extra_features=true".to_string()));
        assert!(!run(false).iter().any(|a| a.starts_with("--extra_features")));
    }

    #[test]
    fn playstation_glyphs_are_only_passed_to_extras_builds() {
        let run = |extras, glyphs: &str| {
            let mut opts = options();
            opts.glyphs = glyphs.into();
            launch_arguments(
                Platform::Linux,
                &build(false, extras),
                Path::new("/g"),
                Path::new("/s"),
                &opts,
                -1,
            )
        };
        let flag = "--pc_button_glyphs=playstation".to_string();
        assert!(run(true, "playstation").unwrap().contains(&flag));
        assert!(run(true, "auto").unwrap().contains(&"--pc_button_glyphs=auto".to_string()));
        assert!(!run(false, "auto").unwrap().iter().any(|a| a.starts_with("--pc_button_glyphs")));
        assert!(!run(true, "xbox").unwrap().iter().any(|a| a.starts_with("--pc_button_glyphs")));
        assert!(!run(true, "").unwrap().iter().any(|a| a.starts_with("--pc_button_glyphs")));
        // The cvar only exists in extras builds, so normal builds never get it.
        assert!(!run(false, "playstation").unwrap().iter().any(|a| a.starts_with("--pc_button_glyphs")));
        assert!(run(true, "ps4").is_err());
    }

    #[test]
    fn extras_explicit_choices_override_saved_settings() {
        let args = launch_arguments(
            Platform::Windows,
            &build(true, true),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            -1,
        )
        .unwrap();
        for expected in [
            "--draw_resolution_scale_x=2",
            "--pc_use_saved_render_scale=false",
            "--fullscreen=true",
            "--pc_use_saved_window_mode=false",
        ] {
            assert!(args.contains(&expected.into()), "{expected}");
        }
        let normal = launch_arguments(
            Platform::Windows,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &options(),
            -1,
        )
        .unwrap();
        assert!(
            !normal.iter().any(|a| a.starts_with("--pc_use_saved")),
            "normal builds lack these cvars"
        );
    }

    #[test]
    fn rejects_unsupported_values() {
        let mut opts = options();
        opts.res_scale = 4;
        assert!(launch_arguments(
            Platform::Linux,
            &build(false, true),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
        let mut opts = options();
        opts.vk_path = "rov".into();
        assert!(launch_arguments(
            Platform::Linux,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
        let mut opts = options();
        opts.fps = 75;
        assert!(launch_arguments(
            Platform::Windows,
            &build(false, false),
            Path::new("/g"),
            Path::new("/s"),
            &opts,
            -1
        )
        .is_err());
    }

    #[test]
    fn update_overlay_mounts_its_folder_as_update_and_validates_there() {
        let game = scratch("overlay");
        let overlay = game.join("update");
        fs::create_dir_all(overlay.join("CPKs")).unwrap();
        // Without the overlay executable the folder is a plain staged copy.
        assert_eq!(update_layout(&game).0, game);
        fs::write(overlay.join("Default.xex"), "x").unwrap();
        assert_eq!(update_layout(&game).0, overlay);
        let args = launch_arguments(
            Platform::Linux,
            &build(true, false),
            &game,
            Path::new("/s"),
            &options(),
            -1,
        )
        .unwrap();
        assert!(args.contains(&format!("--update_data_root={}", overlay.display())));
        assert_eq!(validate_game_dir(&build(true, false), &game).unwrap_err().key, "err.gameDir.unverified.update");
        // One folder for both versions builds update as an overlay: no second folder is passed.
        let request = BuildRequest {
            iso: String::new(),
            title_update: String::new(),
            variants: vec!["update".into()],
            cmake: String::new(),
            download_sdk: false,
            download_sources: false,
            channel: String::new(),
        };
        let args = build_client_arguments(Platform::Linux, Path::new("/r"), &request, &game, &game).unwrap();
        assert!(!args.iter().any(|a| a.starts_with("--game-dir-update")));
        fs::remove_dir_all(game).unwrap();
    }
}
