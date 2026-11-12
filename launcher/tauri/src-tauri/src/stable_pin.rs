//! Reads launcher/stable.json: the release a standalone launcher builds by
//! default (its normal build profile). Shared by build.rs, which embeds it, and
//! the tests.

/// (tag, commit) from the file's text, or why it is not usable.
pub fn parse(text: &str) -> Result<(String, String), String> {
    let value: serde_json::Value = serde_json::from_str(text).map_err(|e| format!("not JSON: {e}"))?;
    let field = |name: &str| value[name].as_str().unwrap_or_default().to_string();
    check(&field("tag"), &field("commit"))
}

/// The same checks for a pin given directly (SOTR_STABLE_TAG / SOTR_STABLE_COMMIT).
pub fn check(tag: &str, commit: &str) -> Result<(String, String), String> {
    let (tag, commit) = (tag.trim().to_string(), commit.trim().to_string());
    if commit.len() != 40 || !commit.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(format!("\"commit\" must be a full 40-character commit id, not {commit:?}"));
    }
    // Shown in labels and folder-free; keep it to what a git tag name usually is.
    if tag.is_empty() || tag.len() > 32 || !tag.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)) {
        return Err(format!("\"tag\" must be a short release tag such as beta-1.4, not {tag:?}"));
    }
    Ok((tag, commit.to_ascii_lowercase()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_checked_in_pin_parses() {
        let text = include_str!("../../../stable.json");
        let (tag, commit) = parse(text).unwrap();
        assert!(!tag.is_empty());
        assert_eq!(commit.len(), 40);
    }

    #[test]
    fn malformed_pins_are_rejected() {
        let id = "095fb2c67e78b803f2c55d241bd238afb86fc72d";
        assert!(parse(&format!(r#"{{"tag":"beta-1.4","commit":"{id}"}}"#)).is_ok());
        assert!(parse(r#"{"tag":"beta-1.4","commit":"095fb2c6"}"#).is_err());
        assert!(parse(&format!(r#"{{"tag":"","commit":"{id}"}}"#)).is_err());
        assert!(parse(&format!(r#"{{"tag":"a b","commit":"{id}"}}"#)).is_err());
        assert!(parse("not json").is_err());
        assert_eq!(check(" beta-1.5 ", &id.to_uppercase()).unwrap(), ("beta-1.5".to_string(), id.to_string()));
        assert!(check("beta-1.5", "").is_err());
    }
}
