// Hub state: the recent-projects list and the registered-engines list.
//
// Everything here is PURE and unit-tested. Loading/saving touches the
// filesystem but falls back to Default on any error -- a hand-edited or
// truncated state file must never brick the Hub.

use serde::{Deserialize, Serialize};

use crate::{paths, store};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct RecentProject {
    pub path: String,
    pub name: String,
    pub last_opened_utc: String,
    pub engine_abi: u32,

    /// Which registered engine opens THIS project. `None` = follow the Hub's
    /// default (the sidebar selection).
    ///
    /// Hub state, deliberately NOT the .arcproj. This is an `EngineEntry::id`,
    /// which is a normalised absolute exe path -- machine-specific by nature.
    /// Unreal reaches the same conclusion from the other direction: its
    /// `EngineAssociation` does live in the .uproject, but only ever as a
    /// portable version string or a random GUID resolved through the local
    /// registry, and DesktopPlatformBase.cpp:444 blanks it entirely when saving
    /// a non-foreign project "to allow portability between source control
    /// databases". A raw local path in shared project content is the one thing
    /// that design avoids.
    #[serde(default)]
    pub engine_id: Option<String>,

    /// Extra command-line arguments appended after `--project <path>`, as the
    /// user typed them. Empty = launch with no extras.
    ///
    /// Stored RAW rather than pre-split: what the user typed is what the field
    /// must show them next time, and a re-joined token list is not that. It is
    /// split at launch, by `project::split_args`.
    ///
    /// Hub state, not the .arcproj, for the same reason as `engine_id`: these
    /// are one person's local switches (a backend override, a scene to boot
    /// into), not a property of the project that belongs in shared content.
    #[serde(default)]
    pub args: String,

    /// True when `path` no longer resolves on disk -- the row renders greyed
    /// with Locate/Remove instead of vanishing (a missing project is usually a
    /// moved folder, not an abandoned one). Stamped by `load` on EVERY read,
    /// so whatever value the file holds is overwritten before the UI sees it;
    /// it rides the persisted struct only because save/load and the
    /// `load_state` IPC response share one Serialize impl (same constraint as
    /// `HubState::warnings`).
    #[serde(default)]
    pub missing: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct EngineEntry {
    /// Stable key for the frontend and for per-project pins. Always
    /// `normalise_path(path)` -- construct through `EngineEntry::new` so the
    /// two cannot drift apart.
    pub id: String,
    pub path: String,
    pub engine_abi: u32,
    pub build: String,

    /// True when `path` no longer resolves on disk. Same rule as
    /// `RecentProject::missing`: stamped by `load` on EVERY read, never
    /// trusted from the file. A missing engine keeps its row (registration is
    /// user intent; a clean rebuild restores it in place) but the UI says so,
    /// and the pre-launch probe already refuses it honestly.
    #[serde(default)]
    pub missing: bool,
}

impl EngineEntry {
    /// The ONE place `id` is derived from `path`. It was previously computed at
    /// the call site, leaving two fields that had to agree by convention.
    pub fn new(exe_path: &str, engine_abi: u32, build: String) -> Self {
        Self {
            id: normalise_path(exe_path),
            path: exe_path.to_string(),
            engine_abi,
            build,
            missing: false,
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct HubState {
    #[serde(default)]
    pub recents: Vec<RecentProject>,
    #[serde(default)]
    pub engines: Vec<EngineEntry>,

    /// Problems found while loading, for the UI to show. Populated by `load`.
    ///
    /// Exists because recovering from a corrupt state file used to be silent:
    /// the user simply found an empty project list and no explanation.
    ///
    /// MUST stay serializable. `save` writes `recents` and `engines`
    /// individually, so this struct's Serialize impl has exactly one consumer:
    /// the `load_state` IPC response. A `skip_serializing` here (which this
    /// briefly had, reasoning "it is never persisted") removes the field from
    /// the only place it was ever meant to appear, and the frontend then sees
    /// `undefined` where it expects an array.
    #[serde(default)]
    pub warnings: Vec<String>,
}

// Compare-only normalisation. Windows paths are case-insensitive and accept
// either separator, so the SAME project reached two ways must collapse to one
// entry. The ORIGINAL string is what gets stored and displayed.
pub fn normalise_path(p: &str) -> String {
    p.replace('\\', "/")
        .trim_end_matches('/')
        .to_lowercase()
}

// Insert-or-move-to-front. Re-opening a project must refresh it, not duplicate
// it, so the list stays newest-first without growing.
pub fn touch_recent(v: &mut Vec<RecentProject>, mut entry: RecentProject) {
    let key = normalise_path(&entry.path);

    // Carry the per-project engine pin across the re-insert. open_project builds
    // a fresh entry from the launch and has no reason to know about the pin, so
    // without this every launch would silently reset the project to the default.
    // Same for the saved command-line arguments. Clearing them is done through
    // set_project_args, which edits the entry in place, so an empty string
    // arriving HERE always means "the caller had nothing to say", never
    // "the user emptied the field".
    if entry.engine_id.is_none() || entry.args.is_empty() {
        if let Some(prev) = v.iter().find(|e| normalise_path(&e.path) == key) {
            if entry.engine_id.is_none() {
                entry.engine_id.clone_from(&prev.engine_id);
            }
            if entry.args.is_empty() {
                entry.args.clone_from(&prev.args);
            }
        }
    }

    v.retain(|e| normalise_path(&e.path) != key);
    v.insert(0, entry);
}

/// Save the extra launch arguments for one project. False if it is not listed.
pub fn set_project_args(v: &mut [RecentProject], path: &str, args: &str) -> bool {
    let key = normalise_path(path);
    match v.iter_mut().find(|e| normalise_path(&e.path) == key) {
        Some(e) => {
            e.args = args.to_string();
            true
        }
        None => false,
    }
}

/// Repoint a listed project at the path the user just located it at.
///
/// Same in-place contract as `rename_recent` below (the pin, the arguments and
/// last-opened survive; the list is not reordered; a collision with an entry
/// already at the new path is absorbed) -- but the name and ABI are REPLACED
/// from the manifest the caller just read, because the project may have been
/// renamed or rebuilt while the Hub could not see it.
pub fn relocate_recent(
    v: &mut Vec<RecentProject>,
    old_path: &str,
    new_path: &str,
    new_name: &str,
    new_abi: u32,
) -> bool {
    let old_key = normalise_path(old_path);
    let new_key = normalise_path(new_path);

    let Some(i) = v.iter().position(|e| normalise_path(&e.path) == old_key) else {
        return false;
    };
    v[i].path = new_path.to_string();
    v[i].name = new_name.to_string();
    v[i].engine_abi = new_abi;
    v[i].missing = false;

    // Retain by index so the entry just edited survives its own key check.
    let mut n = 0;
    v.retain(|e| {
        let keep = n == i || normalise_path(&e.path) != new_key;
        n += 1;
        keep
    });
    true
}

/// Repoint a listed project at its new path and name after a rename.
///
/// Edits IN PLACE rather than remove-then-touch: everything else on the entry
/// (the engine pin, the saved arguments, the ABI, when it was last opened) has
/// to survive, and a rename is not an open, so it must not jump to the top of
/// the list either. Any other entry that would now collide with the new path is
/// dropped, so a rename can never mint a duplicate.
pub fn rename_recent(v: &mut Vec<RecentProject>, old_path: &str, new_path: &str, new_name: &str) -> bool {
    let old_key = normalise_path(old_path);
    let new_key = normalise_path(new_path);

    let Some(i) = v.iter().position(|e| normalise_path(&e.path) == old_key) else {
        return false;
    };
    v[i].path = new_path.to_string();
    v[i].name = new_name.to_string();

    // Retain by index so the entry just edited survives its own key check.
    let mut n = 0;
    v.retain(|e| {
        let keep = n == i || normalise_path(&e.path) != new_key;
        n += 1;
        keep
    });
    true
}

// Pin a project to a registered engine, or clear the pin with `None` to send it
// back to following the Hub default. Returns false if the project is not listed.
pub fn set_project_engine(v: &mut [RecentProject], path: &str, engine_id: Option<String>) -> bool {
    let key = normalise_path(path);
    match v.iter_mut().find(|e| normalise_path(&e.path) == key) {
        Some(e) => {
            e.engine_id = engine_id;
            true
        }
        None => false,
    }
}

// Forgetting an engine must not leave projects pinned to it. The pin is cleared
// rather than repointed: which engine should replace it is the user's call, and
// falling back to the default is the honest answer until they make it.
pub fn unpin_engine(v: &mut [RecentProject], engine_id: &str) {
    for e in v.iter_mut() {
        if e.engine_id.as_deref() == Some(engine_id) {
            e.engine_id = None;
        }
    }
}

pub fn remove_recent(v: &mut Vec<RecentProject>, path: &str) -> bool {
    let key = normalise_path(path);
    let before = v.len();
    v.retain(|e| normalise_path(&e.path) != key);
    v.len() != before
}

// Re-registering the same engine refreshes its probed data rather than
// appending a duplicate row.
pub fn upsert_engine(v: &mut Vec<EngineEntry>, entry: EngineEntry) {
    let key = normalise_path(&entry.path);
    v.retain(|e| normalise_path(&e.path) != key);
    v.push(entry);
}

pub fn remove_engine(v: &mut Vec<EngineEntry>, path: &str) -> bool {
    let key = normalise_path(path);
    let before = v.len();
    v.retain(|e| normalise_path(&e.path) != key);
    v.len() != before
}

pub fn load() -> HubState {
    let mut warnings = Vec::new();
    let mut recents: Vec<RecentProject> =
        store::read_or_default(&paths::recents_file(), &mut warnings);
    // Stamped on every load, never trusted from the file: `missing` is a fact
    // about the disk right now, and a stale persisted value in either
    // direction would grey a healthy row or offer Launch on a gone one.
    for e in recents.iter_mut() {
        e.missing = !std::path::Path::new(&e.path).exists();
    }
    let mut engines: Vec<EngineEntry> =
        store::read_or_default(&paths::engines_file(), &mut warnings);
    for e in engines.iter_mut() {
        e.missing = !std::path::Path::new(&e.path).exists();
    }
    HubState { recents, engines, warnings }
}

/// The disk-truth fingerprint the background watcher compares between polls:
/// every listed path paired with whether it is missing right now. Membership
/// changes flip it too, which is fine -- a redundant refresh after a command
/// is idempotent, while a MISSED transition is exactly the stale-row bug the
/// watcher exists to prevent (a project moved mid-session looked healthy
/// until some other action reloaded the list).
pub fn disk_fingerprint(s: &HubState) -> Vec<(String, bool)> {
    s.recents
        .iter()
        .map(|e| (e.path.clone(), e.missing))
        .chain(s.engines.iter().map(|e| (e.path.clone(), e.missing)))
        .collect()
}

pub fn save(s: &HubState) -> Result<(), String> {
    // Engines FIRST. The two files are written separately, so a failure between
    // them leaves a mismatch; ordering it this way means the surviving mismatch
    // is "an engine exists that nothing points at yet" rather than "a project
    // is pinned to an engine that was never recorded".
    store::write_atomic(&paths::engines_file(), &s.engines)?;
    store::write_atomic(&paths::recents_file(), &s.recents)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rp(path: &str, when: &str) -> RecentProject {
        RecentProject {
            path: path.to_string(),
            name: "N".to_string(),
            last_opened_utc: when.to_string(),
            engine_abi: 7,
            engine_id: None,
            args: String::new(),
            missing: false,
        }
    }

    fn ee(path: &str, abi: u32, build: &str) -> EngineEntry {
        EngineEntry::new(path, abi, build.to_string())
    }

    #[test]
    fn engine_entry_derives_its_id_from_its_path() {
        // The invariant the whole pin mechanism rests on: a pin stores an id,
        // and forget_engine looks one up by normalising a path.
        let e = EngineEntry::new("C:\\Eng\\ArcaneEditor.exe", 7, "b".into());
        assert_eq!(e.id, normalise_path(&e.path));
        assert_eq!(e.path, "C:\\Eng\\ArcaneEditor.exe", "the original spelling is kept");
    }

    #[test]
    fn two_spellings_of_one_engine_share_an_id() {
        let a = EngineEntry::new("C:/Eng/ArcaneEditor.exe", 7, "b".into());
        let b = EngineEntry::new("c:\\eng\\arcaneeditor.exe", 7, "b".into());
        assert_eq!(a.id, b.id);
    }

    #[test]
    fn touch_recent_inserts_newest_first() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].path, "C:/b");
    }

    #[test]
    fn touch_recent_moves_existing_to_front_without_duplicating() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        touch_recent(&mut v, rp("C:/a", "3"));
        assert_eq!(v.len(), 2, "re-opening must not duplicate the entry");
        assert_eq!(v[0].path, "C:/a");
        assert_eq!(v[0].last_opened_utc, "3", "timestamp must refresh");
    }

    #[test]
    fn touch_recent_dedupes_case_and_separator_variants() {
        // Windows: the SAME project reached two ways must be one entry.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Dev/MyGame", "1"));
        touch_recent(&mut v, rp("c:\\dev\\mygame", "2"));
        assert_eq!(v.len(), 1);
    }

    #[test]
    fn touch_recent_keeps_the_original_spelling_for_display() {
        // Normalisation is for COMPARISON only; what we store is what we show.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:\\Dev\\MyGame", "1"));
        assert_eq!(v[0].path, "C:\\Dev\\MyGame");
    }

    #[test]
    fn touch_recent_preserves_a_per_project_engine_pin() {
        // THE regression this guards: open_project rebuilds the entry from the
        // launch, so without carry-over every launch would reset the pin and
        // the setting would appear to not stick.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_engine(&mut v, "C:/a", Some("eng-1".into())));
        touch_recent(&mut v, rp("C:/a", "2"));
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"));
        assert_eq!(v[0].last_opened_utc, "2", "the rest of the entry still refreshes");
    }

    #[test]
    fn touch_recent_lets_an_explicit_pin_win_over_the_carried_one() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        set_project_engine(&mut v, "C:/a", Some("old".into()));
        let mut next = rp("C:/a", "2");
        next.engine_id = Some("new".into());
        touch_recent(&mut v, next);
        assert_eq!(v[0].engine_id.as_deref(), Some("new"));
    }

    #[test]
    fn set_project_engine_clears_with_none_and_reports_unknown_paths() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_engine(&mut v, "C:/a", Some("eng-1".into())));
        assert!(set_project_engine(&mut v, "C:/a", None), "clearing is still a hit");
        assert_eq!(v[0].engine_id, None);
        assert!(!set_project_engine(&mut v, "C:/nope", Some("x".into())));
    }

    #[test]
    fn set_project_engine_matches_case_and_separator_variants() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Dev/MyGame", "1"));
        assert!(set_project_engine(&mut v, "c:\\dev\\mygame", Some("eng-1".into())));
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"));
    }

    #[test]
    fn unpin_engine_clears_only_projects_using_that_engine() {
        // Forgetting an engine must not leave a project pointing at nothing.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        set_project_engine(&mut v, "C:/a", Some("gone".into()));
        set_project_engine(&mut v, "C:/b", Some("kept".into()));
        unpin_engine(&mut v, "gone");
        let a = v.iter().find(|e| e.path == "C:/a").unwrap();
        let b = v.iter().find(|e| e.path == "C:/b").unwrap();
        assert_eq!(a.engine_id, None);
        assert_eq!(b.engine_id.as_deref(), Some("kept"));
    }

    #[test]
    fn a_recents_file_written_before_pins_existed_still_loads() {
        // serde(default) on engine_id: an older file has no such key.
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/a","name":"N","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert_eq!(back[0].engine_id, None);
    }

    #[test]
    fn touch_recent_preserves_saved_launch_arguments() {
        // Same regression class as the pin: open_project rebuilds the entry, so
        // without carry-over launching a project would wipe its own arguments.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_args(&mut v, "C:/a", "--backend vulkan"));
        touch_recent(&mut v, rp("C:/a", "2"));
        assert_eq!(v[0].args, "--backend vulkan");
    }

    #[test]
    fn set_project_args_can_clear_and_reports_unknown_paths() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_args(&mut v, "C:/a", "--frames 10"));
        // Clearing has to survive the next touch_recent, which is why the
        // carry-over above is keyed on "the caller said nothing" -- not on the
        // stored value being non-empty.
        assert!(set_project_args(&mut v, "c:\\a", ""));
        assert_eq!(v[0].args, "");
        touch_recent(&mut v, rp("C:/a", "2"));
        assert_eq!(v[0].args, "", "a cleared field must stay cleared");
        assert!(!set_project_args(&mut v, "C:/nope", "x"));
    }

    #[test]
    fn a_recents_file_written_before_args_existed_still_loads() {
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/a","name":"N","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert_eq!(back[0].args, "");
    }

    #[test]
    fn rename_recent_keeps_everything_except_path_and_name() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Games/Old/Old.arcproj", "1"));
        set_project_engine(&mut v, "C:/Games/Old/Old.arcproj", Some("eng-1".into()));
        set_project_args(&mut v, "C:/Games/Old/Old.arcproj", "--frames 3");

        assert!(rename_recent(
            &mut v,
            "C:/Games/Old/Old.arcproj",
            "C:/Games/New/New.arcproj",
            "New",
        ));
        assert_eq!(v[0].path, "C:/Games/New/New.arcproj");
        assert_eq!(v[0].name, "New");
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"), "the pin must survive");
        assert_eq!(v[0].args, "--frames 3", "the arguments must survive");
        assert_eq!(v[0].engine_abi, 7);
        assert_eq!(v[0].last_opened_utc, "1", "renaming is not opening");
    }

    #[test]
    fn rename_recent_does_not_reorder_the_list() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        // b is at the front; renaming a must not promote it.
        assert!(rename_recent(&mut v, "C:/a", "C:/z", "z"));
        assert_eq!(v[0].path, "C:/b");
        assert_eq!(v[1].path, "C:/z");
    }

    #[test]
    fn rename_recent_absorbs_a_colliding_entry() {
        // Renaming onto a path already in the list must leave ONE row, not two
        // rows the user then has to reconcile by hand.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        assert!(rename_recent(&mut v, "C:/a", "c:\\b", "b"));
        assert_eq!(v.len(), 1);
        // The RENAMED entry is the survivor, spelled as the caller passed it.
        assert_eq!(v[0].path, "c:\\b");
    }

    #[test]
    fn rename_recent_reports_an_unknown_project() {
        let mut v = Vec::new();
        assert!(!rename_recent(&mut v, "C:/a", "C:/b", "b"));
    }

    #[test]
    fn relocate_recent_keeps_the_pin_and_args_but_refreshes_name_and_abi() {
        // The split that makes this NOT rename_recent: the manifest at the new
        // path is the authority on name and ABI (the project may have changed
        // while it was away), while the pin and arguments are Hub state that
        // the move cannot have touched.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Games/Old/Old.arcproj", "1"));
        set_project_engine(&mut v, "C:/Games/Old/Old.arcproj", Some("eng-1".into()));
        set_project_args(&mut v, "C:/Games/Old/Old.arcproj", "--frames 3");
        v[0].missing = true;

        assert!(relocate_recent(
            &mut v,
            "C:/Games/Old/Old.arcproj",
            "D:/Elsewhere/Old/Old.arcproj",
            "Old",
            8,
        ));
        assert_eq!(v[0].path, "D:/Elsewhere/Old/Old.arcproj");
        assert_eq!(v[0].name, "Old");
        assert_eq!(v[0].engine_abi, 8, "the ABI must come from the manifest just read");
        assert!(!v[0].missing, "a located project is not missing");
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"), "the pin must survive");
        assert_eq!(v[0].args, "--frames 3", "the arguments must survive");
        assert_eq!(v[0].last_opened_utc, "1", "locating is not opening");
    }

    #[test]
    fn disk_fingerprint_flips_when_a_project_goes_missing() {
        let mut s = HubState {
            recents: vec![rp("C:/Games/A/A.arcproj", "1")],
            ..Default::default()
        };
        let before = disk_fingerprint(&s);
        s.recents[0].missing = true;
        assert_ne!(disk_fingerprint(&s), before, "a move must change the fingerprint");
        s.recents[0].missing = false;
        assert_eq!(disk_fingerprint(&s), before, "and moving it back must restore it");
    }

    #[test]
    fn disk_fingerprint_sees_engines_as_well_as_projects() {
        let mut s = HubState {
            engines: vec![EngineEntry::new("C:/eng/ArcaneEditor.exe", 8, "Arcane 0.1".into())],
            ..Default::default()
        };
        let before = disk_fingerprint(&s);
        s.engines[0].missing = true;
        assert_ne!(disk_fingerprint(&s), before, "an engine's disappearance counts too");
    }

    #[test]
    fn relocate_recent_does_not_reorder_and_absorbs_a_collision() {
        // Locating a moved project onto a path the list ALREADY holds (the user
        // re-added it by hand before finding the stale row) must leave one row.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/gone", "1"));
        touch_recent(&mut v, rp("C:/found/G.arcproj", "2"));
        assert!(relocate_recent(&mut v, "C:/gone", "c:\\found\\G.arcproj", "G", 7));
        assert_eq!(v.len(), 1);
        // The RELOCATED entry is the survivor, spelled as the caller passed it.
        assert_eq!(v[0].path, "c:\\found\\G.arcproj");
    }

    #[test]
    fn relocate_recent_reports_an_unknown_project() {
        let mut v = Vec::new();
        assert!(!relocate_recent(&mut v, "C:/a", "C:/b", "b", 7));
    }

    #[test]
    fn an_engines_file_written_before_missing_existed_still_loads() {
        let back: Vec<EngineEntry> = serde_json::from_str(
            r#"[{"id":"c:/e.exe","path":"C:/e.exe","engineAbi":7,"build":"b"}]"#,
        )
        .unwrap();
        assert!(!back[0].missing);
    }

    #[test]
    fn a_recents_file_written_before_missing_existed_still_loads() {
        // serde(default) on missing: an older file has no such key -- and load()
        // re-stamps it from the disk anyway, so false is only the parse default.
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/a","name":"N","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert!(!back[0].missing);
    }

    #[test]
    fn remove_recent_reports_whether_it_removed() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(remove_recent(&mut v, "C:/a"));
        assert!(!remove_recent(&mut v, "C:/a"), "second remove is a no-op");
        assert!(v.is_empty());
    }

    #[test]
    fn upsert_engine_replaces_same_path_rather_than_appending() {
        let mut v = Vec::new();
        upsert_engine(&mut v, ee("C:/eng/ArcaneEditor.exe", 7, "old"));
        upsert_engine(&mut v, ee("C:/ENG/arcaneeditor.exe", 8, "new"));
        assert_eq!(v.len(), 1, "re-registering must not append");
        assert_eq!(v[0].engine_abi, 8, "re-registering must refresh probed data");
        assert_eq!(v[0].build, "new");
    }

    #[test]
    fn two_distinct_engines_coexist() {
        // A dev build and a release install side by side is the day-one case.
        let mut v = Vec::new();
        upsert_engine(&mut v, ee("C:/repo/bin/Debug/ArcaneEditor.exe", 7, "Debug"));
        upsert_engine(&mut v, ee("C:/repo/bin/Release/ArcaneEditor.exe", 7, "Release"));
        assert_eq!(v.len(), 2);
        assert!(remove_engine(&mut v, "C:/repo/bin/Debug/ArcaneEditor.exe"));
        assert_eq!(v.len(), 1);
    }

    #[test]
    fn empty_state_is_valid_not_an_error() {
        // An installed Hub with no engine registered is a NORMAL first run.
        let s = HubState::default();
        assert!(s.engines.is_empty());
        assert!(s.recents.is_empty());
    }

    #[test]
    fn hub_state_serialises_every_field_the_frontend_reads() {
        // The round-trip test below cannot catch a dropped field: it compares a
        // value whose `warnings` is empty, so a skipped field still round-trips
        // to an equal struct. This asserts the KEY reaches the wire, which is
        // what the frontend actually destructures.
        let mut s = HubState::default();
        s.warnings.push("something went wrong".to_string());
        let text = serde_json::to_string(&s).unwrap();
        let doc: serde_json::Value = serde_json::from_str(&text).unwrap();

        assert!(doc.get("recents").is_some_and(|v| v.is_array()));
        assert!(doc.get("engines").is_some_and(|v| v.is_array()));
        assert!(
            doc.get("warnings").is_some_and(|v| v.is_array()),
            "load_state is this struct's only Serialize consumer -- a missing \
             key here is `undefined` in the UI, not an empty list: {text}"
        );
        assert_eq!(doc["warnings"][0], "something went wrong");
    }

    #[test]
    fn state_round_trips_through_json() {
        let mut s = HubState::default();
        touch_recent(&mut s.recents, rp("C:/a", "1"));
        upsert_engine(&mut s.engines, ee("C:/e.exe", 7, "b"));
        let text = serde_json::to_string(&s).unwrap();
        let back: HubState = serde_json::from_str(&text).unwrap();
        assert_eq!(back, s);
    }

    #[test]
    fn corrupt_state_text_does_not_parse_so_the_caller_falls_back() {
        let back: Result<HubState, _> = serde_json::from_str::<HubState>("{ not json");
        assert!(back.is_err());
    }

    #[test]
    fn a_state_file_missing_one_list_still_loads_the_other() {
        // Forward/backward compat: serde(default) on both lists.
        let back: HubState = serde_json::from_str(r#"{"recents":[]}"#).unwrap();
        assert!(back.engines.is_empty());
    }
}
