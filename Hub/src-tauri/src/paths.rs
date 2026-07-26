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
    // %APPDATA% (roaming) so the project list follows the user between
    // machines that roam profiles; falls back to temp only if the variable is
    // missing, which should not happen on Windows.
    let base = std::env::var("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    base.join("Arcane").join("hub")
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
