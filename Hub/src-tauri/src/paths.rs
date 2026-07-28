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

use std::path::PathBuf;

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

/// Where state lived before the 2026-07-28 move: roaming %APPDATA%. Read once
/// per launch by `migrate_legacy_state`; never written again.
pub fn legacy_hub_dir() -> PathBuf {
    let base = std::env::var("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    base.join("Arcane").join("hub")
}

/// One-time move-in: each state file that exists at the OLD roaming location
/// but not the new one is copied over. Old files stay in place as a fallback
/// copy -- disk is cheap, a lost project list is not. Runs from tauri's setup
/// hook, before the webview can issue its first load_state.
pub fn migrate_legacy_state() {
    let old_dir = legacy_hub_dir();
    let new_dir = hub_dir();
    // Equal on a machine with unusual env; nothing to move then.
    if old_dir == new_dir || !old_dir.is_dir() {
        return;
    }
    for f in ["recents.json", "engines.json", "settings.json"] {
        let old = old_dir.join(f);
        let new = new_dir.join(f);
        if old.is_file() && !new.exists() {
            let _ = std::fs::create_dir_all(&new_dir);
            let _ = std::fs::copy(&old, &new);
        }
    }
}

pub fn recents_file() -> PathBuf {
    hub_dir().join("recents.json")
}

pub fn engines_file() -> PathBuf {
    hub_dir().join("engines.json")
}

pub fn settings_file() -> PathBuf {
    hub_dir().join("settings.json")
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
}
