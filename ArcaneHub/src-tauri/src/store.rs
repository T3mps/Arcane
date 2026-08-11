// JSON file persistence for the Hub's per-user state.
//
// One place, shared by state.rs and settings.rs, which previously each
// hand-rolled the same read-or-default + create_dir_all + write sequence. Two
// copies meant two chances to get durability wrong, and both had it wrong the
// same way.
//
// Three rules this enforces that the hand-rolled versions did not:
//
// 1. A write is ATOMIC. Writing in place leaves a truncated file if the process
//    dies mid-write, and a truncated file is indistinguishable from a corrupt
//    one on the next read.
// 2. A file that EXISTS but does not parse is PRESERVED, not silently replaced.
//    The old `read_to_string().ok().and_then(parse).unwrap_or_default()` chain
//    turned one bad byte into "the user has no projects", and the next save
//    then overwrote the evidence. Recovering by hand is only possible if the
//    bytes still exist.
// 3. Every file carries a FORMAT VERSION (the `{version, items}` envelope), so
//    a future breaking shape change has a seam to branch on -- and a file from
//    a NEWER Hub is recognised and left alone rather than quarantined as
//    corrupt. serde(default) still covers additive drift without a bump.

use serde::{de::DeserializeOwned, Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// The on-disk format this build writes and the newest it understands.
///
/// Bump it ONLY with a breaking shape change, alongside the migration that
/// reads the old shape -- additive fields ride serde(default) and need no
/// bump. Version 1 is the `{version, items}` envelope introduced 2026-07-28.
/// (The bare pre-envelope shape was readable for a few hours that day; that
/// fallback was removed once this machine's files -- the only machine's --
/// were confirmed on the envelope.)
const STATE_FORMAT_VERSION: u32 = 1;

/// The envelope every state file is written inside. Store-level on purpose:
/// the version describes the FILE format, not any one payload type, and the
/// IPC surface never sees it -- `read_or_default` unwraps to plain `T`.
#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Versioned<T> {
    version: u32,
    items: T,
}

/// Where a corrupt file gets moved so its contents survive the recovery.
fn quarantine_path(p: &Path) -> PathBuf {
    let mut name = p.file_name().unwrap_or_default().to_os_string();
    name.push(".corrupt");
    p.with_file_name(name)
}

/// Read and parse `p`, falling back to `T::default()`.
///
/// A missing file is normal (first run) and reports nothing. A file that exists
/// but cannot be read or parsed is moved aside to `<name>.corrupt` and reported
/// through `warnings`, so the caller can tell the user instead of leaving them
/// to discover an empty list on their own.
///
/// Two shapes are understood: the current `{version, items}` envelope, and a
/// TOO-NEW envelope (left untouched on disk -- it belongs to a newer Hub --
/// with a warning that this build runs on defaults). Anything else quarantines.
pub fn read_or_default<T: Default + DeserializeOwned>(p: &Path, warnings: &mut Vec<String>) -> T {
    let text = match std::fs::read_to_string(p) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return T::default(),
        Err(e) => {
            warnings.push(format!("Could not read {}: {e}", p.display()));
            return T::default();
        }
    };

    // Version check on the raw document BEFORE typed parsing: a newer format's
    // `items` may not parse under this build's types at all, and that must
    // read as "newer Hub wrote this", never as corruption to quarantine.
    if let Ok(doc) = serde_json::from_str::<serde_json::Value>(&text) {
        if let Some(v) = doc.get("version").and_then(serde_json::Value::as_u64) {
            if v as u32 > STATE_FORMAT_VERSION {
                warnings.push(format!(
                    "{} was written by a newer Hub (format {v}, this build reads \
                     up to {STATE_FORMAT_VERSION}). Running with defaults; the \
                     file is untouched.",
                    p.display()
                ));
                return T::default();
            }
        }
    }

    match serde_json::from_str::<Versioned<T>>(&text) {
        Ok(v) => v.items,
        Err(e) => {
            let kept = quarantine_path(p);
            let note = match std::fs::rename(p, &kept) {
                Ok(()) => format!("kept a copy at {}", kept.display()),
                // Report the failure rather than claiming a copy exists.
                Err(re) => format!("could not set the file aside: {re}"),
            };
            warnings.push(format!(
                "{} was unreadable ({e}) and has been reset — {note}.",
                p.display()
            ));
            T::default()
        }
    }
}

/// Serialise `value` into `p` atomically: write a sibling temp file, then
/// rename over the target. `fs::rename` replaces an existing file on Windows,
/// so the target is either the old contents or the new ones, never a partial.
pub fn write_atomic<T: Serialize>(p: &Path, value: &T) -> Result<(), String> {
    let dir = p.parent().ok_or_else(|| format!("no parent directory for {}", p.display()))?;
    std::fs::create_dir_all(dir).map_err(|e| format!("create {}: {e}", dir.display()))?;

    let text = serde_json::to_string_pretty(&Versioned {
        version: STATE_FORMAT_VERSION,
        items: value,
    })
    .map_err(|e| e.to_string())?;

    // PID in the temp name so two Hub instances cannot write the same scratch
    // file. Sharing it would let one process rename away the other's
    // half-written temp -- no corruption thanks to the rename, but one of them
    // would silently publish the other's data.
    let mut tmp_name = p.file_name().unwrap_or_default().to_os_string();
    tmp_name.push(format!(".{}.tmp", std::process::id()));
    let tmp = p.with_file_name(tmp_name);

    std::fs::write(&tmp, text).map_err(|e| format!("write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, p).map_err(|e| {
        // Leave no stray temp file behind on a failed rename.
        let _ = std::fs::remove_file(&tmp);
        format!("replace {}: {e}", p.display())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde::Deserialize;

    #[derive(Debug, Default, PartialEq, Serialize, Deserialize)]
    struct Sample {
        #[serde(default)]
        items: Vec<String>,
    }

    // Each test gets its own directory so they can run in parallel. std has no
    // tempdir, and a single shared path would make these order-dependent.
    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-store-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn a_missing_file_defaults_without_warning() {
        let dir = scratch("missing");
        let mut w = Vec::new();
        let v: Sample = read_or_default(&dir.join("nope.json"), &mut w);
        assert_eq!(v, Sample::default());
        assert!(w.is_empty(), "first run is normal, not a warning");
    }

    #[test]
    fn write_then_read_round_trips() {
        let dir = scratch("roundtrip");
        let p = dir.join("s.json");
        let v = Sample { items: vec!["a".into()] };
        write_atomic(&p, &v).unwrap();
        let mut w = Vec::new();
        assert_eq!(read_or_default::<Sample>(&p, &mut w), v);
        assert!(w.is_empty());
    }

    #[test]
    fn write_creates_missing_parent_directories() {
        let dir = scratch("mkdir");
        let p = dir.join("nested").join("deeper").join("s.json");
        write_atomic(&p, &Sample::default()).unwrap();
        assert!(p.is_file());
    }

    #[test]
    fn write_leaves_no_temp_file_behind() {
        let dir = scratch("notmp");
        let p = dir.join("s.json");
        write_atomic(&p, &Sample::default()).unwrap();
        let strays: Vec<_> = std::fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.ends_with(".tmp"))
            .collect();
        assert!(strays.is_empty(), "left behind: {strays:?}");
    }

    #[test]
    fn write_replaces_existing_contents() {
        let dir = scratch("replace");
        let p = dir.join("s.json");
        write_atomic(&p, &Sample { items: vec!["old".into()] }).unwrap();
        write_atomic(&p, &Sample { items: vec!["new".into()] }).unwrap();
        let mut w = Vec::new();
        assert_eq!(read_or_default::<Sample>(&p, &mut w).items, vec!["new".to_string()]);
    }

    #[test]
    fn a_file_from_a_newer_hub_is_left_alone_and_reported() {
        // A downgrade scenario: the file belongs to the newer build, so it is
        // neither quarantined nor parsed -- defaults plus a warning, and the
        // bytes stay exactly where the newer Hub put them.
        let dir = scratch("toonew");
        let p = dir.join("s.json");
        let future = r#"{"version":99,"items":{"shape":"unknowable"}}"#;
        std::fs::write(&p, future).unwrap();

        let mut w = Vec::new();
        let v: Sample = read_or_default(&p, &mut w);
        assert_eq!(v, Sample::default());
        assert_eq!(w.len(), 1);
        assert!(w[0].contains("newer Hub"), "unexpected warning: {}", w[0]);
        assert_eq!(std::fs::read_to_string(&p).unwrap(), future, "file untouched");
        assert!(!quarantine_path(&p).exists(), "and never quarantined");
    }

    #[test]
    fn a_corrupt_file_is_preserved_not_destroyed() {
        // THE data-loss regression: the previous chain defaulted silently and
        // the next save overwrote the only copy of the user's list.
        let dir = scratch("corrupt");
        let p = dir.join("s.json");
        std::fs::write(&p, "{ this is not json").unwrap();

        let mut w = Vec::new();
        let v: Sample = read_or_default(&p, &mut w);
        assert_eq!(v, Sample::default(), "must still start up");
        assert_eq!(w.len(), 1, "and must say so");

        let kept = quarantine_path(&p);
        assert!(kept.is_file(), "the original bytes must survive");
        assert_eq!(std::fs::read_to_string(&kept).unwrap(), "{ this is not json");
        assert!(!p.exists(), "the bad file is moved, not copied");
    }

    #[test]
    fn a_corrupt_file_recovers_on_the_next_write() {
        let dir = scratch("recover");
        let p = dir.join("s.json");
        std::fs::write(&p, "garbage").unwrap();

        let mut w = Vec::new();
        let _: Sample = read_or_default(&p, &mut w);
        write_atomic(&p, &Sample { items: vec!["fresh".into()] }).unwrap();

        let mut w2 = Vec::new();
        assert_eq!(read_or_default::<Sample>(&p, &mut w2).items, vec!["fresh".to_string()]);
        assert!(w2.is_empty(), "the replacement must be clean");
    }

    #[test]
    fn quarantine_sits_beside_the_original() {
        let q = quarantine_path(Path::new("C:/x/recents.archub"));
        assert_eq!(q, PathBuf::from("C:/x/recents.archub.corrupt"));
    }
}
