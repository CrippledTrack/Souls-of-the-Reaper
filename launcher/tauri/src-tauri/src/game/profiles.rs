//! Local profiles in a save folder, the same files the game reads
//! (patches/profiles/src/system/xam/profile_store.cpp). A profile is an XUID
//! with a name: the XUID never changes and names the profile's save folder
//! (`<root>/<XUID>/`), the name is only a label in `<root>/profiles/<XUID>.name`,
//! so renaming a profile never moves or hides its saves.

use crate::message::{msg, Message};
use serde::Serialize;
use std::{
    collections::hash_map::RandomState,
    fs,
    hash::{BuildHasher, Hasher},
    io,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

/// The XUID saves had before profiles; the profile it becomes keeps the old name.
pub const DEFAULT_XUID: u64 = 0xB13E_BABE_BABE_BABE;
/// New profiles keep these high bits, which pass Diablo III's XUID check.
const XUID_HIGH: u64 = 0xB13E_BABE_0000_0000;
/// XAM name buffers hold 16 bytes with the terminator.
pub const MAX_NAME_BYTES: usize = 15;
const PROFILES_DIR: &str = "profiles";
const TITLE_DIR: &str = "394F07D4";

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Profile {
    /// 16 upper-case hex digits, as the save folder is named.
    pub xuid: String,
    pub name: String,
}

pub fn xuid_text(xuid: u64) -> String {
    format!("{xuid:016X}")
}

fn parse_xuid(text: &str) -> Option<u64> {
    if text.len() != 16 || !text.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    // XUID 0 holds content shared by every user, not a profile.
    u64::from_str_radix(text, 16).ok().filter(|&x| x != 0)
}

/// A usable name: surrounding spaces trimmed, cut to 15 bytes on a character
/// boundary. Empty when nothing usable is left (as the game decides).
pub fn clean_name(raw: &str) -> String {
    let trimmed = raw.trim();
    let mut end = trimmed.len().min(MAX_NAME_BYTES);
    while !trimmed.is_char_boundary(end) {
        end -= 1;
    }
    let name = trimmed[..end].trim_end();
    if name.chars().any(|c| c.is_control()) {
        return String::new();
    }
    name.to_string()
}

fn name_file(root: &Path, xuid: u64) -> PathBuf {
    root.join(PROFILES_DIR).join(format!("{}.name", xuid_text(xuid)))
}

fn read_name(root: &Path, xuid: u64) -> Option<String> {
    let name = clean_name(&fs::read_to_string(name_file(root, xuid)).ok()?);
    (!name.is_empty()).then_some(name)
}

fn write_name(root: &Path, xuid: u64, name: &str) -> io::Result<()> {
    fs::create_dir_all(root.join(PROFILES_DIR))?;
    // Written beside the name file and moved over it, so a crash never leaves half a name.
    let target = name_file(root, xuid);
    let temporary = target.with_extension("name.tmp");
    fs::write(&temporary, name)?;
    fs::rename(&temporary, &target)
}

fn save_folders(root: &Path) -> Vec<u64> {
    fs::read_dir(root)
        .map(|entries| {
            entries
                .flatten()
                .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
                .filter_map(|e| parse_xuid(&e.file_name().to_string_lossy()))
                .collect()
        })
        .unwrap_or_default()
}

fn named(root: &Path) -> Vec<Profile> {
    let mut xuids = save_folders(root);
    if let Ok(entries) = fs::read_dir(root.join(PROFILES_DIR)) {
        for entry in entries.flatten() {
            let file = entry.file_name().to_string_lossy().to_string();
            if let Some(xuid) = file.strip_suffix(".name").and_then(parse_xuid) {
                xuids.push(xuid);
            }
        }
    }
    xuids.sort_unstable();
    xuids.dedup();
    let mut profiles: Vec<Profile> = xuids
        .into_iter()
        .filter_map(|x| read_name(root, x).map(|name| Profile { xuid: xuid_text(x), name }))
        .collect();
    let default = xuid_text(DEFAULT_XUID);
    profiles.sort_by_key(|p| (p.xuid != default, p.name.to_lowercase()));
    profiles
}

fn name_taken(profiles: &[Profile], name: &str, except: Option<&str>) -> bool {
    let wanted = name.to_lowercase();
    profiles.iter().any(|p| Some(p.xuid.as_str()) != except && p.name.to_lowercase() == wanted)
}

/// `name`, or `name 2`, `name 3`... when another profile has it.
fn unique_name(profiles: &[Profile], name: &str) -> String {
    let mut unique = name.to_string();
    let mut n = 2;
    while name_taken(profiles, &unique, None) {
        let suffix = format!(" {n}");
        unique = clean_name(&truncate(name, MAX_NAME_BYTES - suffix.len())) + &suffix;
        n += 1;
    }
    unique
}

fn truncate(text: &str, bytes: usize) -> String {
    let mut end = text.len().min(bytes);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    text[..end].to_string()
}

/// Names the save folders made before profiles, as the game does: the default
/// one keeps the player name (`default_name`, else "User"), the old co-op guest
/// folders become Guest2-4, anything else "Player XXXX".
fn migrate(root: &Path, default_name: &str) {
    for xuid in save_folders(root) {
        if read_name(root, xuid).is_some() {
            continue;
        }
        let name = if xuid == DEFAULT_XUID {
            Some(clean_name(default_name)).filter(|n| !n.is_empty()).unwrap_or_else(|| "User".into())
        } else if xuid > DEFAULT_XUID && xuid <= DEFAULT_XUID + 3 {
            format!("Guest{}", xuid - DEFAULT_XUID + 1)
        } else {
            format!("Player {:04X}", xuid & 0xFFFF)
        };
        let _ = write_name(root, xuid, &unique_name(&named(root), &name));
    }
}

/// Every profile in a save folder (named first if made before profiles): the
/// default profile first, then the rest by name.
pub fn list_profiles(root: &Path, default_name: &str) -> Vec<Profile> {
    migrate(root, default_name);
    named(root)
}

fn random_u32() -> u32 {
    let mut hasher = RandomState::new().build_hasher();
    hasher.write_u128(SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0));
    hasher.finish() as u32
}

/// The `SOTR_PROFILE` value for the Profile setting: an XUID, or empty (the
/// "Choose in game" choice, or nothing chosen) so the game asks Player 1.
pub fn launch_profile(setting: &str) -> String {
    parse_xuid(setting.trim()).map(xuid_text).unwrap_or_default()
}

/// Makes a profile with a new XUID. Its save folder appears once it plays.
pub fn create_profile(root: &Path, raw_name: &str, default_name: &str) -> Result<Profile, Message> {
    let name = clean_name(raw_name);
    if name.is_empty() {
        return Err(msg!("err.profile.noName"));
    }
    let profiles = list_profiles(root, default_name);
    if name_taken(&profiles, &name, None) {
        return Err(msg!("err.profile.taken", name = name));
    }
    // A random low half: Diablo III sometimes passes only the low 32 bits of an
    // XUID, so they must differ between profiles.
    let taken = |low: u32| low == 0 || (0..=3).any(|n| low == (DEFAULT_XUID + n) as u32)
        || profiles.iter().any(|p| parse_xuid(&p.xuid).is_some_and(|x| x as u32 == low));
    let low = (0..64).map(|_| random_u32()).find(|&low| !taken(low)).ok_or(msg!("err.profile.save", detail = "no free XUID"))?;
    let xuid = XUID_HIGH | u64::from(low);
    write_name(root, xuid, &name).map_err(|e| msg!("err.profile.save", detail = e))?;
    Ok(Profile { xuid: xuid_text(xuid), name })
}

pub fn rename_profile(root: &Path, xuid: &str, raw_name: &str, default_name: &str) -> Result<Profile, Message> {
    let id = parse_xuid(xuid).ok_or(msg!("err.profile.missing"))?;
    let name = clean_name(raw_name);
    if name.is_empty() {
        return Err(msg!("err.profile.noName"));
    }
    let profiles = list_profiles(root, default_name);
    if !profiles.iter().any(|p| p.xuid == xuid_text(id)) {
        return Err(msg!("err.profile.missing"));
    }
    if name_taken(&profiles, &name, Some(&xuid_text(id))) {
        return Err(msg!("err.profile.taken", name = name));
    }
    write_name(root, id, &name).map_err(|e| msg!("err.profile.save", detail = e))?;
    Ok(Profile { xuid: xuid_text(id), name })
}

/// Removes a profile with its saves and its title settings.
pub fn delete_profile(root: &Path, xuid: &str) -> Result<(), Message> {
    let id = parse_xuid(xuid).ok_or(msg!("err.profile.missing"))?;
    let text = xuid_text(id);
    let remove_dir = |dir: PathBuf| match fs::remove_dir_all(&dir) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => Err(msg!("err.profile.delete", folder = dir.display(), detail = e)),
        _ => Ok(()),
    };
    remove_dir(root.join(&text))?;
    remove_dir(root.join(TITLE_DIR).join("profile").join(&text))?;
    match fs::remove_file(name_file(root, id)) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => Err(msg!("err.profile.delete", folder = root.display(), detail = e)),
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::game::test_support::scratch;

    fn save_folder(root: &Path, xuid: &str) {
        fs::create_dir_all(root.join(xuid).join("394F07D4/00000001/d3save")).unwrap();
    }

    #[test]
    fn choose_in_game_passes_no_profile() {
        assert_eq!(launch_profile("b13ebabebabebabe"), "B13EBABEBABEBABE");
        assert_eq!(launch_profile("ingame"), "");
        assert_eq!(launch_profile(""), "");
    }

    #[test]
    fn names_are_trimmed_and_cut_on_a_character_boundary() {
        assert_eq!(clean_name("  Reaper  "), "Reaper");
        assert_eq!(clean_name("abcdefghijklmnopq"), "abcdefghijklmno");
        assert_eq!(clean_name("abcdefghijklmn\u{e9}"), "abcdefghijklmn", "é would need byte 16");
        assert_eq!(clean_name("   "), "");
        assert_eq!(clean_name("a\nb"), "");
    }

    #[test]
    fn saves_made_before_profiles_become_profiles() {
        let root = scratch("profiles-migrate");
        save_folder(&root, "B13EBABEBABEBABE");
        save_folder(&root, "B13EBABEBABEBABF");
        save_folder(&root, "0000000000000000");
        fs::create_dir_all(root.join("394F07D4")).unwrap();
        let profiles = list_profiles(&root, "Nephalem");
        assert_eq!(
            profiles,
            vec![
                Profile { xuid: "B13EBABEBABEBABE".into(), name: "Nephalem".into() },
                Profile { xuid: "B13EBABEBABEBABF".into(), name: "Guest2".into() },
            ],
            "XUID 0 (shared content) and the title folder are not profiles"
        );
        assert_eq!(fs::read_to_string(root.join("profiles/B13EBABEBABEBABE.name")).unwrap(), "Nephalem");
        assert_eq!(list_profiles(&root, "Changed")[0].name, "Nephalem", "named once");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn profiles_are_created_renamed_and_deleted_by_xuid() {
        let root = scratch("profiles-edit");
        save_folder(&root, "B13EBABEBABEBABE");
        let made = create_profile(&root, " Friend ", "Me").unwrap();
        assert_eq!(made.name, "Friend");
        assert!(made.xuid.starts_with("B13EBABE") && made.xuid != "B13EBABEBABEBABE");
        assert_eq!(create_profile(&root, "friend", "Me").unwrap_err().key, "err.profile.taken");
        assert_eq!(create_profile(&root, " ", "Me").unwrap_err().key, "err.profile.noName");
        assert_eq!(list_profiles(&root, "Me").len(), 2, "listed before it has saves");

        // Renaming keeps the saves: the folder is the XUID's.
        save_folder(&root, &made.xuid);
        rename_profile(&root, &made.xuid, "Buddy", "Me").unwrap();
        assert!(root.join(&made.xuid).is_dir());
        assert_eq!(rename_profile(&root, &made.xuid, "me", "Me").unwrap_err().key, "err.profile.taken");
        assert_eq!(rename_profile(&root, "B13EBABE00000000", "X", "Me").unwrap_err().key, "err.profile.missing");
        let names: Vec<_> = list_profiles(&root, "Me").into_iter().map(|p| p.name).collect();
        assert_eq!(names, ["Me", "Buddy"]);

        fs::create_dir_all(root.join("394F07D4/profile").join(&made.xuid)).unwrap();
        delete_profile(&root, &made.xuid).unwrap();
        assert!(!root.join(&made.xuid).exists());
        assert!(!root.join("394F07D4/profile").join(&made.xuid).exists());
        assert_eq!(list_profiles(&root, "Me").len(), 1);
        assert!(root.join("B13EBABEBABEBABE").is_dir(), "other profiles untouched");
        fs::remove_dir_all(root).unwrap();
    }
}
