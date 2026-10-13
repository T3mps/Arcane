#![allow(dead_code)]

use arcane_release::manifest::{Artifact, Channel, Channels, HubInfo, Manifest, Release, SCHEMA};

pub const BUILD: &str = "0123456789abcdef0123456789abcdef01234567";

pub fn sha(c: char) -> String {
    c.to_string().repeat(64)
}

pub fn artifact(v: &str, os: &str, kind: &str) -> Artifact {
    let ext = if os == "windows" { "zip" } else { "tar.gz" };
    let suffix = if kind == "symbols" { "-symbols" } else { "" };
    Artifact {
        os: os.into(),
        arch: "x86_64".into(),
        kind: kind.into(),
        path: format!("engine/{v}/arcane-{v}-{os}-x86_64{suffix}.{ext}"),
        size: 1000,
        sha256: sha(if kind == "symbols" { '5' } else { 'e' }),
        extra: Default::default(),
    }
}

pub fn release(v: &str) -> Release {
    let channel = if v.contains("-preview.") { Channel::Preview } else { Channel::Stable };
    Release {
        version: v.into(),
        channel,
        released_at: "2026-10-01T12:00:00Z".into(),
        build: BUILD.into(),
        abi: 59,
        license: "BUSL-1.1".into(),
        change_date: "2030-10-01".into(),
        min_hub_version: None,
        yanked: false,
        yank_reason: None,
        security: false,
        notes_url: format!("https://starworks.dev/arcane/releases#{v}"),
        notes: format!("Notes for {v}."),
        artifacts: vec![artifact(v, "windows", "engine")],
        extra: Default::default(),
    }
}

pub fn manifest(releases: Vec<Release>) -> Manifest {
    Manifest {
        schema: SCHEMA,
        serial: 1,
        generated_at: "2026-10-01T12:00:00Z".into(),
        expires: "2026-12-30T12:00:00Z".into(),
        base_url: "https://downloads.starworks.dev/".into(),
        channels: Channels::default(),
        hub: HubInfo::default(),
        releases,
        extra: Default::default(),
    }
}
use std::fs;
use std::path::PathBuf;
use std::sync::OnceLock;

pub const TEST_PASSWORD: &str = "arcane-test-key-not-a-secret";

pub fn key_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests").join("keys")
}

/// Test key 1 or 2, opened once per test binary (scrypt is slow on purpose).
pub fn test_secret(n: u8) -> &'static minisign::SecretKey {
    static KEYS: [OnceLock<minisign::SecretKey>; 2] = [OnceLock::new(), OnceLock::new()];
    KEYS[usize::from(n - 1)].get_or_init(|| {
        let text = fs::read_to_string(key_dir().join(format!("test-manifest-{n}.key"))).unwrap();
        arcane_release::sign::open_secret_key(&text, TEST_PASSWORD).unwrap()
    })
}

pub fn test_public_line(n: u8) -> String {
    let text = fs::read_to_string(key_dir().join(format!("test-manifest-{n}.pub"))).unwrap();
    text.lines().map(str::trim).rfind(|l| !l.is_empty()).unwrap().to_string()
}

pub fn test_public(n: u8) -> minisign_verify::PublicKey {
    minisign_verify::PublicKey::from_base64(&test_public_line(n)).unwrap()
}
