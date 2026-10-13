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
    }
}
