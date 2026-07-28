// Where the Hub keeps its per-user state.
//
// This is HUB-OWNED: the engine never reads it. Keeping user-scope state out of
// the engine preserves the rule that Core and the runtime carry no host/user
// vocabulary.
//
// Deliberately NOT derived from current_exe(). The Hub is INSTALLED to
// %LOCALAPPDATA%\Programs\Arcane Hub\, so there is no repo root to find and no
// sibling engine to assume -- that is Tools/setup-wizard's assumption, and it
// is wrong here.

use std::path::{Path, PathBuf};

pub fn hub_dir() -> PathBuf {
    // %LOCALAPPDATA% (machine-local), NOT roaming %APPDATA% -- decided
    // 2026-07-28, reversing the original choice. Everything in these files is
    // a machine-specific absolute path (D:\ projects, engine exes), so a
    // roaming profile would deliver another machine a list where every row is
    // missing and every engine is gone. Settings ride along rather than
    // splitting the store in two. Falls back to temp only if the variable is
    // missing, which should not happen on Windows.
    let base = std::env::var("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    base.join("Arcane").join("hub")
}

/// The Hub's state-file extension. JSON inside (store.rs's versioned
/// envelope), the Arcane family name outside -- .arcproj/.arcmat/.arcsprite/
/// .archub: these are the Hub's OWN generated files, not hand-edited config,
/// and the family extension says whose they are at a glance.
pub const STATE_EXT: &str = "archub";

/// Where state lived before the 2026-07-28 move: roaming %APPDATA%. Read once
/// per launch by `migrate_legacy_state`; never written again.
pub fn legacy_hub_dir() -> PathBuf {
    let base = std::env::var("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    base.join("Arcane").join("hub")
}

/// One-time move-ins, run from tauri's setup hook before the webview can issue
/// its first load_state. Two generations are covered:
///   1. the `.json` names used before the .archub extension (same directory),
///   2. the pre-2026-07-28 roaming %APPDATA% files (always `.json` -- the
///      extension arrived after the move).
///
/// A file already at the current name is never touched, and the NEWER source
/// wins: a local .json postdates the roaming copy it was itself migrated
/// from. Sources are COPIED, never deleted -- disk is cheap, a lost project
/// list is not, and a downgraded Hub still finds the shapes it understands.
pub fn migrate_legacy_state() {
    migrate_into(&hub_dir(), &legacy_hub_dir());
}

/// The testable body of `migrate_legacy_state`: directories injected so the
/// tests run against scratch dirs instead of this machine's real profile.
pub fn migrate_into(new_dir: &Path, legacy_dir: &Path) {
    for stem in ["recents", "engines", "settings"] {
        let target = new_dir.join(format!("{stem}.{STATE_EXT}"));
        if target.exists() {
            continue;
        }
        let candidates = [
            new_dir.join(format!("{stem}.json")),
            legacy_dir.join(format!("{stem}.json")),
        ];
        for src in candidates {
            if src.is_file() {
                let _ = std::fs::create_dir_all(new_dir);
                let _ = std::fs::copy(&src, &target);
                break;
            }
        }
    }
}

pub fn recents_file() -> PathBuf {
    hub_dir().join(format!("recents.{STATE_EXT}"))
}

pub fn engines_file() -> PathBuf {
    hub_dir().join(format!("engines.{STATE_EXT}"))
}

pub fn settings_file() -> PathBuf {
    hub_dir().join(format!("settings.{STATE_EXT}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_files_live_under_the_hub_dir() {
        let dir = hub_dir();
        assert!(recents_file().starts_with(&dir));
        assert!(engines_file().starts_with(&dir));
        assert!(settings_file().starts_with(&dir));
    }

    #[test]
    fn every_state_file_is_distinct() {
        // Two of these sharing a name would have one silently overwrite the
        // other on save, and the loss would only show up on next launch.
        let all = [recents_file(), engines_file(), settings_file()];
        for (i, a) in all.iter().enumerate() {
            for b in all.iter().skip(i + 1) {
                assert_ne!(a, b);
            }
        }
    }

    #[test]
    fn hub_dir_is_namespaced_under_arcane() {
        let p = hub_dir();
        let s = p.to_string_lossy().replace('\\', "/");
        assert!(s.ends_with("Arcane/hub"), "unexpected hub dir: {s}");
    }

    #[test]
    fn every_state_file_wears_the_family_extension() {
        for f in [recents_file(), engines_file(), settings_file()] {
            assert!(
                f.extension().is_some_and(|e| e == STATE_EXT),
                "not .{STATE_EXT}: {}",
                f.display()
            );
        }
    }

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-paths-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn migrate_carries_a_roaming_json_file_into_the_new_name() {
        let root = scratch("roam");
        let (new_dir, legacy) = (root.join("new"), root.join("old"));
        std::fs::create_dir_all(&legacy).unwrap();
        std::fs::write(legacy.join("recents.json"), "[1]").unwrap();

        migrate_into(&new_dir, &legacy);
        assert_eq!(
            std::fs::read_to_string(new_dir.join("recents.archub")).unwrap(),
            "[1]"
        );
        assert!(legacy.join("recents.json").is_file(), "sources are copied, never deleted");
    }

    #[test]
    fn migrate_prefers_the_local_json_over_the_roaming_one() {
        // The local .json postdates the roaming copy it was itself migrated
        // from earlier the same day; the older file must not win.
        let root = scratch("newer");
        let (new_dir, legacy) = (root.join("new"), root.join("old"));
        std::fs::create_dir_all(&new_dir).unwrap();
        std::fs::create_dir_all(&legacy).unwrap();
        std::fs::write(new_dir.join("engines.json"), "local").unwrap();
        std::fs::write(legacy.join("engines.json"), "roaming").unwrap();

        migrate_into(&new_dir, &legacy);
        assert_eq!(
            std::fs::read_to_string(new_dir.join("engines.archub")).unwrap(),
            "local"
        );
    }

    #[test]
    fn migrate_never_touches_an_existing_current_file() {
        let root = scratch("settled");
        let (new_dir, legacy) = (root.join("new"), root.join("old"));
        std::fs::create_dir_all(&new_dir).unwrap();
        std::fs::create_dir_all(&legacy).unwrap();
        std::fs::write(new_dir.join("settings.archub"), "current").unwrap();
        std::fs::write(new_dir.join("settings.json"), "stale-local").unwrap();
        std::fs::write(legacy.join("settings.json"), "stale-roaming").unwrap();

        migrate_into(&new_dir, &legacy);
        assert_eq!(
            std::fs::read_to_string(new_dir.join("settings.archub")).unwrap(),
            "current",
            "a settled file is settled"
        );
    }
}
