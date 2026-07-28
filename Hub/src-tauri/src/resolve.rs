// The disk-resolution half of the Hub: turning a RECORDED project path into
// the (root, manifest) pair the commands act on, plus the manifest's ABI and
// the folder a project lives in. IO over project.rs's pure rules -- split out
// of lib.rs alongside spawn.rs so the command file stays an IPC surface.
// project.rs itself stays fs-free and unit-tested; the functions here are the
// ones that touch the live disk, which is why they live apart.

use std::path::{Path, PathBuf};

use crate::project;

/// The ABI a project was BUILT AGAINST, read from its manifest.
///
/// This is the number `Runtime::OpenProject` (Runtime.cpp:387) compares against
/// the engine's own constant and refuses on mismatch, so it is the only honest
/// input to the Hub's compatibility display. None = unknown (no manifest,
/// ambiguous folder, unreadable JSON), which the UI treats as "cannot prove a
/// conflict" rather than as a fault.
pub fn manifest_abi(project_path: &Path) -> Option<u32> {
    let file = if project_path.extension().is_some_and(|e| {
        e.eq_ignore_ascii_case(project::MANIFEST_EXT)
    }) {
        project_path.to_path_buf()
    } else {
        let names: Vec<String> = std::fs::read_dir(project_path)
            .ok()?
            .filter_map(|e| e.ok())
            .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
            .map(|e| e.file_name().to_string_lossy().to_string())
            .collect();
        project_path.join(project::pick_manifest(&names)?)
    };
    project::parse_manifest_abi(&std::fs::read_to_string(file).ok()?)
}

/// The FOLDER a project lives in: the parent of its .arcproj, or the recorded
/// path itself when the entry is folder-shaped (how opens were recorded before
/// the dialog asked for a manifest). Paired with `projectDir` in
/// src/lib/format.ts, which does the same job for display.
pub fn project_dir(p: &Path) -> PathBuf {
    if p.extension().is_some_and(|e| e.eq_ignore_ascii_case(project::MANIFEST_EXT)) {
        return p.parent().map(Path::to_path_buf).unwrap_or_else(|| p.to_path_buf());
    }
    p.to_path_buf()
}

/// A recorded path resolved to (project root, manifest file).
///
/// Applies the engine's own rule for a folder: exactly one .arcproj, or it is
/// ambiguous and refused (Project.cpp:29-41). Renaming a project we could not
/// unambiguously identify would rename the wrong thing.
pub fn resolve_project(recorded: &Path) -> Result<(PathBuf, PathBuf), String> {
    if recorded.is_file() {
        if !recorded.extension().is_some_and(|e| e.eq_ignore_ascii_case(project::MANIFEST_EXT)) {
            return Err(format!("{} is not a .arcproj file", recorded.display()));
        }
        let root = recorded
            .parent()
            .ok_or_else(|| format!("{} has no parent folder", recorded.display()))?;
        return Ok((root.to_path_buf(), recorded.to_path_buf()));
    }
    if !recorded.is_dir() {
        return Err(format!("{} is no longer on disk", recorded.display()));
    }
    let names: Vec<String> = std::fs::read_dir(recorded)
        .map_err(|e| format!("could not read {}: {e}", recorded.display()))?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect();
    let file = project::pick_manifest(&names).ok_or_else(|| {
        format!(
            "{} does not contain exactly one .arcproj, so there is no single \
             project to act on",
            recorded.display()
        )
    })?;
    Ok((recorded.to_path_buf(), recorded.join(file)))
}

// These touch the real filesystem (that is this module's whole job), so each
// test gets its own scratch dir -- same pattern as store.rs's tests.
#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-resolve-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write_manifest(dir: &Path, name: &str, abi: u32) -> PathBuf {
        let m = dir.join(format!("{name}.arcproj"));
        std::fs::write(&m, project::manifest_json(name, abi).unwrap()).unwrap();
        m
    }

    #[test]
    fn a_manifest_file_resolves_to_its_parent_and_itself() {
        let dir = scratch("file");
        let m = write_manifest(&dir, "G", 7);
        let (root, manifest) = resolve_project(&m).unwrap();
        assert_eq!(root, dir);
        assert_eq!(manifest, m);
    }

    #[test]
    fn a_folder_with_exactly_one_manifest_resolves() {
        let dir = scratch("folder");
        let m = write_manifest(&dir, "G", 7);
        let (root, manifest) = resolve_project(&dir).unwrap();
        assert_eq!(root, dir);
        assert_eq!(manifest, m);
    }

    #[test]
    fn a_folder_with_two_manifests_is_refused_as_ambiguous() {
        // The engine's own rule (Project.cpp): acting on the wrong one of two
        // projects is worse than making the user pick.
        let dir = scratch("ambiguous");
        write_manifest(&dir, "A", 7);
        write_manifest(&dir, "B", 7);
        let err = resolve_project(&dir).unwrap_err();
        assert!(err.contains("exactly one"), "unexpected message: {err}");
    }

    #[test]
    fn a_non_arcproj_file_is_refused() {
        let dir = scratch("wrongfile");
        let f = dir.join("readme.txt");
        std::fs::write(&f, "hello").unwrap();
        assert!(resolve_project(&f).unwrap_err().contains(".arcproj"));
    }

    #[test]
    fn a_path_no_longer_on_disk_is_refused() {
        let dir = scratch("goneness");
        let err = resolve_project(&dir.join("nope")).unwrap_err();
        assert!(err.contains("no longer on disk"), "unexpected message: {err}");
    }

    #[test]
    fn manifest_abi_round_trips_what_create_project_writes() {
        // manifest_json is the writer create_project uses; reading back the
        // abi it stamped is the whole compatibility story in one round trip.
        let dir = scratch("abi");
        let m = write_manifest(&dir, "G", 9);
        assert_eq!(manifest_abi(&m), Some(9), "by manifest file");
        assert_eq!(manifest_abi(&dir), Some(9), "by project folder");
    }

    #[test]
    fn manifest_abi_is_none_for_garbage_or_nothing() {
        let dir = scratch("badabi");
        assert_eq!(manifest_abi(&dir), None, "empty folder proves nothing");
        std::fs::write(dir.join("X.arcproj"), "{ not json").unwrap();
        assert_eq!(manifest_abi(&dir.join("X.arcproj")), None);
    }

    #[test]
    fn project_dir_strips_only_a_manifest_leaf() {
        assert_eq!(project_dir(Path::new("C:/g/G.arcproj")), PathBuf::from("C:/g"));
        assert_eq!(project_dir(Path::new("C:/g")), PathBuf::from("C:/g"));
    }
}
