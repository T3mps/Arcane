use std::fs::{self, File};
use std::io::Read;

use arcane_release::digest::{self, TreeProblem, MANIFEST_TXT};
use arcane_release::pack;
use sha2::{Digest, Sha256};
use tempfile::TempDir;

fn sha256(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

fn stage() -> TempDir {
    let t = TempDir::new().unwrap();
    fs::create_dir_all(t.path().join("bin/sub")).unwrap();
    fs::write(t.path().join("bin/editor.exe"), b"editor").unwrap();
    fs::write(t.path().join("bin/sub/a.dll"), b"a").unwrap();
    fs::write(t.path().join("LICENSE"), b"license").unwrap();
    t
}

fn zip_names(path: &std::path::Path) -> Vec<String> {
    let a = zip::ZipArchive::new(File::open(path).unwrap()).unwrap();
    let mut names: Vec<String> = a.file_names().map(String::from).collect();
    names.sort();
    names
}

fn tar_names(path: &std::path::Path) -> Vec<String> {
    let mut a = tar::Archive::new(flate2::read::GzDecoder::new(File::open(path).unwrap()));
    let mut names: Vec<String> =
        a.entries().unwrap().map(|e| e.unwrap().path().unwrap().to_string_lossy().into_owned()).collect();
    names.sort();
    names
}

const EXPECTED: [&str; 4] = ["arcane-0.3.1/LICENSE", "arcane-0.3.1/bin/editor.exe", "arcane-0.3.1/bin/sub/a.dll", "arcane-0.3.1/manifest.txt"];

#[test]
fn a_digest_is_the_size_and_the_sha256() {
    let t = TempDir::new().unwrap();
    let p = t.path().join("abc");
    fs::write(&p, b"abc").unwrap();
    let d = digest::digest_file(&p).unwrap();
    assert_eq!(d.size, 3);
    assert_eq!(d.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

#[test]
fn the_listing_is_sha256sum_format_sorted_and_omits_itself() {
    let s = stage();
    fs::write(s.path().join(MANIFEST_TXT), "stale").unwrap();
    let listing = digest::tree_listing(s.path()).unwrap();
    let paths: Vec<&str> = listing.lines().map(|l| l.split_once("  ").unwrap().1).collect();
    assert_eq!(paths, ["LICENSE", "bin/editor.exe", "bin/sub/a.dll"]);
    assert!(listing.starts_with(&format!("{}  LICENSE\n", sha256(b"license"))));
}

#[test]
fn a_zip_holds_every_file_under_one_root_folder() {
    let s = stage();
    let out = TempDir::new().unwrap();
    let zip = out.path().join("e.zip");
    let d = pack::pack(s.path(), "arcane-0.3.1", &zip).unwrap();
    assert_eq!(d, digest::digest_file(&zip).unwrap());
    assert_eq!(zip_names(&zip), EXPECTED);
    let mut a = zip::ZipArchive::new(File::open(&zip).unwrap()).unwrap();
    let mut text = String::new();
    a.by_name("arcane-0.3.1/manifest.txt").unwrap().read_to_string(&mut text).unwrap();
    assert_eq!(text, digest::tree_listing(s.path()).unwrap());
}

#[test]
fn a_tar_gz_holds_the_same() {
    let s = stage();
    let out = TempDir::new().unwrap();
    let tgz = out.path().join("e.tar.gz");
    pack::pack(s.path(), "arcane-0.3.1", &tgz).unwrap();
    assert_eq!(tar_names(&tgz), EXPECTED);
}

#[test]
fn packing_is_deterministic() {
    let s = stage();
    let out = TempDir::new().unwrap();
    for ext in ["zip", "tar.gz"] {
        let a = pack::pack(s.path(), "r", &out.path().join(format!("a.{ext}"))).unwrap();
        let b = pack::pack(s.path(), "r", &out.path().join(format!("b.{ext}"))).unwrap();
        assert_eq!(a, b, "{ext} differs between two packs");
    }
}

#[test]
fn verify_tree_reports_changed_missing_and_unlisted_files() {
    let s = stage();
    let out = TempDir::new().unwrap();
    pack::pack(s.path(), "r", &out.path().join("x.zip")).unwrap();
    assert!(digest::verify_tree(s.path()).unwrap().is_empty());
    fs::write(s.path().join("bin/editor.exe"), b"patched").unwrap();
    fs::remove_file(s.path().join("bin/sub/a.dll")).unwrap();
    fs::write(s.path().join("bin/extra.dll"), b"x").unwrap();
    let mut problems = digest::verify_tree(s.path()).unwrap();
    problems.sort_by_key(|p| format!("{p:?}"));
    assert_eq!(
        problems,
        vec![
            TreeProblem::Changed("bin/editor.exe".into()),
            TreeProblem::Missing("bin/sub/a.dll".into()),
            TreeProblem::Unlisted("bin/extra.dll".into()),
        ]
    );
}

#[test]
fn a_listing_may_not_climb_out_of_its_folder() {
    let s = stage();
    fs::write(s.path().join(MANIFEST_TXT), format!("{}  ../outside\n", sha256(b"x"))).unwrap();
    assert!(digest::verify_tree(s.path()).is_err());
}

#[test]
fn unknown_archive_types_and_nested_root_names_are_refused() {
    let s = stage();
    let out = TempDir::new().unwrap();
    assert!(pack::pack(s.path(), "r", &out.path().join("x.7z")).is_err());
    assert!(pack::pack(s.path(), "a/b", &out.path().join("x.zip")).is_err());
}

#[cfg(unix)]
#[test]
fn symlinks_are_refused() {
    let s = stage();
    std::os::unix::fs::symlink("editor.exe", s.path().join("bin/link")).unwrap();
    let out = TempDir::new().unwrap();
    assert!(pack::pack(s.path(), "r", &out.path().join("x.tar.gz")).is_err());
}

#[cfg(unix)]
#[test]
fn tar_keeps_the_executable_bit() {
    use std::os::unix::fs::PermissionsExt;
    let s = stage();
    fs::set_permissions(s.path().join("bin/editor.exe"), fs::Permissions::from_mode(0o755)).unwrap();
    let out = TempDir::new().unwrap();
    let tgz = out.path().join("x.tar.gz");
    pack::pack(s.path(), "r", &tgz).unwrap();
    let mut a = tar::Archive::new(flate2::read::GzDecoder::new(File::open(&tgz).unwrap()));
    let mode = a
        .entries()
        .unwrap()
        .map(|e| e.unwrap())
        .find(|e| e.path().unwrap().ends_with("bin/editor.exe"))
        .unwrap()
        .header()
        .mode()
        .unwrap();
    assert_eq!(mode, 0o755);
}
