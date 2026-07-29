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

pub const BEHAVIOR_TRAY: &str = "tray";
pub const BEHAVIOR_HIDE: &str = "hide";
pub const BEHAVIOR_STAY: &str = "stay";

pub const SORT_OPENED: &str = "opened";
pub const SORT_NAME: &str = "name";
pub const SORT_ENGINE: &str = "engine";
pub const SORT_ABI: &str = "abi";

/// Normalise the project-list sort column, defaulting anything unrecognised
/// to last-opened. Same String-with-normaliser reasoning as `clean_view`.
pub fn clean_sort(v: &str) -> String {
    let t = v.trim();
    for k in [SORT_NAME, SORT_ENGINE, SORT_ABI] {
        if t.eq_ignore_ascii_case(k) {
            return k.to_string();
        }
    }
    SORT_OPENED.to_string()
}

/// Normalise the after-launch behaviour, defaulting anything unrecognised to
/// the tray. Same String-with-normaliser reasoning as `clean_view`: a serde
/// enum fails the whole document on an unknown variant, and a settings file
/// that will not parse is treated as corrupt and reset.
pub fn clean_behavior(v: &str) -> String {
    let t = v.trim();
    if t.eq_ignore_ascii_case(BEHAVIOR_HIDE) {
        BEHAVIOR_HIDE.to_string()
    } else if t.eq_ignore_ascii_case(BEHAVIOR_STAY) {
        BEHAVIOR_STAY.to_string()
    } else {
        BEHAVIOR_TRAY.to_string()
    }
}

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

    /// What the Hub window does after a successful launch. Three values:
    /// `tray` (default) -- hide into the system tray: click brings it back,
    /// right-click offers quick launch/quit, and the Hub STAYS in the tray
    /// when the last editor exits (a launcher window popping itself back up
    /// when the work closes is the Epic launcher's most-complained-about
    /// habit; the icon is the handle, so nothing needs to pop).
    /// `hide` -- vanish entirely, restore when the LAST editor exits;
    /// relaunching the Hub un-hides it early via the single-instance
    /// callback. `stay` -- keep the window open beside the editor.
    /// Replaced the hide_while_running bool 2026-07-29 when the tray
    /// arrived; no legacy read of the old key, per the no-migration rule.
    /// Read Rust-side in open_project (how the window goes away) and the
    /// editor wait thread (how it comes back). Always normalised through
    /// `clean_behavior`, so consumers can trust the value.
    #[serde(default = "default_behavior")]
    pub launch_behavior: String,

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

    /// Which column orders the project list: `opened` (default) | `name` |
    /// `engine` | `abi`. Set by clicking a column header (list layout) or the
    /// sort control (grid); favourites always sit above the rest whatever
    /// this says -- the partition is the frontend's job (format.ts
    /// sortProjects). Always normalised through `clean_sort`.
    #[serde(default = "default_sort")]
    pub project_sort: String,

    /// Sort direction, persisted separately so flipping never loses the
    /// column. `default = "yes"` because the default column is `opened` and
    /// newest-first is the only order a launcher's recents make sense in.
    #[serde(default = "yes")]
    pub project_sort_desc: bool,

    /// Ask before deleting a project. Read in `+page.svelte`'s delete handler,
    /// which skips straight to `delete_project` when this is off.
    ///
    /// ON by default, and `default = "yes"` rather than a bare
    /// `#[serde(default)]` for a reason that matters more here than anywhere
    /// else in this struct: the bare form uses `bool::default()`, which is
    /// FALSE. Every settings file written before this field existed would then
    /// load with confirmation silently disabled -- turning a menu item into a
    /// one-click delete for exactly the users who never asked for it.
    #[serde(default = "yes")]
    pub confirm_delete: bool,
}

fn default_view() -> String {
    VIEW_GRID.to_string()
}

fn default_behavior() -> String {
    BEHAVIOR_TRAY.to_string()
}

fn default_sort() -> String {
    SORT_OPENED.to_string()
}

fn yes() -> bool {
    true
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            default_project_dir: String::new(),
            launch_behavior: default_behavior(),
            project_view: default_view(),
            project_sort: default_sort(),
            project_sort_desc: yes(),
            confirm_delete: yes(),
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
    // Normalise on the way OUT as well as in: a file written before these
    // fields existed carries "" for them, and a hand-edited one may carry
    // anything. Consumers must never have to defend against a bad string.
    Settings {
        project_view: clean_view(&s.project_view),
        launch_behavior: clean_behavior(&s.launch_behavior),
        project_sort: clean_sort(&s.project_sort),
        ..s
    }
}

pub fn save(s: &Settings) -> Result<(), String> {
    store::write_atomic(&paths::settings_file(), s)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_the_designed_behaviour() {
        let s = Settings::default();
        assert_eq!(s.default_project_dir, "");
        assert_eq!(s.launch_behavior, BEHAVIOR_TRAY, "the tray is the designed default");
        assert_eq!(s.project_view, VIEW_GRID, "the grid is what shipped first");
        assert_eq!(s.project_sort, SORT_OPENED, "recents order by recency until asked otherwise");
        assert!(s.project_sort_desc, "newest first is the only sensible recents default");
        assert!(s.confirm_delete, "deleting has always asked first");
    }

    #[test]
    fn clean_sort_accepts_the_four_columns_and_defaults_the_rest() {
        assert_eq!(clean_sort(SORT_OPENED), SORT_OPENED);
        assert_eq!(clean_sort(SORT_NAME), SORT_NAME);
        assert_eq!(clean_sort(SORT_ENGINE), SORT_ENGINE);
        assert_eq!(clean_sort(SORT_ABI), SORT_ABI);
        assert_eq!(clean_sort("  Name "), SORT_NAME, "trimmed and case-folded");
        assert_eq!(clean_sort("stars"), SORT_OPENED, "unknown falls back");
        assert_eq!(clean_sort(""), SORT_OPENED, "so does a pre-field file");
    }

    #[test]
    fn a_file_written_before_confirm_delete_existed_still_confirms() {
        // THE regression this field's `default = "yes"` exists to stop. With a
        // bare #[serde(default)] this loads as false, and every user who had
        // ever saved a setting would get a one-click project delete on the next
        // launch without touching anything.
        let back: Settings = serde_json::from_str(r#"{"launchBehavior":"stay"}"#).unwrap();
        assert!(back.confirm_delete);
    }

    #[test]
    fn a_file_written_before_launch_behavior_existed_gets_the_tray() {
        // Same defaulted-field reasoning as confirm_delete: an older file must
        // land on the designed behaviour, not on an accidental empty string.
        let back: Settings = serde_json::from_str(r#"{"confirmDelete":true}"#).unwrap();
        assert_eq!(back.launch_behavior, BEHAVIOR_TRAY);
    }

    #[test]
    fn clean_behavior_accepts_the_three_modes_and_defaults_the_rest() {
        assert_eq!(clean_behavior(BEHAVIOR_TRAY), BEHAVIOR_TRAY);
        assert_eq!(clean_behavior(BEHAVIOR_HIDE), BEHAVIOR_HIDE);
        assert_eq!(clean_behavior(BEHAVIOR_STAY), BEHAVIOR_STAY);
        assert_eq!(clean_behavior("  Hide  "), BEHAVIOR_HIDE, "trimmed and case-folded");
        assert_eq!(clean_behavior("closeAfterLaunch"), BEHAVIOR_TRAY, "unknown falls back");
        assert_eq!(clean_behavior(""), BEHAVIOR_TRAY, "so does a pre-field file");
    }

    #[test]
    fn confirm_delete_can_actually_be_turned_off() {
        // The other half: an explicit false must survive the round trip, or the
        // setting would be permanently stuck on.
        let back: Settings = serde_json::from_str(r#"{"confirmDelete":false}"#).unwrap();
        assert!(!back.confirm_delete);
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
    fn an_unknown_behavior_does_not_discard_the_other_settings() {
        // THE reason these are Strings and not serde enums: an unknown variant
        // would fail the whole document, and a settings file that will not
        // parse is treated as corrupt and reset.
        let back: Settings =
            serde_json::from_str(r#"{"confirmDelete":false,"launchBehavior":"nonsense","projectView":"nonsense"}"#)
                .expect("must still parse");
        assert!(!back.confirm_delete, "the other settings survive");
        assert_eq!(clean_behavior(&back.launch_behavior), BEHAVIOR_TRAY);
        assert_eq!(clean_view(&back.project_view), VIEW_GRID);
    }

    #[test]
    fn settings_round_trip_through_json() {
        let s = Settings {
            default_project_dir: "D:/Games".to_string(),
            launch_behavior: BEHAVIOR_STAY.to_string(),
            project_view: VIEW_LIST.to_string(),
            project_sort: SORT_NAME.to_string(),
            project_sort_desc: false,
            confirm_delete: false,
        };
        let back: Settings = serde_json::from_str(&serde_json::to_string(&s).unwrap()).unwrap();
        assert_eq!(back, s);
    }

    #[test]
    fn serialises_with_camel_case_keys_for_the_frontend() {
        let text = serde_json::to_string(&Settings::default()).unwrap();
        assert!(text.contains("defaultProjectDir"), "got {text}");
        assert!(text.contains("launchBehavior"), "got {text}");
        assert!(text.contains("projectSort"), "got {text}");
        assert!(text.contains("projectSortDesc"), "got {text}");
        assert!(text.contains("confirmDelete"), "got {text}");
    }

    #[test]
    fn a_settings_file_missing_a_field_still_loads() {
        // Forward compat: an older file must not reset the fields it does have.
        let back: Settings = serde_json::from_str(r#"{"launchBehavior":"hide"}"#).unwrap();
        assert_eq!(back.launch_behavior, BEHAVIOR_HIDE);
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
