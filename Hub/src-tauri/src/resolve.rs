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

/// What a recursive scan found. `ambiguous` counts folders holding MORE than
/// one .arcproj (the engine refuses those, so the Hub must not guess);
/// `truncated` is the no-silent-caps flag -- true when the visit budget ran
/// out, so "scan finished" can never quietly mean "scan gave up".
pub struct ScanOutcome {
    pub manifests: Vec<PathBuf>,
    pub ambiguous: u32,
    pub truncated: bool,
}

/// How many directories one scan may visit. A mistaken "scan C:\" should end
/// in a truthful truncation report, not a minutes-long hang.
const SCAN_VISIT_BUDGET: usize = 100_000;

/// Walk `dir` recursively collecting every folder that holds exactly one
/// .arcproj (Godot's Scan, with the engine's own one-manifest rule). Skips
/// dot-directories and the DUPLICATE_SKIP names -- build output can contain
/// staged copies of a project that would import as duplicates. Keeps
/// descending past a found project: nothing forbids a project tree holding
/// sample projects deeper in.
pub fn scan_tree(dir: &Path) -> Result<ScanOutcome, String> {
    if !dir.is_dir() {
        return Err(format!("{} is not a folder", dir.display()));
    }
    let mut out = ScanOutcome { manifests: Vec::new(), ambiguous: 0, truncated: false };
    let mut queue = vec![dir.to_path_buf()];
    let mut visited = 0usize;

    while let Some(d) = queue.pop() {
        visited += 1;
        if visited > SCAN_VISIT_BUDGET {
            out.truncated = true;
            break;
        }
        // Unreadable directories are skipped, not fatal: a scan of a big tree
        // WILL meet something access-denied, and one locked folder must not
        // void the whole import.
        let Ok(entries) = std::fs::read_dir(&d) else { continue };

        let mut files = Vec::new();
        let mut manifest_count = 0u32;
        for entry in entries.filter_map(|e| e.ok()) {
            let name = entry.file_name().to_string_lossy().to_string();
            let Ok(ty) = entry.file_type() else { continue };
            if ty.is_dir() {
                if name.starts_with('.') || project::skip_in_duplicate(&name) {
                    continue;
                }
                queue.push(entry.path());
            } else if ty.is_file() {
                if name.to_ascii_lowercase().ends_with(".arcproj") {
                    manifest_count += 1;
                }
                files.push(name);
            }
        }
        if manifest_count > 1 {
            out.ambiguous += 1;
        } else if let Some(m) = project::pick_manifest(&files) {
            out.manifests.push(d.join(m));
        }
    }
    // Stable order for the list and the tests: the queue is depth-first in
    // whatever order read_dir served, which is not a contract.
    out.manifests.sort();
    Ok(out)
}

/// Recursively copy `from` into `to`, skipping `project::DUPLICATE_SKIP`
/// directory names at every depth. Fails on the first IO error and leaves
/// whatever was copied -- the CALLER removes the partial tree, because the
/// rollback promise is part of duplicate_project's user-facing contract, not
/// of a copy primitive.
///
/// Symlinks are neither followed nor copied: a link out of the project tree
/// must not let "duplicate this project" copy an unbounded amount of
/// somewhere else, and a broken link in the copy would be worse than none.
pub fn copy_tree(from: &Path, to: &Path) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("could not create {}: {e}", to.display()))?;
    let entries = std::fs::read_dir(from)
        .map_err(|e| format!("could not read {}: {e}", from.display()))?;
    for entry in entries {
        let entry = entry.map_err(|e| format!("could not read {}: {e}", from.display()))?;
        let name = entry.file_name();
        let src = entry.path();
        let ty = entry
            .file_type()
            .map_err(|e| format!("could not inspect {}: {e}", src.display()))?;
        if ty.is_dir() {
            if project::skip_in_duplicate(&name.to_string_lossy()) {
                continue;
            }
            copy_tree(&src, &to.join(&name))?;
        } else if ty.is_file() {
            std::fs::copy(&src, to.join(&name))
                .map_err(|e| format!("could not copy {}: {e}", src.display()))?;
        }
    }
    Ok(())
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

    fn mk(dir: &Path, rel: &str) -> PathBuf {
        let p = dir.join(rel);
        std::fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn scan_tree_finds_nested_projects_and_reports_what_it_refused() {
        let dir = scratch("scan");
        // Two real projects at different depths...
        write_manifest(&mk(&dir, "A"), "A", 7);
        write_manifest(&mk(&dir, "deep/nested/B"), "B", 7);
        // ...an ambiguous folder (two manifests -- the engine would refuse it)...
        let amb = mk(&dir, "Amb");
        std::fs::write(amb.join("X.arcproj"), b"{}").unwrap();
        std::fs::write(amb.join("Y.arcproj"), b"{}").unwrap();
        // ...and projects a scan must NOT surface: inside a dot-dir and inside
        // build output (a staged copy would import as a duplicate).
        write_manifest(&mk(&dir, ".hidden/C"), "C", 7);
        write_manifest(&mk(&dir, "Binaries/Staged"), "S", 7);

        let out = scan_tree(&dir).unwrap();
        let names: Vec<String> = out
            .manifests
            .iter()
            .map(|m| m.file_name().unwrap().to_string_lossy().to_string())
            .collect();
        assert_eq!(names, ["A.arcproj", "B.arcproj"], "sorted, and only the real ones");
        assert_eq!(out.ambiguous, 1);
        assert!(!out.truncated);
    }

    #[test]
    fn scan_tree_refuses_a_path_that_is_not_a_folder() {
        let dir = scratch("scan-notdir");
        let f = dir.join("file.txt");
        std::fs::write(&f, b"x").unwrap();
        assert!(scan_tree(&f).is_err());
        assert!(scan_tree(&dir.join("missing")).is_err());
    }

    #[test]
    fn copy_tree_copies_content_and_skips_build_output_at_every_depth() {
        let dir = scratch("copytree");
        let src = dir.join("Src");
        // A UE-shaped project: Content + Source at the root, build output at
        // the root AND under a plugin -- the any-depth case.
        std::fs::create_dir_all(src.join("Content/Maps")).unwrap();
        std::fs::write(src.join("Content/Maps/level.ascene"), b"scene").unwrap();
        std::fs::write(src.join("G.arcproj"), b"{}").unwrap();
        std::fs::create_dir_all(src.join("Binaries")).unwrap();
        std::fs::write(src.join("Binaries/Game.dll"), b"x").unwrap();
        std::fs::create_dir_all(src.join("Plugins/P/Intermediate")).unwrap();
        std::fs::write(src.join("Plugins/P/Intermediate/junk.obj"), b"x").unwrap();
        std::fs::write(src.join("Plugins/P/P.dll"), b"p").unwrap();
        std::fs::create_dir_all(src.join(".git")).unwrap();
        std::fs::write(src.join(".git/HEAD"), b"ref").unwrap();

        let dst = dir.join("Dst");
        copy_tree(&src, &dst).unwrap();

        assert!(dst.join("Content/Maps/level.ascene").is_file());
        assert!(dst.join("G.arcproj").is_file());
        assert!(dst.join("Plugins/P/P.dll").is_file());
        assert!(!dst.join("Binaries").exists(), "root build output is skipped");
        assert!(!dst.join("Plugins/P/Intermediate").exists(), "nested build output too");
        assert!(!dst.join(".git").exists(), "a duplicate is not a second checkout");
    }

    #[test]
    fn copy_tree_of_a_missing_source_fails_without_inventing_a_destination_tree() {
        let dir = scratch("copytree-missing");
        let dst = dir.join("Dst");
        assert!(copy_tree(&dir.join("NotThere"), &dst).is_err());
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
