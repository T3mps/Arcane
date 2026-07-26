// Hub state: the recent-projects list and the registered-engines list.
//
// Everything here is PURE and unit-tested. Loading/saving touches the
// filesystem but falls back to Default on any error -- a hand-edited or
// truncated state file must never brick the Hub.

use serde::{Deserialize, Serialize};

use crate::paths;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct RecentProject {
    pub path: String,
    pub name: String,
    pub last_opened_utc: String,
    pub engine_abi: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct EngineEntry {
    pub id: String,
    pub path: String,
    pub engine_abi: u32,
    pub build: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct HubState {
    #[serde(default)]
    pub recents: Vec<RecentProject>,
    #[serde(default)]
    pub engines: Vec<EngineEntry>,
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
pub fn touch_recent(v: &mut Vec<RecentProject>, entry: RecentProject) {
    let key = normalise_path(&entry.path);
    v.retain(|e| normalise_path(&e.path) != key);
    v.insert(0, entry);
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

fn read_or_default<T: Default + for<'de> Deserialize<'de>>(p: &std::path::Path) -> T {
    std::fs::read_to_string(p)
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

pub fn load() -> HubState {
    HubState {
        recents: read_or_default(&paths::recents_file()),
        engines: read_or_default(&paths::engines_file()),
    }
}

pub fn save(s: &HubState) -> Result<(), String> {
    let dir = paths::hub_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("create {}: {e}", dir.display()))?;
    let r = serde_json::to_string_pretty(&s.recents).map_err(|e| e.to_string())?;
    let g = serde_json::to_string_pretty(&s.engines).map_err(|e| e.to_string())?;
    std::fs::write(paths::recents_file(), r).map_err(|e| e.to_string())?;
    std::fs::write(paths::engines_file(), g).map_err(|e| e.to_string())?;
    Ok(())
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
        }
    }

    fn ee(path: &str, abi: u32, build: &str) -> EngineEntry {
        EngineEntry {
            id: format!("id-{abi}"),
            path: path.to_string(),
            engine_abi: abi,
            build: build.to_string(),
        }
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
