//! Text for the user. The host sends a key and its parameters; the page shows
//! them in the launcher's language (src/locales/*.js, `t(key, params)`). Every
//! key used with `msg!` must exist in src/locales/en.js; a test checks.
//!
//! Errors from the commands are Messages too. Detail the user cannot act on
//! in their language (process output, I/O errors) travels as `err.other`
//! with the text in `detail`.

use serde::Serialize;
use std::{collections::BTreeMap, fmt};

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Message {
    pub key: &'static str,
    pub params: BTreeMap<&'static str, String>,
}

impl Message {
    pub fn new(key: &'static str) -> Self {
        Message { key, params: BTreeMap::new() }
    }

    pub fn with(mut self, name: &'static str, value: impl fmt::Display) -> Self {
        self.params.insert(name, value.to_string());
        self
    }
}

/// `msg!("err.launch.noBuild")`, `msg!("err.gameDir.missingCpk", file = name, folder = path.display())`.
macro_rules! msg {
    ($key:literal $(, $name:ident = $value:expr)* $(,)?) => {
        $crate::message::Message::new($key)$(.with(stringify!($name), $value))*
    };
}
pub(crate) use msg;

/// Untranslated detail: an I/O error, a process's output.
impl From<String> for Message {
    fn from(detail: String) -> Self {
        Message::new("err.other").with("detail", detail)
    }
}

impl From<&str> for Message {
    fn from(detail: &str) -> Self {
        detail.to_string().into()
    }
}

/// English-free rendering for logs: the key and its parameters.
impl fmt::Display for Message {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.key)?;
        for (name, value) in &self.params {
            write!(f, " {name}={value:?}")?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, path::Path};

    /// Every `msg!("key"` literal in the host's sources.
    fn keys_in(dir: &Path, keys: &mut Vec<String>) {
        for entry in fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                keys_in(&path, keys);
            } else if path.extension().is_some_and(|e| e == "rs") {
                let text = fs::read_to_string(&path).unwrap();
                for (i, _) in text.match_indices("msg!(\"") {
                    let rest = &text[i + 6..];
                    let key = &rest[..rest.find('"').unwrap()];
                    // The macro's own documentation is not a use.
                    if key != "key" {
                        keys.push(key.to_string());
                    }
                }
            }
        }
    }

    #[test]
    fn every_message_key_has_english_text() {
        let src = Path::new(env!("CARGO_MANIFEST_DIR"));
        let mut keys = vec!["err.other".to_string()];
        keys_in(&src.join("src"), &mut keys);
        assert!(keys.len() > 20, "found the msg! uses");
        let english = fs::read_to_string(src.join("../src/locales/en.js")).unwrap();
        let missing: Vec<_> = keys.iter().filter(|k| !english.contains(&format!("\"{k}\":"))).collect();
        assert!(missing.is_empty(), "keys missing from src/locales/en.js: {missing:?}");
    }

    #[test]
    fn messages_carry_their_parameters() {
        let m = msg!("err.gameDir.missingCpk", file = "Patch.cpk", folder = "/g");
        assert_eq!(m.key, "err.gameDir.missingCpk");
        assert_eq!(m.params["file"], "Patch.cpk");
        assert_eq!(serde_json::to_value(&m).unwrap()["params"]["folder"], "/g");
        assert_eq!(Message::from("boom").params["detail"], "boom");
    }
}
