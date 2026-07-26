// Hub settings: user preferences the Hub actually acts on.
//
// Deliberately NOT a kitchen sink. Every field here changes observable
// behaviour at a named call site, listed beside it. A preference nothing reads
// is worse than no preference at all -- it tells the user they configured
// something when they did not.
//
// Same durability contract as state.rs: any read failure falls back to
// Default. A hand-edited or truncated settings file must never brick the Hub,
// and settings are by definition the least important thing to preserve.

use serde::{Deserialize, Serialize};

use crate::{paths, store};

pub const VIEW_GRID: &str = "grid";
pub const VIEW_LIST: &str = "list";

/// Normalise the project-list layout, defaulting anything unrecognised to the
/// grid.
///
/// Deliberately a String field with a normaliser rather than a serde enum: an
/// unknown variant makes serde fail the WHOLE document, and since a settings
/// file that will not parse is treated as corrupt, one bad value here would
/// discard every other setting with it.
pub fn clean_view(v: &str) -> String {
    if v.trim().eq_ignore_ascii_case(VIEW_LIST) {
        VIEW_LIST.to_string()
    } else {
        VIEW_GRID.to_string()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Settings {
    /// Starting directory for the New Project and Open Project dialogs.
    /// Empty = let the OS pick (its own last-used location).
    #[serde(default)]
    pub default_project_dir: String,

    /// Close the Hub once a project launches. Launchers genuinely differ on
    /// this -- Steam stays, most IDE launchers exit -- so it is a setting
    /// rather than a decision made for the user. Read in `launch()`.
    #[serde(default)]
    pub close_after_launch: bool,

    /// Project list layout: `grid` or `list`. Set from the toggle above the
    /// list rather than from the Settings tab -- it is a view control, and
    /// making the user leave the projects screen to change how the projects
    /// screen looks would be worse. Read in ProjectsView. Always normalised
    /// through `clean_view`, so consumers can trust the value.
    ///
    /// `default = "default_view"`, not a bare `#[serde(default)]`: the bare
    /// form uses `String::default()` ("") and would disagree with this struct's
    /// own Default, leaving two answers to "what does a fresh install show".
    #[serde(default = "default_view")]
    pub project_view: String,
}

fn default_view() -> String {
    VIEW_GRID.to_string()
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            default_project_dir: String::new(),
            close_after_launch: false,
            project_view: default_view(),
        }
    }
}

/// Trim a user-typed directory into the form the dialog layer wants.
///
/// Pure so it can be tested without a filesystem. Existence is checked
/// separately at the command boundary: a configured folder that has since been
/// deleted must degrade to "let the OS choose", not to an error dialog.
pub fn clean_dir(dir: &str) -> String {
    let t = dir.trim().trim_matches('"');
    // Keep a bare root ("C:\", "/") intact -- trimming its separator would turn
    // it into "C:", which Windows reads as the drive's *current* directory.
    let trimmed = t.trim_end_matches(['/', '\\']);
    if trimmed.is_empty() || trimmed.ends_with(':') {
        t.to_string()
    } else {
        trimmed.to_string()
    }
}

/// Settings discard their warnings: unlike the project list, a reset settings
/// file loses only preferences the user can see and re-set on the same screen.
pub fn load() -> Settings {
    let mut ignored = Vec::new();
    let s: Settings = store::read_or_default(&paths::settings_file(), &mut ignored);
    // Normalise on the way OUT as well as in: a file written before this field
    // existed carries "" for it, and a hand-edited one may carry anything.
    // Consumers must never have to defend against a bad layout string.
    Settings { project_view: clean_view(&s.project_view), ..s }
}

pub fn save(s: &Settings) -> Result<(), String> {
    store::write_atomic(&paths::settings_file(), s)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_the_pre_settings_behaviour() {
        // Adding settings must not change what an existing user sees on the
        // launch after the update.
        let s = Settings::default();
        assert_eq!(s.default_project_dir, "");
        assert!(!s.close_after_launch, "the Hub stayed open before settings existed");
        assert_eq!(s.project_view, VIEW_GRID, "the grid is what shipped first");
    }

    #[test]
    fn clean_view_accepts_the_two_layouts_and_defaults_the_rest() {
        assert_eq!(clean_view(VIEW_LIST), VIEW_LIST);
        assert_eq!(clean_view(VIEW_GRID), VIEW_GRID);
        assert_eq!(clean_view("  List  "), VIEW_LIST, "trimmed and case-folded");
        assert_eq!(clean_view("carousel"), VIEW_GRID, "unknown falls back");
        assert_eq!(clean_view(""), VIEW_GRID, "so does a file written before the field");
    }

    #[test]
    fn an_unknown_layout_does_not_discard_the_other_settings() {
        // THE reason this is a String and not a serde enum: an unknown variant
        // would fail the whole document, and a settings file that will not
        // parse is treated as corrupt and reset.
        let back: Settings =
            serde_json::from_str(r#"{"closeAfterLaunch":true,"projectView":"nonsense"}"#)
                .expect("must still parse");
        assert!(back.close_after_launch, "the other settings survive");
        assert_eq!(clean_view(&back.project_view), VIEW_GRID);
    }

    #[test]
    fn settings_round_trip_through_json() {
        let s = Settings {
            default_project_dir: "D:/Games".to_string(),
            close_after_launch: true,
            project_view: VIEW_LIST.to_string(),
        };
        let back: Settings = serde_json::from_str(&serde_json::to_string(&s).unwrap()).unwrap();
        assert_eq!(back, s);
    }

    #[test]
    fn serialises_with_camel_case_keys_for_the_frontend() {
        let text = serde_json::to_string(&Settings::default()).unwrap();
        assert!(text.contains("defaultProjectDir"), "got {text}");
        assert!(text.contains("closeAfterLaunch"), "got {text}");
    }

    #[test]
    fn a_settings_file_missing_a_field_still_loads() {
        // Forward compat: an older file must not reset the fields it does have.
        let back: Settings = serde_json::from_str(r#"{"closeAfterLaunch":true}"#).unwrap();
        assert!(back.close_after_launch);
        assert_eq!(back.default_project_dir, "");
    }

    #[test]
    fn an_unknown_field_is_ignored_rather_than_fatal() {
        // Downgrade compat: a newer Hub's file must still load in an older one.
        let back: Settings = serde_json::from_str(r#"{"somethingNew":1}"#).unwrap();
        assert_eq!(back, Settings::default());
    }

    #[test]
    fn corrupt_settings_text_does_not_parse_so_the_caller_falls_back() {
        assert!(serde_json::from_str::<Settings>("{ not json").is_err());
    }

    #[test]
    fn clean_dir_trims_whitespace_and_trailing_separators() {
        assert_eq!(clean_dir("  D:/Games/  "), "D:/Games");
        assert_eq!(clean_dir("D:\\Games\\"), "D:\\Games");
        assert_eq!(clean_dir("D:/Games"), "D:/Games");
    }

    #[test]
    fn clean_dir_strips_the_quotes_explorer_copy_as_path_adds() {
        // "Copy as path" in Explorer yields a quoted string; pasting it into
        // the field is the obvious way to fill it in.
        assert_eq!(clean_dir("\"D:\\Games\""), "D:\\Games");
    }

    #[test]
    fn clean_dir_keeps_a_bare_drive_root_usable() {
        // "D:\" -> "D:" would mean the drive's CURRENT directory on Windows,
        // which is a different folder than the user picked.
        assert_eq!(clean_dir("D:\\"), "D:\\");
        assert_eq!(clean_dir("/"), "/");
    }

    #[test]
    fn clean_dir_maps_blank_input_to_empty() {
        assert_eq!(clean_dir("   "), "");
        assert_eq!(clean_dir(""), "");
    }
}
