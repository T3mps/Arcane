mod common;

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use tempfile::TempDir;

const BIN: &str = env!("CARGO_BIN_EXE_arcrelease");
const NOW: &str = "2026-10-12T00:00:00Z";

fn key_text(n: u8) -> String {
    fs::read_to_string(common::key_dir().join(format!("test-manifest-{n}.key"))).unwrap()
}

fn pub_path(n: u8) -> String {
    common::key_dir().join(format!("test-manifest-{n}.pub")).to_string_lossy().into_owned()
}

/// Runs arcrelease; `key` = Some(n) signs with test key n.
fn arc(args: &[&str], key: Option<u8>, password: &str) -> Output {
    let mut c = Command::new(BIN);
    c.args(args).env_remove("ARCANE_MINISIGN_KEY").env_remove("ARCANE_MINISIGN_PASSWORD");
    if let Some(n) = key {
        c.env("ARCANE_MINISIGN_KEY", key_text(n)).env("ARCANE_MINISIGN_PASSWORD", password);
    }
    c.output().unwrap()
}

fn ok(o: &Output) {
    assert!(o.status.success(), "stderr: {}", String::from_utf8_lossy(&o.stderr));
}

fn failed_with(o: &Output, text: &str) {
    let err = String::from_utf8_lossy(&o.stderr);
    assert!(!o.status.success(), "unexpectedly succeeded");
    assert!(err.contains(text), "stderr does not mention {text:?}: {err}");
}

fn p(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

fn make_dist(root: &Path, version: &str) -> PathBuf {
    let dist = root.join("dist");
    let channel = if version.contains("-preview.") { "preview" } else { "stable" };
    for (os, ext) in [("windows", "zip"), ("linux", "tar.gz")] {
        let d = dist.join(os);
        fs::create_dir_all(&d).unwrap();
        fs::write(d.join("engine.json"), format!(r#"{{"version":"{version}","build":"{}","abi":59,"channel":"{channel}"}}"#, common::BUILD)).unwrap();
        fs::write(d.join(format!("arcane-{version}-{os}-x86_64.{ext}")), format!("engine {os}")).unwrap();
        fs::write(d.join(format!("arcane-{version}-{os}-x86_64-symbols.{ext}")), format!("symbols {os}")).unwrap();
    }
    dist
}

/// init (serial 1) into tmp/m1.
fn init(tmp: &Path) -> PathBuf {
    let out = tmp.join("m1");
    ok(&arc(&["init", "--trust-key", &pub_path(1), "--now", NOW, "--out-dir", &p(&out)], Some(1), common::TEST_PASSWORD));
    out
}

/// add <version> on top of `cur` into tmp/<name>.
fn add(tmp: &Path, cur: &Path, version: &str, name: &str) -> (Output, PathBuf) {
    let dist = make_dist(&tmp.join(format!("d-{name}")), version);
    let notes = tmp.join("notes.md");
    fs::write(&notes, "Fixes and features.").unwrap();
    let out = tmp.join(name);
    let o = arc(
        &[
            "add", "--manifest", &p(&cur.join("releases.json")), "--sig", &p(&cur.join("releases.json.minisig")),
            "--trust-key", &pub_path(1), "--now", NOW, "--dist", &p(&dist), "--version", version,
            "--released-at", NOW, "--notes-file", &p(&notes), "--out-dir", &p(&out),
        ],
        Some(1),
        common::TEST_PASSWORD,
    );
    (o, out)
}

fn verify(dir: &Path, extra: &[&str]) -> Output {
    let mut args = vec![
        "verify".to_string(), "--manifest".into(), p(&dir.join("releases.json")), "--sig".into(),
        p(&dir.join("releases.json.minisig")), "--trust-key".into(), pub_path(1), "--now".into(), NOW.into(),
    ];
    args.extend(extra.iter().map(|s| s.to_string()));
    let refs: Vec<&str> = args.iter().map(String::as_str).collect();
    arc(&refs, None, "")
}

fn serial(dir: &Path) -> u64 {
    let v: serde_json::Value = serde_json::from_slice(&fs::read(dir.join("releases.json")).unwrap()).unwrap();
    v["serial"].as_u64().unwrap()
}

#[test]
fn init_writes_a_verifiable_serial_1() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    ok(&verify(&m1, &[]));
    assert_eq!(serial(&m1), 1);
}

#[test]
fn add_publishes_the_dist_folder_as_four_artifacts() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let (o, m2) = add(t.path(), &m1, "0.3.1", "m2");
    ok(&o);
    ok(&verify(&m2, &[]));
    let v: serde_json::Value = serde_json::from_slice(&fs::read(m2.join("releases.json")).unwrap()).unwrap();
    assert_eq!(v["serial"], 2);
    let artifacts = v["releases"][0]["artifacts"].as_array().unwrap();
    assert_eq!(artifacts.len(), 4);
    assert_eq!(artifacts[0]["path"], "engine/0.3.1/arcane-0.3.1-windows-x86_64.zip");
    assert_eq!(artifacts[0]["sha256"], hex::encode(<sha2::Sha256 as sha2::Digest>::digest(b"engine windows")));
    assert_eq!(v["releases"][0]["notes"], "Fixes and features.");
}

#[test]
fn a_published_version_is_refused_and_nothing_is_written() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let (o, m2) = add(t.path(), &m1, "0.3.1", "m2");
    ok(&o);
    let (o, m3) = add(t.path(), &m2, "0.3.1", "m3");
    failed_with(&o, "already published");
    assert!(!m3.join("releases.json").exists());
}

#[test]
fn a_tampered_current_manifest_is_never_built_on() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let path = m1.join("releases.json");
    let text = fs::read_to_string(&path).unwrap().replace("\"serial\": 1", "\"serial\": 7");
    fs::write(&path, text).unwrap();
    let (o, m2) = add(t.path(), &m1, "0.3.1", "m2");
    failed_with(&o, "does not verify");
    assert!(!m2.exists());
}

#[test]
fn signing_with_an_untrusted_key_writes_nothing() {
    let t = TempDir::new().unwrap();
    let out = t.path().join("m1");
    let o = arc(&["init", "--trust-key", &pub_path(1), "--now", NOW, "--out-dir", &p(&out)], Some(2), common::TEST_PASSWORD);
    failed_with(&o, "does not verify against the trusted keys");
    assert!(!out.join("releases.json").exists());
}

#[test]
fn a_wrong_password_fails_clearly() {
    let t = TempDir::new().unwrap();
    let out = t.path().join("m1");
    let o = arc(&["init", "--trust-key", &pub_path(1), "--now", NOW, "--out-dir", &p(&out)], Some(1), "wrong");
    failed_with(&o, "could not be opened");
}

#[test]
fn a_dist_folder_with_surprises_is_refused() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let dist = make_dist(&t.path().join("d-x"), "0.3.1");
    fs::write(dist.join("linux").join("stray.txt"), "x").unwrap();
    let notes = t.path().join("n.md");
    fs::write(&notes, "n").unwrap();
    let o = arc(
        &[
            "add", "--manifest", &p(&m1.join("releases.json")), "--sig", &p(&m1.join("releases.json.minisig")),
            "--trust-key", &pub_path(1), "--now", NOW, "--dist", &p(&dist), "--version", "0.3.1",
            "--released-at", NOW, "--notes-file", &p(&notes), "--out-dir", &p(&t.path().join("m2")),
        ],
        Some(1),
        common::TEST_PASSWORD,
    );
    failed_with(&o, "unexpected file stray.txt");
}

#[test]
fn engine_json_must_agree_across_oses() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let dist = make_dist(&t.path().join("d-x"), "0.3.1");
    let other = fs::read_to_string(dist.join("linux/engine.json")).unwrap().replace("\"abi\":59", "\"abi\":60");
    fs::write(dist.join("linux/engine.json"), other).unwrap();
    let notes = t.path().join("n.md");
    fs::write(&notes, "n").unwrap();
    let o = arc(
        &[
            "add", "--manifest", &p(&m1.join("releases.json")), "--sig", &p(&m1.join("releases.json.minisig")),
            "--trust-key", &pub_path(1), "--now", NOW, "--dist", &p(&dist), "--version", "0.3.1",
            "--released-at", NOW, "--notes-file", &p(&notes), "--out-dir", &p(&t.path().join("m2")),
        ],
        Some(1),
        common::TEST_PASSWORD,
    );
    failed_with(&o, "differs between OSes");
}

#[test]
fn yank_and_resign_keep_released_at() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let (o, m2) = add(t.path(), &m1, "0.3.1", "m2");
    ok(&o);
    let m3 = t.path().join("m3");
    ok(&arc(
        &[
            "yank", "--manifest", &p(&m2.join("releases.json")), "--sig", &p(&m2.join("releases.json.minisig")),
            "--trust-key", &pub_path(1), "--now", "2026-11-01T00:00:00Z", "--version", "0.3.1", "--reason", "Broken.",
            "--out-dir", &p(&m3),
        ],
        Some(1),
        common::TEST_PASSWORD,
    ));
    let m4 = t.path().join("m4");
    ok(&arc(
        &[
            "resign", "--manifest", &p(&m3.join("releases.json")), "--sig", &p(&m3.join("releases.json.minisig")),
            "--trust-key", &pub_path(1), "--now", "2026-12-01T00:00:00Z", "--out-dir", &p(&m4),
        ],
        Some(1),
        common::TEST_PASSWORD,
    ));
    let v: serde_json::Value = serde_json::from_slice(&fs::read(m4.join("releases.json")).unwrap()).unwrap();
    assert_eq!(v["serial"], 4);
    assert_eq!(v["releases"][0]["yanked"], true);
    assert_eq!(v["releases"][0]["released_at"], NOW);
}

#[test]
fn verify_refuses_a_rollback() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    failed_with(&verify(&m1, &["--highest-serial", "2"]), "rollback");
    ok(&verify(&m1, &["--highest-serial", "1"]));
}

#[test]
fn an_expired_manifest_can_still_be_resigned() {
    let t = TempDir::new().unwrap();
    let old = t.path().join("old");
    ok(&arc(&["init", "--trust-key", &pub_path(1), "--now", "2026-01-01T00:00:00Z", "--out-dir", &p(&old)], Some(1), common::TEST_PASSWORD));
    failed_with(&verify(&old, &[]), "expired");
    let fresh = t.path().join("fresh");
    ok(&arc(
        &[
            "resign", "--manifest", &p(&old.join("releases.json")), "--sig", &p(&old.join("releases.json.minisig")),
            "--trust-key", &pub_path(1), "--now", NOW, "--out-dir", &p(&fresh),
        ],
        Some(1),
        common::TEST_PASSWORD,
    ));
    ok(&verify(&fresh, &[]));
}

#[test]
fn without_trust_key_the_test_key_is_not_trusted() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let o = arc(&["verify", "--manifest", &p(&m1.join("releases.json")), "--sig", &p(&m1.join("releases.json.minisig"))], None, "");
    assert!(!o.status.success());
}

#[test]
fn change_date_reports_the_bsl_date() {
    let t = TempDir::new().unwrap();
    let m1 = init(t.path());
    let o = arc(
        &[
            "change-date", "--manifest", &p(&m1.join("releases.json")), "--sig", &p(&m1.join("releases.json.minisig")),
            "--trust-key", &pub_path(1), "--version", "0.3.1", "--released-at", NOW,
        ],
        None,
        "",
    );
    ok(&o);
    assert_eq!(String::from_utf8_lossy(&o.stdout).trim(), "2030-10-12");
}
