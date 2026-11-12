//! Display size and the Shadow renderer's resolution and aspect ratio.

use super::*;
use crate::message::{msg, Message};
use std::{
    fs,
    path::Path,
};

/// Largest display the Shadow renderer lets the game build (its guest-space targets,
/// port/src/experiments/shadow_aspect.cpp), and the game's own.
pub(crate) const SHADOW_GUEST_MAX: (u32, u32) = (2048, 1536);

pub(crate) const SHADOW_GUEST: (u32, u32) = (1280, 720);

/// Render heights the launcher accepts when typed in.
/// (The renderer's per-frame memory grows with the square of the height.)
pub const SHADOW_HEIGHT_RANGE: std::ops::RangeInclusive<u32> = 360..=2880;

/// The display's real size in pixels. A toolkit that only knows whole-number scales
/// (GTK 3 on Wayland) reports a fractionally scaled display rounded up: a 2560x1600
/// panel at 125% arrives as 2048x1280 at scale 2, so `reported` is 4096x2560. The
/// displays' own native modes (`native_modes`, from the system) settle it: the one
/// mode of the same shape between the logical and the reported size is the real one.
/// Anything else (an exact scale, no such mode, or several) keeps `reported`.
pub fn true_monitor_size(reported: (u32, u32), scale: f64, native_modes: &[(u32, u32)]) -> (u32, u32) {
    if scale <= 1.0 || reported.0 == 0 || reported.1 == 0 {
        return reported;
    }
    let logical = (f64::from(reported.0) / scale, f64::from(reported.1) / scale);
    let shape = f64::from(reported.0) / f64::from(reported.1);
    let mut fits: Vec<(u32, u32)> = native_modes
        .iter()
        .copied()
        .filter(|&(w, h)| {
            h > 0
                && (f64::from(w) / f64::from(h) / shape - 1.0).abs() < 0.01
                && f64::from(w) >= logical.0 - 1.0
                && w <= reported.0
        })
        .collect();
    fits.sort_unstable();
    fits.dedup();
    match fits.as_slice() {
        [only] => *only,
        _ => reported,
    }
}

/// Native modes of the connected displays (Linux: the first mode each connector
/// lists under /sys/class/drm). Empty elsewhere or when unreadable.
pub fn native_display_modes(drm: &Path) -> Vec<(u32, u32)> {
    let Ok(entries) = fs::read_dir(drm) else { return Vec::new() };
    entries
        .flatten()
        .filter_map(|entry| {
            let dir = entry.path();
            if fs::read_to_string(dir.join("status")).ok()?.trim() != "connected" {
                return None;
            }
            let modes = fs::read_to_string(dir.join("modes")).ok()?;
            // "2560x1600", possibly with an interlace suffix ("1920x1080i").
            let (w, h) = modes.lines().next()?.split_once('x')?;
            let h: String = h.chars().take_while(char::is_ascii_digit).collect();
            Some((w.trim().parse().ok()?, h.parse().ok()?))
        })
        .collect()
}

/// What the Shadow renderer is asked for: the game's own display size `(width, height)`
/// (1280x720 unless the aspect ratio differs from 16:9) and the lines to draw at (0: as
/// many as that display has).
///
/// The game's display keeps 720 lines and grows sideways for wider shapes, or keeps 1280
/// columns and grows taller for narrower ones, as far as the renderer allows; a shape
/// beyond that (wider than about 2.84:1) is clamped and shown with bars. Native draws
/// at the display's height, or at fewer lines when the picture is the wider of the two
/// and so fills the display's width first.
pub fn shadow_display(options: &GameOptions) -> Result<((u32, u32), u32), Message> {
    let even = |v: f64| ((v / 2.0).round() as u32) * 2;
    let guest = match (options.shadow_aspect.as_str(), options.monitor) {
        ("" | "16:9", _) | ("auto", None) => SHADOW_GUEST,
        ("16:10", _) => (1280, 800),
        ("21:9", _) => (1680, 720),
        ("auto", Some((w, h))) if w > 0 && h > 0 => {
            let aspect = f64::from(w) / f64::from(h);
            if aspect >= 16.0 / 9.0 {
                (even(720.0 * aspect).clamp(SHADOW_GUEST.0, SHADOW_GUEST_MAX.0), 720)
            } else {
                (1280, even(1280.0 / aspect).clamp(SHADOW_GUEST.1, SHADOW_GUEST_MAX.1))
            }
        }
        ("auto", _) => SHADOW_GUEST,
        _ => return Err(msg!("err.option.aspect")),
    };
    let height = match (options.shadow_native, options.monitor) {
        (true, Some((w, h))) if w > 0 && h > 0 => {
            let fit_width = even(f64::from(w) * f64::from(guest.1) / f64::from(guest.0));
            h.min(fit_width).clamp(*SHADOW_HEIGHT_RANGE.start(), *SHADOW_HEIGHT_RANGE.end())
        }
        (true, _) => 0,
        (false, _) if options.shadow_height == 0 => 0,
        (false, _) if SHADOW_HEIGHT_RANGE.contains(&options.shadow_height) => options.shadow_height,
        _ => return Err(msg!("err.option.renderHeight", min = SHADOW_HEIGHT_RANGE.start(), max = SHADOW_HEIGHT_RANGE.end())),
    };
    Ok((guest, height))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::test_support::*;

    #[test]
    fn fractionally_scaled_displays_report_their_real_size() {
        let modes = [(1920, 1080), (2560, 1440), (2560, 1600)];
        // 2560x1600 at 125%: GTK says 2048x1280 at scale 2.
        assert_eq!(true_monitor_size((4096, 2560), 2.0, &modes), (2560, 1600));
        // Unscaled, or a true 200%: as reported.
        assert_eq!(true_monitor_size((2560, 1440), 1.0, &modes), (2560, 1440));
        assert_eq!(true_monitor_size((2560, 1600), 2.0, &modes), (2560, 1600));
        // 4K at 150% (2560x1440 logical, reported 5120x2880) beside a 1440p display of
        // the same shape: two candidates, so nothing is assumed.
        assert_eq!(true_monitor_size((5120, 2880), 2.0, &[(3840, 2160), (2560, 1440)]), (5120, 2880));
        assert_eq!(true_monitor_size((5120, 2880), 2.0, &[(3840, 2160), (1920, 1200)]), (3840, 2160));
        assert_eq!(true_monitor_size((4096, 2560), 2.0, &[]), (4096, 2560));

        let drm = scratch("drm");
        for (name, status, modes) in [("card1-eDP-1", "connected", "2560x1600\n1920x1200\n"), ("card0-DP-3", "disconnected", ""), ("card0-DP-4", "connected", "1920x1080i\n")] {
            fs::create_dir_all(drm.join(name)).unwrap();
            fs::write(drm.join(name).join("status"), status).unwrap();
            fs::write(drm.join(name).join("modes"), modes).unwrap();
        }
        let mut found = native_display_modes(&drm);
        found.sort_unstable();
        assert_eq!(found, [(1920, 1080), (2560, 1600)]);
        assert!(native_display_modes(&drm.join("missing")).is_empty());
        fs::remove_dir_all(drm).unwrap();
    }

    #[test]
    fn shadow_native_and_auto_follow_the_display() {
        let display = |monitor, native, aspect: &str| {
            let mut o = options();
            o.shadow = true;
            o.shadow_native = native;
            o.shadow_aspect = aspect.into();
            o.monitor = monitor;
            shadow_display(&o).unwrap()
        };
        // 16:9 displays: the game's own shape at the display's lines.
        assert_eq!(display(Some((2560, 1440)), true, "auto"), ((1280, 720), 1440));
        // 16:10 and 3:2 grow taller; 21:9 grows wider.
        assert_eq!(display(Some((2560, 1600)), true, "auto"), ((1280, 800), 1600));
        assert_eq!(display(Some((2256, 1504)), true, "auto"), ((1280, 854), 1504));
        assert_eq!(display(Some((3440, 1440)), true, "auto"), ((1720, 720), 1440));
        // 32:9 is beyond what the renderer builds: clamped, still at the display's lines.
        assert_eq!(display(Some((5120, 1440)), true, "auto"), ((2048, 720), 1440));
        // A picture wider than the display fills its width, so fewer lines are drawn.
        assert_eq!(display(Some((2560, 1440)), true, "21:9"), ((1680, 720), 1098));
        // Unknown display: the game's own 720p, 16:9.
        assert_eq!(display(None, true, "auto"), ((1280, 720), 0));
        // Arguments: only what differs from the game's own display is passed.
        let mut o = options();
        o.shadow = true;
        o.shadow_native = true;
        o.shadow_aspect = "auto".into();
        o.monitor = Some((3440, 1440));
        let args = launch_arguments(Platform::Linux, &build(false, false), Path::new("/g"), Path::new("/s"), &o, -1).unwrap();
        assert!(args.contains(&"--shadow_guest_width=1720".to_string()));
        assert!(args.contains(&"--shadow_render_height=1440".to_string()));
        assert!(!args.iter().any(|a| a.starts_with("--shadow_guest_height")));
    }
}
