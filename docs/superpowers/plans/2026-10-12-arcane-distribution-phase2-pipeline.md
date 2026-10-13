# Arcane distribution, phase 2: release pipeline and hosting — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A signed, verifiable release manifest and the GitHub Actions + Cloudflare R2 pipeline that publishes engine archives to `downloads.starworks.dev`, proven end to end by a rehearsal that signs a real binary and publishes it to a `staging/` area.

**Architecture:** One Rust crate, `ArcaneHub/crates/arcane_release`, owns the manifest: its schema, the operations CI applies (add, yank, re-sign), minisign signing and verification, and the archive packer, with an `arcrelease` CLI for CI. The Hub links the same crate in phase 3 (`default-features = false`), so there is one implementation of the format. Thin bash scripts drive R2 through aws-cli's S3 API with conditional writes, never overwriting an archive and never replacing a manifest someone else changed. Two workflows (`release.yml` and `manifest-admin.yml`) run them behind the `release` environment's approval gate.

**Tech Stack:** Rust 2021 (serde, semver, time, minisign 0.7.9, minisign-verify 0.3, zip 2, tar, flate2, clap 4), bash with jq, aws-cli v2 (pinned), GitHub Actions, Azure Artifact Signing (`azure/artifact-signing-action`), Cloudflare R2 and Cache Rules.

**Spec:** `docs/superpowers/specs/2026-10-12-arcane-distribution-design.md` (phase 2 of s11; sections s3, s4, s5, s9, s10, s12). Read it before starting.

**This is plan 2 of 6.** The spec's six phases each get their own plan, written when that phase is next. Phase 1 (engine release-readiness) waits for modularization P6/P7 and plan A (`ci/local-pipeline`), neither of which is on `main` yet. Phase 2 needs neither: it builds and proves everything after "a folder of finished engine files exists", using a fixture build in place of the engine. Phase 3 (the Hub engine manager) consumes this plan's crate and golden manifests.

## Global Constraints

- Crate: `ArcaneHub/crates/arcane_release`, package `arcane_release`, binary `arcrelease`, edition 2021, `publish = false`. Feature `tools` (default) holds signing, archives and the CLI; `--no-default-features` must build (the Hub's dependency, phase 3).
- Signing: `minisign = "=0.7.9"`. Verification: `minisign-verify = "0.3"`. Every key is **encrypted**, and is always opened with `into_secret_key(Some(password))`: `None` prompts on the terminal and would hang CI.
- Manifest: schema 1 exactly as spec s5, plus a `notes` string per release (spec s8 says the notes are copied into `releases.json`). `expires` = signing time + **90 days**. Timestamps are RFC 3339 UTC with `Z` and whole seconds; dates are `YYYY-MM-DD`.
- `released_at` is never rewritten. `change_date` = the earliest `released_at` among releases sharing the version's major.minor.patch (previews included), plus 4 years; Feb 29 to a non-leap year falls back to Feb 28 (spec s5, s12).
- `released_at` is the moment the release run starts (the `prepare` job), so phase 1's LICENSE stamp and the manifest agree. An approval delay makes it slightly earlier than public availability, which errs toward the user (spec s12).
- Channels: `stable` = the newest non-yanked stable; `preview` = the newest non-yanked release of either channel.
- Hosting: bucket `arcane-downloads`, served only at `https://downloads.starworks.dev/` (never `r2.dev`). Uploads use the S3 API via aws-cli **2.37.13**, with `AWS_REQUEST_CHECKSUM_CALCULATION=WHEN_REQUIRED` and `AWS_RESPONSE_CHECKSUM_VALIDATION=WHEN_REQUIRED`, never Wrangler (spec s12).
- Archives: `engine/<v>/arcane-<v>-<os>-x86_64[-symbols].<zip|tar.gz>`, written with `If-None-Match: *`. An existing object with the same SHA-256 counts as already uploaded; different content fails.
- Manifest pointer `releases.json` is written with `If-Match: <etag read>`, then `releases.json.minisig`. Each serial is also kept immutably at `history/<serial, 9 digits>.json(.minisig)`.
- The manifest pair (`*.json`, `*.minisig`) **bypasses** the edge cache. The spec (s3) said "a short TTL"; bypass is that TTL at zero. With any TTL, the edge could serve a new `releases.json` beside a stale signature.
- `staging/` is a second, separate manifest (`base_url` `https://downloads.starworks.dev/staging/`), signed with the committed **test** key, which no Hub trusts. Dry runs and rehearsals use it; production never does.
- Workflows (spec s4, roadmap D26):
  - every third-party `uses:` is pinned to a 40-hex commit SHA;
  - top-level `permissions: contents: read`, with jobs granted only what they need;
  - secrets live only in the `release` environment, which needs the owner's approval;
  - no `pull_request` or `pull_request_target` trigger reaches it;
  - rehearsals and manifest admin run only from `main`;
  - no workflow-level concurrency group (final review: a queued run cancels a pending one); R2's conditional writes keep writers apart.
- Release runners: `windows-2025` and `ubuntu-24.04`.
- Notes: `docs/releases/<version>.md` must exist and be non-empty at the tag. It feeds both the GitHub Release and `releases.json`.
- Phase 1 contract (assumed here, provided there):
  - `Version.hpp` gains `inline constexpr int kVersionPatch = N;` and `inline constexpr const char* kVersionPrerelease = "";` (`"preview.N"` for previews);
  - `engine.json` is `{ "version", "build" (40-hex SHA), "abi", "channel" }`;
  - a stage folder holds `stage/engine/...` and `stage/symbols/...`;
  - symlinks are not allowed in a stage.
- Action versions match plan A's pins (PR #7):
  - `actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1`;
  - `actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2`;
  - `actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0` if a cache is added;
  - `dtolnay/rust-toolchain@686976e191b89faba57d3206551f0f330d8cb249 # stable`.

  The rest (`actions/download-artifact`, `azure/login`, `azure/artifact-signing-action`, `actions/create-github-app-token`) are written at their current major and frozen by `pin-actions.sh`. Never a v4 action (the Node 20 line the 2026-10-12 Actions audit retired, C3).
- Every `actions/checkout` sets `persist-credentials: false` (audit C7). Nothing in these jobs pushes with git; `gh` uses an explicit `GH_TOKEN`.
- `history/<serial>.json` and its `.minisig` are written FIRST and are write-once. An existing copy is refused unless it is byte-identical, which only a re-run of the same signing produces. That way repair always has a signed copy to restore from.
- The website rebuild is a `repository_dispatch` (`arcane-release`) to `StarworksDev/StarworksWebsite`. Its token comes from a StarworksDev GitHub App (`actions/create-github-app-token`), never a personal token. The website's own workflow builds and deploys (Task 16).
- A watchdog outside GitHub's scheduler checks `releases.json`'s `expires` daily and alerts when fewer than 30 days remain: the Starworks Ops bot (Task 17). A disabled schedule fails silently.
- Never echo a secret. Scripts read secrets from the environment only.

## Review Focus

These are the failure modes most likely to hurt someone using this, which the spec implies but doesn't spell out. Each is pinned by a test in the owning task.

1. **Two runs write `releases.json` at once.** For example, a tag publishes while the monthly re-sign is approved. Expected: the loser fails with nothing overwritten, and the winner's manifest stands. Pinned in Task 7 (`race`).
2. **A publish is re-run after it uploaded the archives but failed before the manifest.** Expected: identical archives are accepted as already uploaded, and a different file under the same name is refused. Pinned in Task 7 (`rerun_after_partial`, `put_immutable`).
3. **The published manifest doesn't verify**, because it was tampered with or because the pointer and its signature were only half updated. Expected: CI refuses to build on it and names the repair. Repair restores the newest history pair that verifies. Pinned in Task 5 (CLI) and Task 7 (`tampered`, `half_update_and_repair`).
4. **The signing secret is wrong**: a bad password, or a key the readers don't trust. Expected: nothing is written, and the message says why. Pinned in Task 5.
5. **Releases that share a base version, and calendar edges.** Expected:
   - the first preview starts the four-year clock;
   - yanking the newest preview moves the channel back;
   - a leap-day release whose date four years on doesn't exist gets February 28.

   Pinned in Task 3.

---

### Task 1: The `arcane_release` crate, version rules and the manifest schema

**Files:**
- Create: `ArcaneHub/crates/arcane_release/Cargo.toml`
- Create: `ArcaneHub/crates/arcane_release/.cargo/config.toml`
- Create: `ArcaneHub/crates/arcane_release/src/lib.rs`
- Create: `ArcaneHub/crates/arcane_release/src/version.rs`
- Create: `ArcaneHub/crates/arcane_release/src/manifest.rs`
- Create: `ArcaneHub/crates/arcane_release/tests/common/mod.rs`
- Test: `ArcaneHub/crates/arcane_release/tests/manifest_test.rs`

**Interfaces:**
- Produces:
  - `version::parse(&str) -> Result<semver::Version, VersionError>`;
  - `version::channel_of(&Version) -> Result<Channel, VersionError>`;
  - `version::base(&Version) -> Version`;
  - the types `Manifest`, `Release`, `Artifact`, `Channels`, `HubInfo`, `Channel`;
  - `Manifest::{parse, to_bytes, release, validate}`;
  - `ManifestError`;
  - `parse_ts`, `format_ts`, `parse_date`, `format_date`, `is_lower_hex`;
  - the constants `SCHEMA`, `BASE_URL`, `NOTES_URL_BASE`, `LICENSE_ID`.

- [ ] **Step 1: Create the crate files**

`ArcaneHub/crates/arcane_release/Cargo.toml`:

```toml
[package]
name = "arcane_release"
version = "0.1.0"
description = "Arcane release manifest: schema, operations, signing, verification and archives (CI and the Hub)"
authors = ["Starworks"]
edition = "2021"
publish = false

[features]
default = ["tools"]
# CI-only: signing, archives and the arcrelease CLI. The Hub (phase 3) depends
# on this crate with default-features = false: verification and manifest.txt
# checks only.
tools = ["dep:minisign", "dep:clap", "dep:anyhow", "dep:zip", "dep:tar", "dep:flate2"]

[[bin]]
name = "arcrelease"
path = "src/bin/arcrelease.rs"
required-features = ["tools"]

[dependencies]
serde = { version = "1", features = ["derive"] }
# preserve_order: the manifest's bytes are what is signed; a stable field order
# keeps a re-serialized manifest byte-identical (the goldens depend on it).
serde_json = { version = "1", features = ["preserve_order"] }
semver = "1"
time = { version = "0.3", features = ["formatting", "parsing", "macros"] }
thiserror = "2"
minisign-verify = "0.3"
sha2 = "0.10"
hex = "0.4"
walkdir = "2"
minisign = { version = "=0.7.9", optional = true }
clap = { version = "4", features = ["derive"], optional = true }
anyhow = { version = "1", optional = true }
zip = { version = "2", default-features = false, features = ["deflate"], optional = true }
tar = { version = "0.4", optional = true }
flate2 = { version = "1", optional = true }

[dev-dependencies]
tempfile = "3"
```

`ArcaneHub/crates/arcane_release/.cargo/config.toml`:

```toml
# Cargo intermediates land under Arcane/bin-int/, beside the Hub's
# (ArcaneHub/src-tauri/.cargo/config.toml) and premake's objdirs.
[build]
target-dir = "../../../bin-int/release-cargo"
```

`ArcaneHub/crates/arcane_release/src/lib.rs`:

```rust
//! Arcane's release manifest, `releases.json` (spec
//! docs/superpowers/specs/2026-10-12-arcane-distribution-design.md s5): its
//! schema, the operations CI applies to it, minisign signing and verification,
//! and the engine archive packer. CI drives it through the `arcrelease`
//! binary; the Hub links the library with `default-features = false`.

pub mod manifest;
pub mod version;
```

- [ ] **Step 2: Write the failing tests**

`ArcaneHub/crates/arcane_release/tests/common/mod.rs`:

```rust
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
```

`ArcaneHub/crates/arcane_release/tests/manifest_test.rs`:

```rust
mod common;

use arcane_release::manifest::{Channel, Manifest, ManifestError};
use arcane_release::version::{self, VersionError};
use common::{manifest, release};

#[test]
fn stable_and_preview_versions_parse() {
    assert_eq!(version::channel_of(&version::parse("0.3.1").unwrap()).unwrap(), Channel::Stable);
    assert_eq!(version::channel_of(&version::parse("0.4.0-preview.2").unwrap()).unwrap(), Channel::Preview);
}

#[test]
fn other_prereleases_and_build_metadata_are_refused() {
    for v in ["0.4.0-beta.1", "0.4.0-preview", "0.4.0-preview.x", "0.4.0-preview.1.2"] {
        assert!(matches!(version::parse(v), Err(VersionError::BadPrerelease(_))), "{v} was accepted");
    }
    assert!(matches!(version::parse("0.4.0+abc"), Err(VersionError::BuildMetadata(_))));
    assert!(matches!(version::parse("v0.4.0"), Err(VersionError::NotSemver(_))));
}

#[test]
fn previews_order_numerically_and_before_their_stable() {
    let v = |s: &str| version::parse(s).unwrap();
    assert!(v("0.4.0-preview.10") > v("0.4.0-preview.2"));
    assert!(v("0.4.0") > v("0.4.0-preview.10"));
    assert!(v("0.4.0-preview.1") > v("0.3.1"));
    assert_eq!(version::base(&v("0.4.0-preview.2")), v("0.4.0"));
}

#[test]
fn round_trips_through_json() {
    let m = manifest(vec![release("0.3.1")]);
    assert_eq!(Manifest::parse(&m.to_bytes()).unwrap(), m);
}

#[test]
fn unknown_fields_are_ignored() {
    let mut v: serde_json::Value = serde_json::from_slice(&manifest(vec![release("0.3.1")]).to_bytes()).unwrap();
    v["added_later"] = serde_json::json!({ "x": 1 });
    v["releases"][0]["also_later"] = serde_json::json!(true);
    let m = Manifest::parse(&serde_json::to_vec(&v).unwrap()).unwrap();
    assert_eq!(m.releases[0].version, "0.3.1");
}

#[test]
fn a_good_manifest_validates() {
    let mut m = manifest(vec![release("0.4.0-preview.2"), release("0.3.1")]);
    m.channels.stable = Some("0.3.1".into());
    m.channels.preview = Some("0.4.0-preview.2".into());
    m.validate().unwrap();
}

#[test]
fn duplicate_versions_are_refused() {
    let m = manifest(vec![release("0.3.1"), release("0.3.1")]);
    assert_eq!(m.validate(), Err(ManifestError::DuplicateVersion("0.3.1".into())));
}

#[test]
fn a_channel_that_disagrees_with_the_version_is_refused() {
    let mut r = release("0.3.1");
    r.channel = Channel::Preview;
    assert!(matches!(manifest(vec![r]).validate(), Err(ManifestError::ChannelMismatch { .. })));
}

#[test]
fn artifacts_must_sit_in_their_version_folder() {
    for path in [
        "engine/0.3.0/arcane.zip",
        "engine/0.3.1/../x.zip",
        "engine/0.3.1/sub/x.zip",
        "engine/0.3.1/",
        "/engine/0.3.1/x.zip",
        "engine/0.3.1/.hidden.zip",
    ] {
        let mut r = release("0.3.1");
        r.artifacts[0].path = path.into();
        assert!(matches!(manifest(vec![r]).validate(), Err(ManifestError::BadArtifact { .. })), "{path} was accepted");
    }
}

#[test]
fn hashes_sizes_and_build_are_checked() {
    let mut r = release("0.3.1");
    r.artifacts[0].sha256 = "abc".into();
    assert!(matches!(manifest(vec![r]).validate(), Err(ManifestError::BadArtifact { .. })));
    let mut r = release("0.3.1");
    r.artifacts[0].sha256 = common::sha('E');
    assert!(matches!(manifest(vec![r]).validate(), Err(ManifestError::BadArtifact { .. })));
    let mut r = release("0.3.1");
    r.artifacts[0].size = 0;
    assert!(matches!(manifest(vec![r]).validate(), Err(ManifestError::BadArtifact { .. })));
    let mut r = release("0.3.1");
    r.build = "abc123".into();
    assert_eq!(manifest(vec![r]).validate(), Err(ManifestError::BadBuild("0.3.1".into())));
}

#[test]
fn a_channel_may_not_point_at_a_yanked_or_missing_release() {
    let mut r = release("0.3.1");
    r.yanked = true;
    let mut m = manifest(vec![r]);
    m.channels.stable = Some("0.3.1".into());
    assert!(matches!(m.validate(), Err(ManifestError::BadChannel { .. })));
    let mut m = manifest(vec![]);
    m.channels.preview = Some("9.9.9-preview.1".into());
    assert!(matches!(m.validate(), Err(ManifestError::BadChannel { .. })));
}

#[test]
fn only_schema_1_is_read() {
    let mut m = manifest(vec![]);
    m.schema = 2;
    assert_eq!(m.validate(), Err(ManifestError::UnsupportedSchema(2)));
}

#[test]
fn timestamps_are_utc_rfc3339_and_dates_are_iso() {
    let mut m = manifest(vec![]);
    m.expires = "2026-12-30T12:00:00+02:00".into();
    assert!(matches!(m.validate(), Err(ManifestError::BadTimestamp(_))));
    let mut r = release("0.3.1");
    r.change_date = "2030/10/01".into();
    assert!(matches!(manifest(vec![r]).validate(), Err(ManifestError::BadDate(_))));
}

#[test]
fn base_url_is_https_with_a_trailing_slash() {
    for url in ["http://downloads.starworks.dev/", "https://downloads.starworks.dev"] {
        let mut m = manifest(vec![]);
        m.base_url = url.into();
        assert!(matches!(m.validate(), Err(ManifestError::BadBaseUrl(_))), "{url} was accepted");
    }
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test manifest_test`
Expected: FAIL to compile, with `file not found for module` for `manifest` / `version`.

- [ ] **Step 4: Write `version.rs`**

```rust
//! Release versions: semver, with exactly one prerelease form, `preview.N`
//! (spec s4: `v0.3.0` stable, `v0.3.0-preview.N` previews). Semver's own
//! ordering does the rest: preview.10 > preview.2, and 0.4.0 > 0.4.0-preview.N.

use semver::Version;

use crate::manifest::Channel;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum VersionError {
    #[error("not a semver version: {0}")]
    NotSemver(String),
    #[error("{0}: the only prerelease form is preview.N (e.g. 0.4.0-preview.2)")]
    BadPrerelease(String),
    #[error("{0}: build metadata (+...) is not allowed in a release version")]
    BuildMetadata(String),
}

pub fn parse(v: &str) -> Result<Version, VersionError> {
    let parsed = Version::parse(v).map_err(|_| VersionError::NotSemver(v.to_string()))?;
    if !parsed.build.is_empty() {
        return Err(VersionError::BuildMetadata(v.to_string()));
    }
    channel_of(&parsed)?;
    Ok(parsed)
}

pub fn channel_of(v: &Version) -> Result<Channel, VersionError> {
    if v.pre.is_empty() {
        return Ok(Channel::Stable);
    }
    // semver already refuses leading zeros in numeric identifiers.
    match v.pre.as_str().strip_prefix("preview.") {
        Some(n) if !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()) => Ok(Channel::Preview),
        _ => Err(VersionError::BadPrerelease(v.to_string())),
    }
}

/// major.minor.patch without the prerelease: 0.4.0-preview.2 -> 0.4.0.
pub fn base(v: &Version) -> Version {
    Version::new(v.major, v.minor, v.patch)
}
```

- [ ] **Step 5: Write `manifest.rs`**

```rust
//! `releases.json`, schema 1 (spec s5). serde ignores fields it doesn't know,
//! so a later schema-1 manifest may ADD fields; a change an older Hub would
//! misread bumps `schema` instead. `kind`, `os` and `arch` are strings, not
//! enums, so a new artifact kind never breaks an older Hub's parse.

use std::collections::HashSet;

use serde::{Deserialize, Serialize};
use time::format_description::well_known::Rfc3339;
use time::macros::format_description;
use time::{Date, OffsetDateTime, UtcOffset};

use crate::version::{self, VersionError};

pub const SCHEMA: u32 = 1;
pub const BASE_URL: &str = "https://downloads.starworks.dev/";
pub const NOTES_URL_BASE: &str = "https://starworks.dev/arcane/releases#";
pub const LICENSE_ID: &str = "BUSL-1.1";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Channel {
    Stable,
    Preview,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Artifact {
    pub os: String,
    pub arch: String,
    pub kind: String,
    pub path: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Release {
    pub version: String,
    pub channel: Channel,
    pub released_at: String,
    /// The release's full git commit SHA (the probe's `commit`), not the
    /// probe's display string `build` (spec s5).
    pub build: String,
    pub abi: u32,
    pub license: String,
    pub change_date: String,
    #[serde(default)]
    pub min_hub_version: Option<String>,
    #[serde(default)]
    pub yanked: bool,
    #[serde(default)]
    pub yank_reason: Option<String>,
    #[serde(default)]
    pub security: bool,
    pub notes_url: String,
    #[serde(default)]
    pub notes: String,
    pub artifacts: Vec<Artifact>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Channels {
    pub stable: Option<String>,
    pub preview: Option<String>,
}

/// The Hub's own release line (spec s5 `hub`); filled in by phase 4.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct HubInfo {
    pub latest: Option<String>,
    pub min_supported: Option<String>,
    #[serde(default)]
    pub artifacts: Vec<Artifact>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Manifest {
    pub schema: u32,
    pub serial: u64,
    pub generated_at: String,
    pub expires: String,
    pub base_url: String,
    pub channels: Channels,
    #[serde(default)]
    pub hub: HubInfo,
    pub releases: Vec<Release>,
}

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ManifestError {
    #[error("unsupported manifest schema {0} (this build reads schema {SCHEMA}); update the Hub")]
    UnsupportedSchema(u32),
    #[error(transparent)]
    Version(#[from] VersionError),
    #[error("{0} is listed more than once")]
    DuplicateVersion(String),
    #[error("{version}: channel is {got:?} but the version says {want:?}")]
    ChannelMismatch { version: String, got: Channel, want: Channel },
    #[error("{0}: not an RFC 3339 UTC timestamp ending in Z")]
    BadTimestamp(String),
    #[error("{0}: not a YYYY-MM-DD date")]
    BadDate(String),
    #[error("{version}: artifact {path:?}: {why}")]
    BadArtifact { version: String, path: String, why: &'static str },
    #[error("channel {channel} points at {version}, which is missing or yanked")]
    BadChannel { channel: &'static str, version: String },
    #[error("base_url must be https and end with '/': {0}")]
    BadBaseUrl(String),
    #[error("{0}: build must be a full 40-character lowercase git SHA")]
    BadBuild(String),
}

impl Manifest {
    pub fn parse(bytes: &[u8]) -> Result<Manifest, serde_json::Error> {
        serde_json::from_slice(bytes)
    }

    /// The exact bytes that are signed and uploaded.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut bytes = serde_json::to_vec_pretty(self).expect("a manifest always serializes");
        bytes.push(b'\n');
        bytes
    }

    pub fn release(&self, version: &str) -> Option<&Release> {
        self.releases.iter().find(|r| r.version == version)
    }

    pub fn validate(&self) -> Result<(), ManifestError> {
        if self.schema != SCHEMA {
            return Err(ManifestError::UnsupportedSchema(self.schema));
        }
        if !(self.base_url.starts_with("https://") && self.base_url.ends_with('/')) {
            return Err(ManifestError::BadBaseUrl(self.base_url.clone()));
        }
        parse_ts(&self.generated_at)?;
        parse_ts(&self.expires)?;
        let mut seen = HashSet::new();
        for r in &self.releases {
            let v = version::parse(&r.version)?;
            if !seen.insert(r.version.as_str()) {
                return Err(ManifestError::DuplicateVersion(r.version.clone()));
            }
            let want = version::channel_of(&v)?;
            if r.channel != want {
                return Err(ManifestError::ChannelMismatch { version: r.version.clone(), got: r.channel, want });
            }
            parse_ts(&r.released_at)?;
            parse_date(&r.change_date)?;
            if !is_lower_hex(&r.build, 40) {
                return Err(ManifestError::BadBuild(r.version.clone()));
            }
            for a in &r.artifacts {
                check_artifact(&r.version, a)?;
            }
        }
        for (channel, pick) in [("stable", &self.channels.stable), ("preview", &self.channels.preview)] {
            if let Some(v) = pick {
                if !matches!(self.release(v), Some(r) if !r.yanked) {
                    return Err(ManifestError::BadChannel { channel, version: v.clone() });
                }
            }
        }
        Ok(())
    }
}

fn check_artifact(version: &str, a: &Artifact) -> Result<(), ManifestError> {
    let bad = |why: &'static str| ManifestError::BadArtifact { version: version.to_string(), path: a.path.clone(), why };
    let folder = format!("engine/{version}/");
    let name = a.path.strip_prefix(&folder).ok_or_else(|| bad("not under engine/<version>/"))?;
    if name.is_empty() || name.contains('/') || name.contains('\\') || name.starts_with('.') {
        return Err(bad("not a plain file name"));
    }
    if !is_lower_hex(&a.sha256, 64) {
        return Err(bad("sha256 is not 64 lowercase hex digits"));
    }
    if a.size == 0 {
        return Err(bad("size is 0"));
    }
    Ok(())
}

pub fn is_lower_hex(s: &str, len: usize) -> bool {
    s.len() == len && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

pub fn parse_ts(s: &str) -> Result<OffsetDateTime, ManifestError> {
    match OffsetDateTime::parse(s, &Rfc3339) {
        Ok(t) if t.offset() == UtcOffset::UTC && s.ends_with('Z') => Ok(t),
        _ => Err(ManifestError::BadTimestamp(s.to_string())),
    }
}

pub fn format_ts(t: OffsetDateTime) -> String {
    let t = t.to_offset(UtcOffset::UTC).replace_nanosecond(0).expect("0 is a valid nanosecond");
    t.format(&Rfc3339).expect("RFC 3339 formats any UTC time")
}

pub fn parse_date(s: &str) -> Result<Date, ManifestError> {
    Date::parse(s, format_description!("[year]-[month]-[day]")).map_err(|_| ManifestError::BadDate(s.to_string()))
}

pub fn format_date(d: Date) -> String {
    d.format(format_description!("[year]-[month]-[day]")).expect("a date always formats")
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test manifest_test`
Expected: PASS, 14 tests.

Also run `cargo check --no-default-features --lib`. Expected: it compiles, which proves the Hub's dependency builds.

- [ ] **Step 7: Commit**

```bash
git add ArcaneHub/crates/arcane_release
git commit -m "feat(release): arcane_release crate -- version rules and the releases.json schema (distribution spec s5)"
```

---

### Task 2: Signing, verification and the trusted keys

**Files:**
- Create: `ArcaneHub/crates/arcane_release/src/sign.rs`
- Create: `ArcaneHub/crates/arcane_release/src/verify.rs`
- Create: `ArcaneHub/crates/arcane_release/src/keys.rs`
- Create: `ArcaneHub/crates/arcane_release/src/bin/arcrelease.rs` (keygen only; Task 5 completes it)
- Create: `ArcaneHub/crates/arcane_release/tests/keys/test-manifest-1.{key,pub}`, `test-manifest-2.{key,pub}` (generated)
- Modify: `ArcaneHub/crates/arcane_release/src/lib.rs`
- Modify: `ArcaneHub/crates/arcane_release/tests/common/mod.rs`
- Modify: `.gitattributes`
- Test: `ArcaneHub/crates/arcane_release/tests/verify_test.rs`

**Interfaces:**
- Consumes: `Manifest`, `ManifestError`, `parse_ts`, `SCHEMA` (Task 1).
- Produces:
  - `sign::generate(comment, password) -> Result<GeneratedKey, SignError>`;
  - `sign::open_secret_key(secret_key_file, password) -> Result<minisign::SecretKey, SignError>`;
  - `sign::sign(&SecretKey, &[u8], trusted_comment) -> Result<String, SignError>`;
  - `verify::parse_public_key(&str) -> Option<PublicKey>`;
  - `verify::verify_signed(bytes, sig_text, keys) -> Result<Manifest, VerifyError>`: signature, schema and contents;
  - `verify::verify_manifest(bytes, sig_text, keys, highest_serial_seen: Option<u64>, now) -> Result<Manifest, VerifyError>`: everything the Hub checks;
  - `keys::TRUSTED_MANIFEST_KEYS: &[&str]` and `keys::trusted() -> Vec<PublicKey>`;
  - the test key files and their password, `arcane-test-key-not-a-secret`.

- [ ] **Step 1: Keep the key and golden files byte-exact**

The keys and signed goldens must never get CRLF line endings. A `\r` breaks base64 decoding and every signature. Append to `.gitattributes`:

```
# arcane_release: test keys and signed golden manifests are byte-exact (a CRLF breaks them)
ArcaneHub/crates/arcane_release/tests/keys/** -text
ArcaneHub/crates/arcane_release/tests/golden/** -text
ArcaneHub/crates/arcane_release/keys/** -text
```

- [ ] **Step 2: Write `sign.rs`, `keys.rs`, `verify.rs` and the keygen binary**

`src/sign.rs`:

```rust
//! minisign signing (spec s5). Keys are always encrypted, and always opened
//! with a password: minisign's `into_secret_key(None)` prompts on the
//! terminal, which would hang CI.

use std::io::Cursor;

use minisign::{KeyPair, SecretKey, SecretKeyBox};

#[derive(Debug, thiserror::Error)]
pub enum SignError {
    #[error("the secret key could not be opened (wrong password, or not a minisign secret key): {0}")]
    Open(String),
    #[error("an empty password is not allowed")]
    EmptyPassword,
    #[error("signing failed: {0}")]
    Sign(String),
}

pub struct GeneratedKey {
    /// The .pub file's text: an untrusted comment line, then the key in base64.
    pub public_key_file: String,
    /// The .key file's text (encrypted).
    pub secret_key_file: String,
    pub public_key_base64: String,
}

pub fn generate(comment: &str, password: &str) -> Result<GeneratedKey, SignError> {
    if password.is_empty() {
        return Err(SignError::EmptyPassword);
    }
    let (mut public, mut secret) = (Vec::new(), Vec::new());
    let pair = KeyPair::generate_and_write_encrypted_keypair(&mut public, &mut secret, Some(comment), Some(password.to_string()))
        .map_err(|e| SignError::Sign(e.to_string()))?;
    Ok(GeneratedKey {
        public_key_file: String::from_utf8(public).expect("minisign writes UTF-8"),
        secret_key_file: String::from_utf8(secret).expect("minisign writes UTF-8"),
        public_key_base64: pair.pk.to_base64(),
    })
}

pub fn open_secret_key(secret_key_file: &str, password: &str) -> Result<SecretKey, SignError> {
    if password.is_empty() {
        return Err(SignError::EmptyPassword);
    }
    SecretKeyBox::from_string(secret_key_file)
        .and_then(|b| b.into_secret_key(Some(password.to_string())))
        .map_err(|e| SignError::Open(e.to_string()))
}

/// Signs `bytes` and returns the .minisig file's text. The trusted comment is
/// covered by the signature.
pub fn sign(sk: &SecretKey, bytes: &[u8], trusted_comment: &str) -> Result<String, SignError> {
    minisign::sign(None, sk, Cursor::new(bytes), Some(trusted_comment), Some("signature from the Arcane release key"))
        .map(|b| b.into_string())
        .map_err(|e| SignError::Sign(e.to_string()))
}
```

`src/keys.rs`:

```rust
//! The manifest public keys release builds trust (spec s5, "Keys"). This is a
//! LIST so a key can be rotated: ship the successor here in a Hub release
//! before the old key retires (docs/releasing.md, "Keys"). The test keys under
//! tests/keys/ must never appear here; verify_test checks that.

use minisign_verify::PublicKey;

pub const TRUSTED_MANIFEST_KEYS: &[&str] = &[
    // The production key's base64 line (keys/arcane-manifest-1.pub) is added
    // by the owner's key ceremony, plan task 13.
];

pub fn trusted() -> Vec<PublicKey> {
    TRUSTED_MANIFEST_KEYS
        .iter()
        .map(|k| PublicKey::from_base64(k).expect("TRUSTED_MANIFEST_KEYS holds valid keys (verify_test checks)"))
        .collect()
}
```

`src/verify.rs`:

```rust
//! Manifest verification (spec s5, "How the Hub checks it"). Nothing in the
//! file is parsed until its signature verifies against a trusted key.

use minisign_verify::{PublicKey, Signature};
use serde::Deserialize;
use time::OffsetDateTime;

use crate::manifest::{parse_ts, Manifest, ManifestError, SCHEMA};

#[derive(Debug, thiserror::Error)]
pub enum VerifyError {
    #[error("no trusted keys were given")]
    NoKeys,
    #[error("the signature file is malformed")]
    MalformedSignature,
    #[error("the manifest's signature does not verify against any trusted key")]
    BadSignature,
    #[error("the manifest is not valid schema-{SCHEMA} JSON: {0}")]
    Malformed(String),
    #[error(transparent)]
    Invalid(#[from] ManifestError),
    #[error("rollback refused: serial {got} is lower than the highest seen, {seen}")]
    Rollback { seen: u64, got: u64 },
    #[error("the manifest expired at {0}")]
    Expired(String),
}

/// Reads a .pub file's text (or a bare base64 line).
pub fn parse_public_key(text: &str) -> Option<PublicKey> {
    let line = text
        .lines()
        .map(str::trim)
        .rfind(|l| !l.is_empty() && !l.starts_with("untrusted comment:"))?;
    PublicKey::from_base64(line).ok()
}

/// The signature, the schema and the contents, with no serial or expiry check.
/// CI reads the manifest it is about to replace with this: an expired manifest
/// must still be re-signable.
pub fn verify_signed(bytes: &[u8], sig_text: &str, keys: &[PublicKey]) -> Result<Manifest, VerifyError> {
    if keys.is_empty() {
        return Err(VerifyError::NoKeys);
    }
    let sig = Signature::decode(sig_text).map_err(|_| VerifyError::MalformedSignature)?;
    if !keys.iter().any(|k| k.verify(bytes, &sig, false).is_ok()) {
        return Err(VerifyError::BadSignature);
    }
    #[derive(Deserialize)]
    struct SchemaProbe {
        schema: u32,
    }
    let probe: SchemaProbe = serde_json::from_slice(bytes).map_err(|e| VerifyError::Malformed(e.to_string()))?;
    if probe.schema != SCHEMA {
        return Err(ManifestError::UnsupportedSchema(probe.schema).into());
    }
    let manifest = Manifest::parse(bytes).map_err(|e| VerifyError::Malformed(e.to_string()))?;
    manifest.validate()?;
    Ok(manifest)
}

/// Everything the Hub checks: the signature against its trusted keys,
/// no rollback below the highest serial it has seen (an equal serial is the
/// same manifest fetched again), and not expired.
pub fn verify_manifest(
    bytes: &[u8],
    sig_text: &str,
    keys: &[PublicKey],
    highest_serial_seen: Option<u64>,
    now: OffsetDateTime,
) -> Result<Manifest, VerifyError> {
    let manifest = verify_signed(bytes, sig_text, keys)?;
    if let Some(seen) = highest_serial_seen {
        if manifest.serial < seen {
            return Err(VerifyError::Rollback { seen, got: manifest.serial });
        }
    }
    if now >= parse_ts(&manifest.expires)? {
        return Err(VerifyError::Expired(manifest.expires.clone()));
    }
    Ok(manifest)
}
```

`src/bin/arcrelease.rs` (keygen only for now):

```rust
//! `arcrelease`: Arcane's release tooling. Task 5 adds the manifest commands.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{ensure, Context, Result};
use arcane_release::sign;
use clap::{Parser, Subcommand};

const PASSWORD_ENV: &str = "ARCANE_MINISIGN_PASSWORD";

#[derive(Parser)]
#[command(name = "arcrelease", version, about = "Arcane release tooling: manifests, signing, archives")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Generate an encrypted minisign key pair; the password comes from ARCANE_MINISIGN_PASSWORD.
    Keygen {
        #[arg(long)]
        out_dir: PathBuf,
        #[arg(long)]
        name: String,
    },
}

fn main() -> Result<()> {
    match Cli::parse().cmd {
        Cmd::Keygen { out_dir, name } => keygen(&out_dir, &name),
    }
}

fn keygen(out_dir: &Path, name: &str) -> Result<()> {
    let password = std::env::var(PASSWORD_ENV).with_context(|| format!("set {PASSWORD_ENV} to the key's password"))?;
    let (pub_path, key_path) = (out_dir.join(format!("{name}.pub")), out_dir.join(format!("{name}.key")));
    ensure!(!pub_path.exists() && !key_path.exists(), "{name}.pub or {name}.key already exists in {}", out_dir.display());
    let generated = sign::generate(name, &password)?;
    fs::create_dir_all(out_dir)?;
    fs::write(&pub_path, &generated.public_key_file)?;
    fs::write(&key_path, &generated.secret_key_file)?;
    println!("{}", generated.public_key_base64);
    Ok(())
}
```

Update `src/lib.rs`'s module list:

```rust
pub mod keys;
pub mod manifest;
pub mod verify;
pub mod version;

#[cfg(feature = "tools")]
pub mod sign;
```

- [ ] **Step 3: Generate the two test keys**

Run (bash):

```bash
cd ArcaneHub/crates/arcane_release
export ARCANE_MINISIGN_PASSWORD=arcane-test-key-not-a-secret
cargo run -q --bin arcrelease -- keygen --out-dir tests/keys --name test-manifest-1
cargo run -q --bin arcrelease -- keygen --out-dir tests/keys --name test-manifest-2
unset ARCANE_MINISIGN_PASSWORD
```

Expected: two base64 lines printed, and four files in `tests/keys/`. Then add `tests/keys/README.md`:

```markdown
# Test manifest keys

`test-manifest-1` and `test-manifest-2` are TEST keys. Their password is
`arcane-test-key-not-a-secret`, and they are committed on purpose. Tests,
golden manifests, dry runs and staging rehearsals use them. No Hub trusts
them: `keys::TRUSTED_MANIFEST_KEYS` must never contain them, and
`verify_test` checks this. Key 2 exists to test key rotation.
```

- [ ] **Step 4: Add the key helpers to `tests/common/mod.rs`**

Append:

```rust
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
```

- [ ] **Step 5: Write the failing tests**

`tests/verify_test.rs`:

```rust
mod common;

use arcane_release::manifest::ManifestError;
use arcane_release::sign::{self, SignError};
use arcane_release::verify::{self, VerifyError};
use arcane_release::keys;
use time::macros::datetime;
use time::OffsetDateTime;

const NOW: OffsetDateTime = datetime!(2026-10-12 00:00 UTC);

fn signed_manifest(serial: u64) -> (Vec<u8>, String) {
    let mut m = common::manifest(vec![common::release("0.3.1")]);
    m.serial = serial;
    let bytes = m.to_bytes();
    let sig = sign::sign(common::test_secret(1), &bytes, "test").unwrap();
    (bytes, sig)
}

#[test]
fn a_signed_manifest_verifies() {
    let (b, s) = signed_manifest(3);
    let m = verify::verify_manifest(&b, &s, &[common::test_public(1)], None, NOW).unwrap();
    assert_eq!(m.serial, 3);
}

#[test]
fn one_changed_byte_fails_the_signature() {
    let (mut b, s) = signed_manifest(3);
    let i = b.iter().position(|&c| c == b'3').unwrap(); // the serial: still valid JSON after the edit
    b[i] = b'4';
    assert!(matches!(verify::verify_manifest(&b, &s, &[common::test_public(1)], None, NOW), Err(VerifyError::BadSignature)));
}

#[test]
fn garbage_is_stopped_by_the_signature_before_it_is_parsed() {
    let (_, s) = signed_manifest(3);
    assert!(matches!(verify::verify_manifest(b"not json", &s, &[common::test_public(1)], None, NOW), Err(VerifyError::BadSignature)));
}

#[test]
fn another_key_fails_and_a_key_list_allows_rotation() {
    let (b, _) = signed_manifest(3);
    let s2 = sign::sign(common::test_secret(2), &b, "test").unwrap();
    assert!(matches!(verify::verify_manifest(&b, &s2, &[common::test_public(1)], None, NOW), Err(VerifyError::BadSignature)));
    verify::verify_manifest(&b, &s2, &[common::test_public(1), common::test_public(2)], None, NOW).unwrap();
}

#[test]
fn no_keys_and_malformed_signatures_are_errors() {
    let (b, s) = signed_manifest(3);
    assert!(matches!(verify::verify_manifest(&b, &s, &[], None, NOW), Err(VerifyError::NoKeys)));
    assert!(matches!(verify::verify_manifest(&b, "nonsense", &[common::test_public(1)], None, NOW), Err(VerifyError::MalformedSignature)));
}

#[test]
fn a_lower_serial_is_a_rollback_and_an_equal_one_is_fine() {
    let (b, s) = signed_manifest(4);
    let keys = [common::test_public(1)];
    assert!(matches!(verify::verify_manifest(&b, &s, &keys, Some(5), NOW), Err(VerifyError::Rollback { seen: 5, got: 4 })));
    verify::verify_manifest(&b, &s, &keys, Some(4), NOW).unwrap();
}

#[test]
fn expiry_is_exclusive() {
    let (b, s) = signed_manifest(3); // expires 2026-12-30T12:00:00Z
    let keys = [common::test_public(1)];
    assert!(matches!(verify::verify_manifest(&b, &s, &keys, None, datetime!(2026-12-30 12:00 UTC)), Err(VerifyError::Expired(_))));
    verify::verify_manifest(&b, &s, &keys, None, datetime!(2026-12-30 11:59:59 UTC)).unwrap();
}

#[test]
fn verify_signed_ignores_serial_and_expiry() {
    let (b, s) = signed_manifest(3);
    verify::verify_signed(&b, &s, &[common::test_public(1)]).unwrap();
}

#[test]
fn an_unknown_schema_is_reported_as_such() {
    let bytes = b"{\"schema\": 2, \"serial\": 9}\n";
    let s = sign::sign(common::test_secret(1), bytes, "test").unwrap();
    let got = verify::verify_manifest(bytes, &s, &[common::test_public(1)], None, NOW);
    assert!(matches!(got, Err(VerifyError::Invalid(ManifestError::UnsupportedSchema(2)))));
}

#[test]
fn a_signed_but_invalid_manifest_is_refused() {
    let m = common::manifest(vec![common::release("0.3.1"), common::release("0.3.1")]);
    let bytes = m.to_bytes();
    let s = sign::sign(common::test_secret(1), &bytes, "test").unwrap();
    let got = verify::verify_manifest(&bytes, &s, &[common::test_public(1)], None, NOW);
    assert!(matches!(got, Err(VerifyError::Invalid(ManifestError::DuplicateVersion(_)))));
}

#[test]
fn the_secret_key_needs_its_password() {
    let text = std::fs::read_to_string(common::key_dir().join("test-manifest-1.key")).unwrap();
    assert!(matches!(sign::open_secret_key(&text, "wrong"), Err(SignError::Open(_))));
    assert!(matches!(sign::open_secret_key(&text, ""), Err(SignError::EmptyPassword)));
}

#[test]
fn keygen_produces_a_working_pair() {
    let g = sign::generate("unit", "pw").unwrap();
    let sk = sign::open_secret_key(&g.secret_key_file, "pw").unwrap();
    let pk = verify::parse_public_key(&g.public_key_file).unwrap();
    let bytes = common::manifest(vec![]).to_bytes();
    let s = sign::sign(&sk, &bytes, "unit").unwrap();
    verify::verify_manifest(&bytes, &s, &[pk], None, NOW).unwrap();
    assert!(verify::parse_public_key(&g.public_key_base64).is_some());
}

#[test]
fn every_trusted_key_parses_and_no_test_key_is_trusted() {
    for k in keys::TRUSTED_MANIFEST_KEYS {
        assert!(minisign_verify::PublicKey::from_base64(k).is_ok(), "bad trusted key {k}");
    }
    for n in [1, 2] {
        let line = common::test_public_line(n);
        assert!(!keys::TRUSTED_MANIFEST_KEYS.contains(&line.as_str()), "test key {n} is trusted");
    }
}
```

- [ ] **Step 6: Run the tests**

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test verify_test --test manifest_test`
Expected: PASS: 13 verify tests and 14 manifest tests. The steps are ordered so the code lands before the tests. Before committing, confirm the tests can fail: temporarily change `k.verify(bytes, &sig, false).is_ok()` to `true` in `verify.rs`. Expect `one_changed_byte_fails_the_signature`, `garbage_is_stopped...` and `another_key_fails...` to FAIL, then restore the line.

- [ ] **Step 7: Commit**

```bash
git add .gitattributes ArcaneHub/crates/arcane_release
git commit -m "feat(release): minisign signing and Hub-equivalent manifest verification; committed test keys (never trusted)"
```

---

### Task 3: Manifest operations (add, yank, re-sign, channels, change dates)

**Files:**
- Create: `ArcaneHub/crates/arcane_release/src/ops.rs`
- Modify: `ArcaneHub/crates/arcane_release/src/lib.rs` (add `pub mod ops;`)
- Test: `ArcaneHub/crates/arcane_release/tests/ops_test.rs`

**Interfaces:**
- Consumes: Task 1's types and time helpers; `version::{parse, channel_of, base}`.
- Produces:
  - `ops::EXPIRY_DAYS = 90` and `ops::CHANGE_YEARS = 4`;
  - `ops::NewRelease { version, build, abi, released_at: OffsetDateTime, min_hub_version: Option<String>, security: bool, notes: String, artifacts: Vec<Artifact> }`;
  - `ops::empty(base_url, now) -> Manifest`, at serial 1;
  - `ops::add_release(&Manifest, NewRelease, now) -> Result<Manifest, OpError>`;
  - `ops::yank(&Manifest, version, reason, now) -> Result<Manifest, OpError>`;
  - `ops::resign(&Manifest, now) -> Result<Manifest, OpError>`;
  - `ops::change_date(&Manifest, &Version, released: Date) -> Date`;
  - `OpError`.

- [ ] **Step 1: Write the failing tests**

`tests/ops_test.rs`:

```rust
mod common;

use arcane_release::manifest::{format_ts, parse_ts, Channel, BASE_URL};
use arcane_release::ops::{self, NewRelease, OpError, EXPIRY_DAYS};
use time::macros::datetime;
use time::{Duration, OffsetDateTime};

const T0: OffsetDateTime = datetime!(2026-10-12 09:30 UTC);

fn new(v: &str, at: OffsetDateTime) -> NewRelease {
    NewRelease {
        version: v.into(),
        build: common::BUILD.into(),
        abi: 59,
        released_at: at,
        min_hub_version: None,
        security: false,
        notes: format!("Notes for {v}."),
        artifacts: vec![common::artifact(v, "windows", "engine"), common::artifact(v, "windows", "symbols")],
    }
}

#[test]
fn a_new_manifest_is_serial_1_and_expires_in_90_days() {
    let m = ops::empty(BASE_URL, T0);
    assert_eq!(m.serial, 1);
    assert_eq!(m.generated_at, "2026-10-12T09:30:00Z");
    assert_eq!(parse_ts(&m.expires).unwrap(), T0 + Duration::days(EXPIRY_DAYS));
    assert!(m.releases.is_empty());
    m.validate().unwrap();
}

#[test]
fn adding_a_release_fills_it_in_and_bumps_the_serial() {
    let m = ops::add_release(&ops::empty(BASE_URL, T0), new("0.3.1", T0), T0).unwrap();
    assert_eq!(m.serial, 2);
    let r = m.release("0.3.1").unwrap();
    assert_eq!(r.channel, Channel::Stable);
    assert_eq!(r.released_at, "2026-10-12T09:30:00Z");
    assert_eq!(r.change_date, "2030-10-12");
    assert_eq!(r.license, "BUSL-1.1");
    assert_eq!(r.notes_url, "https://starworks.dev/arcane/releases#0.3.1");
    assert!(!r.yanked);
    assert_eq!(m.channels.stable.as_deref(), Some("0.3.1"));
    assert_eq!(m.channels.preview.as_deref(), Some("0.3.1"));
}

#[test]
fn a_published_version_is_never_replaced() {
    let m = ops::add_release(&ops::empty(BASE_URL, T0), new("0.3.1", T0), T0).unwrap();
    assert_eq!(ops::add_release(&m, new("0.3.1", T0), T0), Err(OpError::AlreadyPublished("0.3.1".into())));
}

#[test]
fn a_release_needs_an_engine_artifact_and_notes() {
    let mut r = new("0.3.1", T0);
    r.artifacts.retain(|a| a.kind == "symbols");
    assert_eq!(ops::add_release(&ops::empty(BASE_URL, T0), r, T0), Err(OpError::NoEngineArtifact("0.3.1".into())));
    let mut r = new("0.3.1", T0);
    r.notes = "  \n".into();
    assert_eq!(ops::add_release(&ops::empty(BASE_URL, T0), r, T0), Err(OpError::EmptyNotes("0.3.1".into())));
}

#[test]
fn the_change_date_runs_from_the_first_preview() {
    let m = ops::empty(BASE_URL, T0);
    let m = ops::add_release(&m, new("0.4.0-preview.1", datetime!(2026-11-01 10:00 UTC)), T0).unwrap();
    let m = ops::add_release(&m, new("0.4.0-preview.2", datetime!(2026-12-01 10:00 UTC)), T0).unwrap();
    let m = ops::add_release(&m, new("0.4.0", datetime!(2027-01-15 10:00 UTC)), T0).unwrap();
    assert_eq!(m.release("0.4.0-preview.2").unwrap().change_date, "2030-11-01");
    assert_eq!(m.release("0.4.0").unwrap().change_date, "2030-11-01");
    let m = ops::add_release(&m, new("0.4.1", datetime!(2027-02-01 10:00 UTC)), T0).unwrap();
    assert_eq!(m.release("0.4.1").unwrap().change_date, "2031-02-01");
}

#[test]
fn a_leap_day_change_date_falls_back_to_february_28() {
    let m = ops::add_release(&ops::empty(BASE_URL, T0), new("0.3.1", datetime!(2028-02-29 12:00 UTC)), T0).unwrap();
    assert_eq!(m.release("0.3.1").unwrap().change_date, "2032-02-29");
    let m = ops::add_release(&ops::empty(BASE_URL, T0), new("0.3.1", datetime!(2096-02-29 12:00 UTC)), T0).unwrap();
    assert_eq!(m.release("0.3.1").unwrap().change_date, "2100-02-28");
}

#[test]
fn channels_follow_the_newest_live_release() {
    let mut m = ops::empty(BASE_URL, T0);
    for v in ["0.3.0", "0.3.1", "0.4.0-preview.1", "0.4.0-preview.2"] {
        m = ops::add_release(&m, new(v, T0), T0).unwrap();
    }
    assert_eq!(m.channels.stable.as_deref(), Some("0.3.1"));
    assert_eq!(m.channels.preview.as_deref(), Some("0.4.0-preview.2"));
    let m = ops::yank(&m, "0.4.0-preview.2", "Crashes on open.", T0).unwrap();
    assert_eq!(m.channels.preview.as_deref(), Some("0.4.0-preview.1"));
    let m = ops::yank(&m, "0.3.1", "Bad build.", T0).unwrap();
    assert_eq!(m.channels.stable.as_deref(), Some("0.3.0"));
    let m = ops::add_release(&m, new("0.4.0", T0), T0).unwrap();
    assert_eq!(m.channels.stable.as_deref(), Some("0.4.0"));
    assert_eq!(m.channels.preview.as_deref(), Some("0.4.0"));
}

#[test]
fn releases_are_listed_newest_first() {
    let mut m = ops::empty(BASE_URL, T0);
    for v in ["0.3.1", "0.4.0-preview.1", "0.3.0"] {
        m = ops::add_release(&m, new(v, T0), T0).unwrap();
    }
    let order: Vec<&str> = m.releases.iter().map(|r| r.version.as_str()).collect();
    assert_eq!(order, ["0.4.0-preview.1", "0.3.1", "0.3.0"]);
}

#[test]
fn yanking_keeps_the_dates_and_records_the_reason() {
    let m = ops::add_release(&ops::empty(BASE_URL, T0), new("0.3.1", T0), T0).unwrap();
    let before = m.release("0.3.1").unwrap().clone();
    let later = T0 + Duration::days(20);
    let y = ops::yank(&m, "0.3.1", "  Corrupts scenes.  ", later).unwrap();
    let r = y.release("0.3.1").unwrap();
    assert!(r.yanked);
    assert_eq!(r.yank_reason.as_deref(), Some("Corrupts scenes."));
    assert_eq!(r.released_at, before.released_at);
    assert_eq!(r.change_date, before.change_date);
    assert_eq!(y.serial, m.serial + 1);
    let again = ops::yank(&y, "0.3.1", "Corrupts scenes on reload.", later).unwrap();
    assert_eq!(again.release("0.3.1").unwrap().yank_reason.as_deref(), Some("Corrupts scenes on reload."));
    assert_eq!(again.release("0.3.1").unwrap().released_at, before.released_at);
}

#[test]
fn yank_needs_a_known_version_and_a_reason() {
    let m = ops::add_release(&ops::empty(BASE_URL, T0), new("0.3.1", T0), T0).unwrap();
    assert_eq!(ops::yank(&m, "9.9.9", "x", T0), Err(OpError::UnknownVersion("9.9.9".into())));
    assert_eq!(ops::yank(&m, "0.3.1", "   ", T0), Err(OpError::EmptyReason));
}

#[test]
fn resigning_changes_only_the_serial_and_the_dates() {
    let m = ops::add_release(&ops::empty(BASE_URL, T0), new("0.3.1", T0), T0).unwrap();
    let later = T0 + Duration::days(30);
    let r = ops::resign(&m, later).unwrap();
    assert_eq!(r.serial, m.serial + 1);
    assert_eq!(r.generated_at, format_ts(later));
    assert_eq!(parse_ts(&r.expires).unwrap(), later + Duration::days(EXPIRY_DAYS));
    assert_eq!(r.releases, m.releases);
    assert_eq!(r.channels, m.channels);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test ops_test`
Expected: FAIL to compile with `unresolved import arcane_release::ops`.

- [ ] **Step 3: Write `src/ops.rs`** and add `pub mod ops;` to `lib.rs`

```rust
//! The operations CI applies to releases.json (spec s4, s5). Each returns a
//! new manifest with the serial bumped and a fresh 90-day expiry, the
//! channels recomputed and the result validated. None rewrites an existing
//! release's released_at or change_date: those are the BSL dates (spec s12).

use semver::Version;
use time::{Date, Duration, Month, OffsetDateTime, UtcOffset};

use crate::manifest::{
    format_date, format_ts, parse_ts, Artifact, Channel, Channels, HubInfo, Manifest, ManifestError, Release, LICENSE_ID,
    NOTES_URL_BASE, SCHEMA,
};
use crate::version;

pub const EXPIRY_DAYS: i64 = 90;
pub const CHANGE_YEARS: i32 = 4;

#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum OpError {
    #[error("{0} is already published; versions are immutable")]
    AlreadyPublished(String),
    #[error("{0} is not in the manifest")]
    UnknownVersion(String),
    #[error("{0}: a release needs at least one engine artifact")]
    NoEngineArtifact(String),
    #[error("{0}: the release notes are empty")]
    EmptyNotes(String),
    #[error("a yank needs a reason; users see it")]
    EmptyReason,
    #[error(transparent)]
    Invalid(#[from] ManifestError),
}

#[derive(Debug, Clone)]
pub struct NewRelease {
    pub version: String,
    pub build: String,
    pub abi: u32,
    pub released_at: OffsetDateTime,
    pub min_hub_version: Option<String>,
    pub security: bool,
    pub notes: String,
    pub artifacts: Vec<Artifact>,
}

/// A manifest with no releases, at serial 1: the first publish only.
pub fn empty(base_url: &str, now: OffsetDateTime) -> Manifest {
    let mut m = Manifest {
        schema: SCHEMA,
        serial: 0,
        generated_at: String::new(),
        expires: String::new(),
        base_url: base_url.to_string(),
        channels: Channels::default(),
        hub: HubInfo::default(),
        releases: Vec::new(),
    };
    stamp(&mut m, now);
    m
}

pub fn add_release(current: &Manifest, new: NewRelease, now: OffsetDateTime) -> Result<Manifest, OpError> {
    let v = version::parse(&new.version).map_err(ManifestError::from)?;
    if current.release(&new.version).is_some() {
        return Err(OpError::AlreadyPublished(new.version));
    }
    if !new.artifacts.iter().any(|a| a.kind == "engine") {
        return Err(OpError::NoEngineArtifact(new.version));
    }
    if new.notes.trim().is_empty() {
        return Err(OpError::EmptyNotes(new.version));
    }
    let channel = version::channel_of(&v).map_err(ManifestError::from)?;
    let change = change_date(current, &v, new.released_at.to_offset(UtcOffset::UTC).date());
    let mut m = current.clone();
    m.releases.push(Release {
        notes_url: format!("{NOTES_URL_BASE}{}", new.version),
        version: new.version,
        channel,
        released_at: format_ts(new.released_at),
        build: new.build,
        abi: new.abi,
        license: LICENSE_ID.to_string(),
        change_date: format_date(change),
        min_hub_version: new.min_hub_version,
        yanked: false,
        yank_reason: None,
        security: new.security,
        notes: new.notes,
        artifacts: new.artifacts,
    });
    finish(m, now)
}

/// Marks a release yanked. Its archives stay, so pinned projects can still
/// install it (spec s4). A second yank only updates the reason.
pub fn yank(current: &Manifest, version: &str, reason: &str, now: OffsetDateTime) -> Result<Manifest, OpError> {
    let reason = reason.trim();
    if reason.is_empty() {
        return Err(OpError::EmptyReason);
    }
    let mut m = current.clone();
    let r = m.releases.iter_mut().find(|r| r.version == version).ok_or_else(|| OpError::UnknownVersion(version.to_string()))?;
    r.yanked = true;
    r.yank_reason = Some(reason.to_string());
    finish(m, now)
}

/// The scheduled re-sign: same contents, next serial, fresh expiry.
pub fn resign(current: &Manifest, now: OffsetDateTime) -> Result<Manifest, OpError> {
    finish(current.clone(), now)
}

/// Four years after the earliest release sharing `v`'s major.minor.patch
/// (its previews included), or after `released` if that is earlier. Spec
/// s12: a preview is a public distribution, so it starts the clock.
pub fn change_date(m: &Manifest, v: &Version, released: Date) -> Date {
    let base = version::base(v);
    let earliest = m
        .releases
        .iter()
        .filter_map(|r| Some((Version::parse(&r.version).ok()?, parse_ts(&r.released_at).ok()?)))
        .filter(|(rv, _)| version::base(rv) == base)
        .map(|(_, t)| t.date())
        .fold(released, Date::min);
    add_years(earliest, CHANGE_YEARS)
}

fn add_years(d: Date, years: i32) -> Date {
    let year = d.year() + years;
    Date::from_calendar_date(year, d.month(), d.day())
        .unwrap_or_else(|_| Date::from_calendar_date(year, Month::February, 28).expect("February 28 exists every year"))
}

fn finish(mut m: Manifest, now: OffsetDateTime) -> Result<Manifest, OpError> {
    m.releases.sort_by(|a, b| Version::parse(&b.version).ok().cmp(&Version::parse(&a.version).ok()));
    m.channels = Channels { stable: newest(&m, true), preview: newest(&m, false) };
    stamp(&mut m, now);
    m.validate()?;
    Ok(m)
}

fn newest(m: &Manifest, stable_only: bool) -> Option<String> {
    m.releases
        .iter()
        .filter(|r| !r.yanked && (!stable_only || r.channel == Channel::Stable))
        .filter_map(|r| Version::parse(&r.version).ok().map(|v| (v, r.version.clone())))
        .max_by(|a, b| a.0.cmp(&b.0))
        .map(|(_, s)| s)
}

fn stamp(m: &mut Manifest, now: OffsetDateTime) {
    m.serial += 1;
    m.generated_at = format_ts(now);
    m.expires = format_ts(now + Duration::days(EXPIRY_DAYS));
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd ArcaneHub/crates/arcane_release && cargo test`
Expected: PASS, every test in every file.

- [ ] **Step 5: Commit**

```bash
git add ArcaneHub/crates/arcane_release
git commit -m "feat(release): manifest operations -- add, yank, re-sign; channels; BSL change date from the first preview"
```

---

### Task 4: File digests, `manifest.txt` and deterministic archives

**Files:**
- Create: `ArcaneHub/crates/arcane_release/src/digest.rs`
- Create: `ArcaneHub/crates/arcane_release/src/pack.rs`
- Modify: `ArcaneHub/crates/arcane_release/src/lib.rs` (add `pub mod digest;` plus `#[cfg(feature = "tools")] pub mod pack;`)
- Test: `ArcaneHub/crates/arcane_release/tests/pack_test.rs`

**Interfaces:**
- Produces:
  - `digest::MANIFEST_TXT = "manifest.txt"`;
  - `digest::FileDigest { size: u64, sha256: String }`, which is `Serialize`;
  - `digest::digest_file(&Path)`;
  - `digest::list_files(&Path) -> io::Result<Vec<String>>`;
  - `digest::tree_listing(&Path) -> io::Result<String>`;
  - `digest::TreeProblem::{Missing, Changed, Unlisted}(String)`;
  - `digest::verify_tree(&Path) -> io::Result<Vec<TreeProblem>>`, which the Hub's Verify reuses in phase 3;
  - `pack::pack(stage: &Path, root_name: &str, out: &Path) -> anyhow::Result<FileDigest>`.

- [ ] **Step 1: Write the failing tests**

`tests/pack_test.rs`:

```rust
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test pack_test`
Expected: FAIL to compile with `unresolved import arcane_release::digest`.

- [ ] **Step 3: Write `src/digest.rs` and `src/pack.rs`; update `lib.rs`**

`src/digest.rs`:

```rust
//! File digests and `manifest.txt` (spec s7.1; the Hub's Verify, s6). The
//! listing is sha256sum's format, "<sha256>  <path>", one line per file,
//! '/'-separated, sorted, omitting manifest.txt itself, so
//! `sha256sum -c manifest.txt` checks an extracted engine by hand.

use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::Path;

use serde::Serialize;
use sha2::{Digest, Sha256};
use walkdir::WalkDir;

pub const MANIFEST_TXT: &str = "manifest.txt";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct FileDigest {
    pub size: u64,
    pub sha256: String,
}

pub fn digest_file(path: &Path) -> io::Result<FileDigest> {
    let mut file = File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut size = 0u64;
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        size += n as u64;
    }
    Ok(FileDigest { size, sha256: hex::encode(hasher.finalize()) })
}

/// Every regular file under `root` as a sorted '/'-separated relative path,
/// omitting a root-level manifest.txt. Symlinks are an error: a stage holds
/// real files (phase 1 copies a library under its soname).
pub fn list_files(root: &Path) -> io::Result<Vec<String>> {
    let mut out = Vec::new();
    for entry in WalkDir::new(root).follow_links(false) {
        let entry = entry.map_err(io::Error::other)?;
        let kind = entry.file_type();
        if kind.is_symlink() {
            return Err(io::Error::other(format!("{} is a symlink; a stage holds real files", entry.path().display())));
        }
        if !kind.is_file() {
            continue;
        }
        let rel = entry.path().strip_prefix(root).expect("walkdir stays under its root");
        let rel = rel.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect::<Vec<_>>().join("/");
        if rel != MANIFEST_TXT {
            out.push(rel);
        }
    }
    out.sort();
    Ok(out)
}

pub fn tree_listing(root: &Path) -> io::Result<String> {
    let mut listing = String::new();
    for rel in list_files(root)? {
        let d = digest_file(&root.join(&rel))?;
        listing.push_str(&d.sha256);
        listing.push_str("  ");
        listing.push_str(&rel);
        listing.push('\n');
    }
    Ok(listing)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TreeProblem {
    Missing(String),
    Changed(String),
    Unlisted(String),
}

/// Checks `root` against its manifest.txt.
pub fn verify_tree(root: &Path) -> io::Result<Vec<TreeProblem>> {
    let listing = fs::read_to_string(root.join(MANIFEST_TXT))?;
    let mut problems = Vec::new();
    let mut listed = HashSet::new();
    for line in listing.lines().filter(|l| !l.is_empty()) {
        let (sha, rel) = line.split_once("  ").ok_or_else(|| io::Error::other(format!("bad manifest.txt line: {line}")))?;
        if rel.split('/').any(|c| c.is_empty() || c == "." || c == "..") || rel.contains('\\') {
            return Err(io::Error::other(format!("manifest.txt names a path outside its folder: {rel}")));
        }
        listed.insert(rel.to_string());
        let path = root.join(rel);
        if !path.is_file() {
            problems.push(TreeProblem::Missing(rel.to_string()));
        } else if digest_file(&path)?.sha256 != sha {
            problems.push(TreeProblem::Changed(rel.to_string()));
        }
    }
    for rel in list_files(root)? {
        if !listed.contains(&rel) {
            problems.push(TreeProblem::Unlisted(rel));
        }
    }
    Ok(problems)
}
```

`src/pack.rs`:

```rust
//! Engine and symbols archives (spec s7.1): .zip on Windows, .tar.gz on
//! Linux, every file under one root folder, manifest.txt written into the
//! stage first. Deterministic: sorted entries, fixed timestamps and owners, so
//! the same stage always packs to the same bytes and SHA-256.

use std::fs::{self, File};
use std::io;
use std::path::Path;

use anyhow::{bail, Context, Result};

use crate::digest::{digest_file, list_files, tree_listing, FileDigest, MANIFEST_TXT};

pub fn pack(stage: &Path, root_name: &str, out: &Path) -> Result<FileDigest> {
    if root_name.is_empty() || root_name.contains(['/', '\\']) || root_name.starts_with('.') {
        bail!("the archive root must be one folder name, not {root_name:?}");
    }
    let name = out.file_name().and_then(|n| n.to_str()).unwrap_or_default();
    let is_zip = name.ends_with(".zip");
    if !is_zip && !name.ends_with(".tar.gz") {
        bail!("{} must end in .zip or .tar.gz", out.display());
    }
    fs::write(stage.join(MANIFEST_TXT), tree_listing(stage)?)
        .with_context(|| format!("writing {}", stage.join(MANIFEST_TXT).display()))?;
    let mut files = list_files(stage)?;
    files.push(MANIFEST_TXT.to_string());
    files.sort();
    if let Some(dir) = out.parent() {
        fs::create_dir_all(dir)?;
    }
    if is_zip {
        write_zip(stage, root_name, &files, out)?;
    } else {
        write_tar_gz(stage, root_name, &files, out)?;
    }
    Ok(digest_file(out)?)
}

fn write_zip(stage: &Path, root: &str, files: &[String], out: &Path) -> Result<()> {
    let mut zip = zip::ZipWriter::new(File::create(out)?);
    for rel in files {
        let path = stage.join(rel);
        let options = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .last_modified_time(zip::DateTime::default())
            .unix_permissions(mode_of(&fs::metadata(&path)?));
        zip.start_file(format!("{root}/{rel}"), options)?;
        io::copy(&mut File::open(&path)?, &mut zip)?;
    }
    zip.finish()?;
    Ok(())
}

fn write_tar_gz(stage: &Path, root: &str, files: &[String], out: &Path) -> Result<()> {
    let gz = flate2::GzBuilder::new().mtime(0).write(File::create(out)?, flate2::Compression::best());
    let mut tar = tar::Builder::new(gz);
    for rel in files {
        let path = stage.join(rel);
        let meta = fs::metadata(&path)?;
        let mut header = tar::Header::new_gnu();
        header.set_size(meta.len());
        header.set_mode(mode_of(&meta));
        header.set_mtime(0);
        header.set_uid(0);
        header.set_gid(0);
        tar.append_data(&mut header, format!("{root}/{rel}"), File::open(&path)?)?;
    }
    tar.into_inner()?.finish()?;
    Ok(())
}

#[cfg(unix)]
fn mode_of(meta: &fs::Metadata) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    if meta.permissions().mode() & 0o111 != 0 {
        0o755
    } else {
        0o644
    }
}

#[cfg(not(unix))]
fn mode_of(_meta: &fs::Metadata) -> u32 {
    0o644
}
```

`lib.rs` now reads:

```rust
pub mod digest;
pub mod keys;
pub mod manifest;
pub mod ops;
pub mod verify;
pub mod version;

#[cfg(feature = "tools")]
pub mod pack;
#[cfg(feature = "tools")]
pub mod sign;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd ArcaneHub/crates/arcane_release && cargo test && cargo check --no-default-features --lib`
Expected: PASS. On Windows the two `#[cfg(unix)]` tests are compiled out; CI's Ubuntu leg (Task 10) runs them.

- [ ] **Step 5: Commit**

```bash
git add ArcaneHub/crates/arcane_release
git commit -m "feat(release): manifest.txt listing and check, deterministic .zip/.tar.gz packer"
```

---

### Task 5: The `arcrelease` CLI

**Files:**
- Create: `ArcaneHub/crates/arcane_release/src/dist.rs`
- Modify: `ArcaneHub/crates/arcane_release/src/manifest.rs` (add `EngineInfo`)
- Modify: `ArcaneHub/crates/arcane_release/src/lib.rs` (add `#[cfg(feature = "tools")] pub mod dist;`)
- Modify: `ArcaneHub/crates/arcane_release/src/bin/arcrelease.rs` (replace the whole file)
- Test: `ArcaneHub/crates/arcane_release/tests/cli_test.rs`

**Interfaces:**
- Consumes: everything from Tasks 1–4.
- Produces (the scripts in Tasks 7, 8 and 11 call these exact commands and flags):
  - `arcrelease keygen --out-dir D --name N`
  - `arcrelease digest FILE` prints `{"size":N,"sha256":"..."}`
  - `arcrelease pack --stage DIR --root NAME --out FILE`
  - `arcrelease verify-tree --stage DIR`
  - `arcrelease init [--trust-key PUB] [--now T] [--base-url URL] --out-dir D`
  - `arcrelease add --manifest M --sig S [--trust-key PUB] [--now T] --dist DIST --version V --released-at T --notes-file F [--security] [--min-hub-version X] --out-dir D`
  - `arcrelease yank --manifest M --sig S [--trust-key PUB] [--now T] --version V --reason R --out-dir D`
  - `arcrelease resign --manifest M --sig S [--trust-key PUB] [--now T] --out-dir D`
  - `arcrelease verify --manifest M --sig S [--trust-key PUB] [--now T] [--highest-serial N] [--signature-only]`
  - `arcrelease change-date --manifest M --sig S [--trust-key PUB] --version V --released-at T`

  Secrets come from the environment: `ARCANE_MINISIGN_KEY` (the `.key` file's text) and `ARCANE_MINISIGN_PASSWORD`. Without `--trust-key`, the built-in `keys::TRUSTED_MANIFEST_KEYS` are trusted. Outputs are always `D/releases.json` and `D/releases.json.minisig`, and D must not already hold a `releases.json`.
- Also produces:
  - `manifest::EngineInfo { version, build, abi: u32, channel: Channel }`, which is the `engine.json` contract with phase 1;
  - `dist::read_dist(&Path, version) -> anyhow::Result<(EngineInfo, Vec<Artifact>)>`;
  - `dist::archive_name(version, os, symbols) -> String`.

- [ ] **Step 1: Write the failing tests**

`tests/cli_test.rs`:

```rust
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test cli_test`
Expected: FAIL. The binary has only `keygen`, so clap reports `unrecognized subcommand 'init'`.

- [ ] **Step 3: Add `EngineInfo` to `manifest.rs`**

Append to `src/manifest.rs`:

```rust
/// engine.json, inside every engine archive and beside it in the dist folder
/// (spec s7.1): it must equal what the probe reports. Phase 1 writes it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EngineInfo {
    pub version: String,
    pub build: String,
    pub abi: u32,
    pub channel: Channel,
}
```

- [ ] **Step 4: Write `src/dist.rs`** and add `#[cfg(feature = "tools")] pub mod dist;` to `lib.rs`

```rust
//! The dist folder the publish step reads: one folder per OS, each holding
//! the engine and symbols archives plus engine.json (spec s4, step 6).
//! Anything else in it is refused, not ignored.

use std::fs;
use std::path::Path;

use anyhow::{bail, ensure, Context, Result};

use crate::digest::digest_file;
use crate::manifest::{Artifact, EngineInfo};
use crate::version;

pub const ARCH: &str = "x86_64";
pub const OSES: [&str; 2] = ["windows", "linux"];

pub fn extension(os: &str) -> &'static str {
    if os == "windows" {
        "zip"
    } else {
        "tar.gz"
    }
}

pub fn archive_name(version: &str, os: &str, symbols: bool) -> String {
    format!("arcane-{version}-{os}-{ARCH}{}.{}", if symbols { "-symbols" } else { "" }, extension(os))
}

pub fn read_dist(dist: &Path, version: &str) -> Result<(EngineInfo, Vec<Artifact>)> {
    let channel = version::channel_of(&version::parse(version)?)?;
    let mut info: Option<EngineInfo> = None;
    let mut artifacts = Vec::new();
    for os in OSES {
        let dir = dist.join(os);
        ensure!(dir.is_dir(), "{} is missing: a release ships Windows and Linux", dir.display());
        let engine_json = dir.join("engine.json");
        let bytes = fs::read(&engine_json).with_context(|| format!("reading {}", engine_json.display()))?;
        let this: EngineInfo = serde_json::from_slice(&bytes).with_context(|| format!("parsing {}", engine_json.display()))?;
        ensure!(this.version == version, "{} says {}, not {version}", engine_json.display(), this.version);
        ensure!(this.channel == channel, "{} says {:?}, but {version} is {:?}", engine_json.display(), this.channel, channel);
        if info.is_none() {
            info = Some(this.clone());
        }
        ensure!(info.as_ref() == Some(&this), "engine.json differs between OSes: {:?} vs {this:?}", info.as_ref().unwrap());
        let expected = [archive_name(version, os, false), archive_name(version, os, true)];
        for entry in fs::read_dir(&dir)? {
            let name = entry?.file_name().to_string_lossy().into_owned();
            if name != "engine.json" && !expected.contains(&name) {
                bail!("unexpected file {name} in {}", dir.display());
            }
        }
        for (name, kind) in expected.iter().zip(["engine", "symbols"]) {
            let path = dir.join(name);
            ensure!(path.is_file(), "{} is missing", path.display());
            let d = digest_file(&path)?;
            artifacts.push(Artifact {
                os: os.to_string(),
                arch: ARCH.to_string(),
                kind: kind.to_string(),
                path: format!("engine/{version}/{name}"),
                size: d.size,
                sha256: d.sha256,
            });
        }
    }
    Ok((info.expect("OSES is not empty"), artifacts))
}
```

- [ ] **Step 5: Replace `src/bin/arcrelease.rs`**

```rust
//! `arcrelease`: the release tooling CI runs (spec s4, s5, s7.1). A command
//! that writes a manifest first verifies the one it replaces (its signature
//! and contents; expiry is not checked, so an expired manifest can be
//! re-signed), then verifies what it signed before writing anything, so a
//! wrong or untrusted key writes nothing. Secrets come only from the
//! environment: ARCANE_MINISIGN_KEY (the .key file's text) and
//! ARCANE_MINISIGN_PASSWORD.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{ensure, Context, Result};
use arcane_release::digest::{self, MANIFEST_TXT};
use arcane_release::manifest::{self, Manifest, BASE_URL};
use arcane_release::ops::{self, NewRelease};
use arcane_release::{dist, keys, pack, sign, verify};
use clap::{Args, Parser, Subcommand};
use minisign_verify::PublicKey;
use time::OffsetDateTime;

const KEY_ENV: &str = "ARCANE_MINISIGN_KEY";
const PASSWORD_ENV: &str = "ARCANE_MINISIGN_PASSWORD";

#[derive(Parser)]
#[command(name = "arcrelease", version, about = "Arcane release tooling: manifests, signing, archives")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Args)]
struct Trust {
    /// Trust this public key file instead of the keys built into this binary (staging and tests).
    #[arg(long)]
    trust_key: Option<PathBuf>,
}

#[derive(Args)]
struct Current {
    /// The releases.json being read or replaced.
    #[arg(long)]
    manifest: PathBuf,
    /// Its signature, releases.json.minisig.
    #[arg(long)]
    sig: PathBuf,
}

#[derive(Args)]
struct Clock {
    /// Use this RFC 3339 UTC time as "now" (tests).
    #[arg(long)]
    now: Option<String>,
}

#[derive(Subcommand)]
enum Cmd {
    /// Generate an encrypted minisign key pair; the password comes from ARCANE_MINISIGN_PASSWORD.
    Keygen {
        #[arg(long)]
        out_dir: PathBuf,
        #[arg(long)]
        name: String,
    },
    /// Print a file's size and SHA-256 as JSON.
    Digest { file: PathBuf },
    /// Write manifest.txt into a stage folder, then pack it (.zip or .tar.gz) under one root folder.
    Pack {
        #[arg(long)]
        stage: PathBuf,
        #[arg(long)]
        root: String,
        #[arg(long)]
        out: PathBuf,
    },
    /// Check a folder against its manifest.txt (what the Hub's Verify does).
    VerifyTree {
        #[arg(long)]
        stage: PathBuf,
    },
    /// Write a new manifest at serial 1 (the first publish only).
    Init {
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[arg(long, default_value = BASE_URL)]
        base_url: String,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Add a release from a dist folder (dist/<os>/engine.json and its archives).
    Add {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[arg(long)]
        dist: PathBuf,
        #[arg(long)]
        version: String,
        #[arg(long)]
        released_at: String,
        #[arg(long)]
        notes_file: PathBuf,
        #[arg(long)]
        security: bool,
        #[arg(long)]
        min_hub_version: Option<String>,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Mark a release yanked, with the reason users see.
    Yank {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[arg(long)]
        version: String,
        #[arg(long)]
        reason: String,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Re-sign with the next serial and a fresh 90-day expiry.
    Resign {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Verify a manifest the way the Hub does.
    Verify {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[arg(long)]
        highest_serial: Option<u64>,
        /// Check the signature and contents only, as CI does before replacing a manifest.
        #[arg(long)]
        signature_only: bool,
    },
    /// Print the BSL change date a version released at the given time would get.
    ChangeDate {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[arg(long)]
        version: String,
        #[arg(long)]
        released_at: String,
    },
}

fn main() -> Result<()> {
    match Cli::parse().cmd {
        Cmd::Keygen { out_dir, name } => keygen(&out_dir, &name),
        Cmd::Digest { file } => {
            println!("{}", serde_json::to_string(&digest::digest_file(&file)?)?);
            Ok(())
        }
        Cmd::Pack { stage, root, out } => {
            println!("{}", serde_json::to_string(&pack::pack(&stage, &root, &out)?)?);
            Ok(())
        }
        Cmd::VerifyTree { stage } => {
            let problems = digest::verify_tree(&stage).with_context(|| format!("checking against {}", stage.join(MANIFEST_TXT).display()))?;
            for problem in &problems {
                eprintln!("{problem:?}");
            }
            ensure!(problems.is_empty(), "{} file(s) differ from manifest.txt", problems.len());
            println!("ok");
            Ok(())
        }
        Cmd::Init { trust, clock, base_url, out_dir } => {
            let now = now(&clock)?;
            let m = ops::empty(&base_url, now);
            m.validate()?;
            write_signed(&m, &keys_for(&trust)?, &out_dir, now)
        }
        Cmd::Add { current, trust, clock, dist, version, released_at, notes_file, security, min_hub_version, out_dir } => {
            let now = now(&clock)?;
            let keys = keys_for(&trust)?;
            let cur = read_current(&current, &keys)?;
            let (info, artifacts) = dist::read_dist(&dist, &version)?;
            let notes = fs::read_to_string(&notes_file).with_context(|| format!("reading {}", notes_file.display()))?;
            let new = NewRelease {
                version,
                build: info.build,
                abi: info.abi,
                released_at: manifest::parse_ts(&released_at)?,
                min_hub_version,
                security,
                notes,
                artifacts,
            };
            write_signed(&ops::add_release(&cur, new, now)?, &keys, &out_dir, now)
        }
        Cmd::Yank { current, trust, clock, version, reason, out_dir } => {
            let now = now(&clock)?;
            let keys = keys_for(&trust)?;
            let cur = read_current(&current, &keys)?;
            write_signed(&ops::yank(&cur, &version, &reason, now)?, &keys, &out_dir, now)
        }
        Cmd::Resign { current, trust, clock, out_dir } => {
            let now = now(&clock)?;
            let keys = keys_for(&trust)?;
            let cur = read_current(&current, &keys)?;
            write_signed(&ops::resign(&cur, now)?, &keys, &out_dir, now)
        }
        Cmd::Verify { current, trust, clock, highest_serial, signature_only } => {
            let keys = keys_for(&trust)?;
            let bytes = fs::read(&current.manifest).with_context(|| format!("reading {}", current.manifest.display()))?;
            let sig = fs::read_to_string(&current.sig).with_context(|| format!("reading {}", current.sig.display()))?;
            let m = if signature_only {
                verify::verify_signed(&bytes, &sig, &keys)?
            } else {
                verify::verify_manifest(&bytes, &sig, &keys, highest_serial, now(&clock)?)?
            };
            println!("ok: serial {}, expires {}", m.serial, m.expires);
            Ok(())
        }
        Cmd::ChangeDate { current, trust, version, released_at } => {
            let cur = read_current(&current, &keys_for(&trust)?)?;
            let v = arcane_release::version::parse(&version)?;
            let released = manifest::parse_ts(&released_at)?.date();
            println!("{}", manifest::format_date(ops::change_date(&cur, &v, released)));
            Ok(())
        }
    }
}

fn now(clock: &Clock) -> Result<OffsetDateTime> {
    Ok(match &clock.now {
        Some(s) => manifest::parse_ts(s)?,
        None => OffsetDateTime::now_utc(),
    })
}

fn keys_for(trust: &Trust) -> Result<Vec<PublicKey>> {
    if let Some(path) = &trust.trust_key {
        let text = fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
        let key = verify::parse_public_key(&text).with_context(|| format!("{} is not a minisign public key", path.display()))?;
        return Ok(vec![key]);
    }
    let built_in = keys::trusted();
    ensure!(!built_in.is_empty(), "this arcrelease trusts no manifest keys yet (keys::TRUSTED_MANIFEST_KEYS is empty); pass --trust-key");
    Ok(built_in)
}

fn read_current(current: &Current, keys: &[PublicKey]) -> Result<Manifest> {
    let bytes = fs::read(&current.manifest).with_context(|| format!("reading {}", current.manifest.display()))?;
    let sig = fs::read_to_string(&current.sig).with_context(|| format!("reading {}", current.sig.display()))?;
    verify::verify_signed(&bytes, &sig, keys).with_context(|| {
        format!(
            "the current manifest {} does not verify, so nothing is built on it (docs/releasing.md, \"Repair\")",
            current.manifest.display()
        )
    })
}

fn write_signed(m: &Manifest, keys: &[PublicKey], out_dir: &Path, now: OffsetDateTime) -> Result<()> {
    let key_text = std::env::var(KEY_ENV).with_context(|| format!("{KEY_ENV} is not set"))?;
    let password = std::env::var(PASSWORD_ENV).with_context(|| format!("{PASSWORD_ENV} is not set"))?;
    let sk = sign::open_secret_key(&key_text, &password)?;
    let bytes = m.to_bytes();
    let sig = sign::sign(&sk, &bytes, &format!("arcane releases.json serial:{}", m.serial))?;
    verify::verify_manifest(&bytes, &sig, keys, Some(m.serial), now)
        .context("the manifest just signed does not verify against the trusted keys (is ARCANE_MINISIGN_KEY one of them?); nothing was written")?;
    let json = out_dir.join("releases.json");
    ensure!(!json.exists(), "{} already exists; give an empty --out-dir", json.display());
    fs::create_dir_all(out_dir)?;
    fs::write(&json, &bytes)?;
    fs::write(out_dir.join("releases.json.minisig"), sig)?;
    println!("signed serial {} into {}", m.serial, out_dir.display());
    Ok(())
}

fn keygen(out_dir: &Path, name: &str) -> Result<()> {
    let password = std::env::var(PASSWORD_ENV).with_context(|| format!("set {PASSWORD_ENV} to the key's password"))?;
    let (pub_path, key_path) = (out_dir.join(format!("{name}.pub")), out_dir.join(format!("{name}.key")));
    ensure!(!pub_path.exists() && !key_path.exists(), "{name}.pub or {name}.key already exists in {}", out_dir.display());
    let generated = sign::generate(name, &password)?;
    fs::create_dir_all(out_dir)?;
    fs::write(&pub_path, &generated.public_key_file)?;
    fs::write(&key_path, &generated.secret_key_file)?;
    println!("{}", generated.public_key_base64);
    Ok(())
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd ArcaneHub/crates/arcane_release && cargo test && cargo clippy --all-targets -- -D warnings`
Expected: everything passes and clippy is clean. Each CLI test opens a scrypt-encrypted key, so `cli_test` takes tens of seconds.

- [ ] **Step 7: Commit**

```bash
git add ArcaneHub/crates/arcane_release
git commit -m "feat(release): arcrelease CLI -- init/add/yank/resign/verify/change-date over a verified current manifest; dist folder contract"
```

---

### Task 6: Golden manifests for the Hub (phase 3)

**Files:**
- Create: `ArcaneHub/crates/arcane_release/tests/golden_test.rs`
- Create: `ArcaneHub/crates/arcane_release/tests/golden/README.md`
- Create (generated): `ArcaneHub/crates/arcane_release/tests/golden/{valid,older,expired,unknown-fields,rotated,tampered,schema2}.json` and `.json.minisig`

**Interfaces:**
- Consumes: `ops`, `sign`, `verify`, the test keys.
- Produces: fixed, signed manifests for each case spec s10 lists, with expected verdicts at `now = 2026-10-12T00:00:00Z` and highest serial seen = 5. Phase 3's Hub tests read these files.

- [ ] **Step 1: Write the test**

`tests/golden_test.rs`:

```rust
mod common;

use std::fs;
use std::path::PathBuf;

use arcane_release::manifest::{Artifact, ManifestError, BASE_URL};
use arcane_release::ops::{self, NewRelease};
use arcane_release::keys;
use arcane_release::sign;
use arcane_release::verify::{self, VerifyError};
use serde_json::json;
use time::macros::datetime;
use time::OffsetDateTime;

/// The Hub tests verify every golden at this instant with this highest serial.
const NOW: OffsetDateTime = datetime!(2026-10-12 00:00 UTC);
const HIGHEST_SEEN: u64 = 5;

fn dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests").join("golden")
}

fn artifacts(v: &str) -> Vec<Artifact> {
    ["windows", "linux"]
        .into_iter()
        .flat_map(|os| [common::artifact(v, os, "engine"), common::artifact(v, os, "symbols")])
        .collect()
}

fn new(v: &str, at: OffsetDateTime, security: bool, min_hub: Option<&str>) -> NewRelease {
    NewRelease {
        version: v.into(),
        build: common::BUILD.into(),
        abi: 59,
        released_at: at,
        min_hub_version: min_hub.map(String::from),
        security,
        notes: format!("Notes for {v}."),
        artifacts: artifacts(v),
    }
}

fn to_bytes(v: &serde_json::Value) -> Vec<u8> {
    let mut b = serde_json::to_vec_pretty(v).unwrap();
    b.push(b'\n');
    b
}

struct Golden {
    name: &'static str,
    bytes: Vec<u8>,
    /// The test key that signs it.
    key: u8,
    /// Bytes to sign instead of `bytes` (tampered: signed before the edit).
    signed: Option<Vec<u8>>,
}

fn golden(name: &'static str, bytes: Vec<u8>, key: u8, signed: Option<Vec<u8>>) -> Golden {
    Golden { name, bytes, key, signed }
}

fn goldens() -> Vec<Golden> {
    let m1 = ops::empty(BASE_URL, datetime!(2026-09-01 00:00 UTC));
    let m2 = ops::add_release(&m1, new("0.2.0", datetime!(2026-09-01 12:00 UTC), false, None), datetime!(2026-09-01 12:00 UTC)).unwrap();
    let m3 = ops::add_release(&m2, new("0.3.1", datetime!(2026-10-01 12:00 UTC), true, None), datetime!(2026-10-01 12:00 UTC)).unwrap();
    let m4 = ops::add_release(&m3, new("0.4.0-preview.2", datetime!(2026-10-05 12:00 UTC), false, Some("0.3.0")), datetime!(2026-10-05 12:00 UTC)).unwrap();
    let m5 = ops::yank(&m4, "0.2.0", "Projects saved by 0.2.0 can lose their scene on reload.", NOW).unwrap();
    let valid = m5.to_bytes();

    let expired = ops::resign(&m5, datetime!(2026-07-01 00:00 UTC)).unwrap().to_bytes();

    let mut extra: serde_json::Value = serde_json::from_slice(&valid).unwrap();
    extra["future_field"] = json!({ "anything": [1, 2] });
    extra["releases"][0]["future_release_field"] = json!("x");

    let mut tampered: serde_json::Value = serde_json::from_slice(&valid).unwrap();
    tampered["releases"][0]["artifacts"][0]["sha256"] = json!(common::sha('0'));

    vec![
        golden("valid", valid.clone(), 1, None),
        golden("older", m4.to_bytes(), 1, None),
        golden("expired", expired, 1, None),
        golden("unknown-fields", to_bytes(&extra), 1, None),
        golden("rotated", valid.clone(), 2, None),
        // tampered: the bytes changed after signing; its .minisig is valid's.
        golden("tampered", to_bytes(&tampered), 1, Some(valid)),
        golden("schema2", b"{\"schema\": 2, \"serial\": 99}\n".to_vec(), 1, None),
    ]
}

#[test]
fn goldens_are_current_and_verify_as_documented() {
    let bless = std::env::var_os("ARCANE_BLESS_GOLDENS").is_some();
    if bless {
        fs::create_dir_all(dir()).unwrap();
    }
    for Golden { name, bytes, key, signed } in goldens() {
        let sig = sign::sign(common::test_secret(key), signed.as_deref().unwrap_or(&bytes), "arcane golden").unwrap();
        let (jp, sp) = (dir().join(format!("{name}.json")), dir().join(format!("{name}.json.minisig")));
        // minisign 0.7 signs with added randomness, so a signature is not
        // reproducible: the JSON bytes are compared; the committed signatures
        // are proven by the verdicts below.
        if bless {
            fs::write(&jp, &bytes).unwrap();
            fs::write(&sp, &sig).unwrap();
        } else {
            assert_eq!(fs::read(&jp).unwrap(), bytes, "{name}.json is stale: run with ARCANE_BLESS_GOLDENS=1");
        }
    }

    let k1 = [common::test_public(1)];
    let both = [common::test_public(1), common::test_public(2)];
    let read = |n: &str| (fs::read(dir().join(format!("{n}.json"))).unwrap(), fs::read_to_string(dir().join(format!("{n}.json.minisig"))).unwrap());
    let check = |n: &str, keys: &[minisign_verify::PublicKey]| {
        let (b, s) = read(n);
        verify::verify_manifest(&b, &s, keys, Some(HIGHEST_SEEN), NOW)
    };

    let m = check("valid", &k1).unwrap();
    assert_eq!(m.serial, 5);
    assert_eq!(m.channels.stable.as_deref(), Some("0.3.1"));
    assert_eq!(m.channels.preview.as_deref(), Some("0.4.0-preview.2"));
    assert!(m.release("0.2.0").unwrap().yanked);
    assert!(m.release("0.3.1").unwrap().security);
    assert_eq!(m.release("0.4.0-preview.2").unwrap().min_hub_version.as_deref(), Some("0.3.0"));
    assert!(matches!(check("older", &k1), Err(VerifyError::Rollback { seen: 5, got: 4 })));
    assert!(matches!(check("expired", &k1), Err(VerifyError::Expired(_))));
    check("unknown-fields", &k1).unwrap();
    assert!(matches!(check("rotated", &k1), Err(VerifyError::BadSignature)));
    check("rotated", &both).unwrap();
    assert!(matches!(check("tampered", &k1), Err(VerifyError::BadSignature)));
    assert!(matches!(check("schema2", &k1), Err(VerifyError::Invalid(ManifestError::UnsupportedSchema(2)))));
}

/// The rehearsal (test) keys sign staging/. The production key list built into
/// every Hub must reject them: a slip that adds a test key to
/// TRUSTED_MANIFEST_KEYS fails here. (Before the key ceremony the list is empty
/// and this fails closed with NoKeys; after it, with BadSignature.)
#[test]
fn the_production_keys_reject_rehearsal_manifests() {
    for name in ["valid", "rotated"] {
        let bytes = fs::read(dir().join(format!("{name}.json"))).unwrap();
        let sig = fs::read_to_string(dir().join(format!("{name}.json.minisig"))).unwrap();
        let got = verify::verify_manifest(&bytes, &sig, &keys::trusted(), None, NOW);
        assert!(matches!(got, Err(VerifyError::BadSignature | VerifyError::NoKeys)), "{name}: {got:?}");
    }
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test golden_test`
Expected: FAIL with `No such file or directory`, because the goldens don't exist yet.

- [ ] **Step 3: Bless the goldens and write their README**

Run: `ARCANE_BLESS_GOLDENS=1 cargo test --test golden_test`, then `cargo test --test golden_test`.
Expected: both PASS. The `.json` files are reproducible. The `.minisig` files are not, because minisign adds randomness to each signature, so a re-bless rewrites them. Bless only on a deliberate change, and commit the result.

`tests/golden/README.md`:

```markdown
# Golden manifests

Signed fixtures for the Hub's manifest tests (distribution spec s10; phase 3).
Verify them at `now = 2026-10-12T00:00:00Z` with highest serial seen = 5,
trusting `tests/keys/test-manifest-1.pub` (and `-2` where noted).

| File | Expected |
|---|---|
| `valid` | Verifies. Serial 5. Stable 0.3.1 (security), preview 0.4.0-preview.2 (min Hub 0.3.0), 0.2.0 yanked with a reason. |
| `older` | Rollback: serial 4 is below 5. |
| `expired` | Expired (2026-09-29). |
| `unknown-fields` | Verifies. Unknown fields are ignored. |
| `rotated` | Signed with key 2. Bad signature against key 1 alone; verifies against [key 1, key 2]. |
| `tampered` | An artifact hash was changed after signing. Bad signature. |
| `schema2` | Signature fine, schema 2: "unsupported schema, update the Hub". |

Regenerate with `ARCANE_BLESS_GOLDENS=1 cargo test --test golden_test` after
a deliberate schema or operations change. The test checks that each `.json`
is current, then checks every verdict above against the committed files.
Signatures carry randomness, so a re-bless rewrites every `.minisig`. The
files are byte-exact (`.gitattributes`: `-text`).
```

- [ ] **Step 4: Commit**

```bash
git add ArcaneHub/crates/arcane_release/tests
git commit -m "test(release): signed golden manifests (valid, rollback, expired, unknown fields, rotation, tampered, schema 2) for the Hub"
```

---

### Task 7: R2 scripts — immutable archives, conditional manifest writes, history and repair

**Files:**
- Create: `scripts/release/lib.sh`
- Create: `scripts/release/publish.sh`
- Create: `scripts/release/manifest-admin.sh`
- Create: `scripts/release/install-aws-cli.sh`
- Create: `scripts/release/list-unreferenced.sh`
- Create: `scripts/release/fixtures/rehearsal-notes.md`
- Create: `scripts/release/tests/fake-aws/aws` (executable)
- Create: `scripts/release/tests/run-all.sh`
- Test: `scripts/release/tests/test-r2.sh`

**Interfaces:**
- Consumes: the `arcrelease` commands from Task 5 (an `arcrelease` on PATH, or `$ARCRELEASE`).
- Produces:
  - `lib.sh` functions: `die`, `need_env`, `r2_init`, `r2`, `put_immutable FILE KEY`, `upload_dist DIST PREFIX VERSION`, `get_manifest OUT PREFIX` (writes `OUT/releases.json`, `OUT/releases.json.minisig` and `OUT/etag`), `publish_manifest DIR PREFIX ETAG|none`, `repair_manifest PREFIX`, and `select_signing_key publish|production|rehearsal|staging|dry-run` (sets `TRUST`);
  - `publish.sh MODE PREFIX VERSION RELEASED_AT NOTES DIST WORK`;
  - `manifest-admin.sh OP TARGET [VERSION] [REASON]` with `OP` in `init|resign|yank|repair` and `TARGET` in `production|staging`;
  - `install-aws-cli.sh`;
  - `list-unreferenced.sh PREFIX VERSION`;
  - environment: `R2_ACCOUNT_ID`, `R2_BUCKET`, `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, plus `MINISIGN_SECRET_KEY` and `MINISIGN_PASSWORD` for production.

- [ ] **Step 1: Write the fake aws and the failing test**

`scripts/release/tests/fake-aws/aws`:

```bash
#!/usr/bin/env bash
# A stand-in for `aws s3api` over a folder ($FAKE_R2), for the release scripts'
# tests: put-object (with --if-none-match/--if-match), get-object, head-object
# and list-objects-v2, printing the JSON shapes aws-cli prints. Objects live in
# $FAKE_R2/objects/<key>; the sha256 metadata in $FAKE_R2/meta/<key>.
# FAKE_AWS_FAIL_PUT=<key> makes every put to that key fail.
set -euo pipefail
: "${FAKE_R2:?FAKE_R2 is not set}"
echo "$*" >> "$FAKE_R2/calls.log"
[ "${1:-}" = s3api ] || { echo "fake aws: only s3api is faked" >&2; exit 2; }
shift
# aws-cli takes global options before the operation too (lib.sh's r2 does).
while [ "${1:-}" = --endpoint-url ]; do shift 2; done
op=$1
shift
key="" body="" ifnm="" ifm="" sha="" prefix="" out=""
while [ $# -gt 0 ]; do
    case $1 in
        --endpoint-url|--bucket|--content-type|--cache-control) shift 2 ;;
        --key) key=$2; shift 2 ;;
        --body) body=$2; shift 2 ;;
        --if-none-match) ifnm=$2; shift 2 ;;
        --if-match) ifm=$2; shift 2 ;;
        --metadata) sha=${2#sha256=}; shift 2 ;;
        --prefix) prefix=$2; shift 2 ;;
        *) out=$1; shift ;;
    esac
done
obj="$FAKE_R2/objects/$key"
meta="$FAKE_R2/meta/$key"
etag_of() { printf '"%s"' "$(md5sum "$1" | cut -d' ' -f1)"; }
precondition() { echo "An error occurred (PreconditionFailed) when calling the PutObject operation: At least one of the pre-conditions you specified did not hold" >&2; exit 254; }
case $op in
    put-object)
        if [ "${FAKE_AWS_FAIL_PUT:-}" = "$key" ]; then echo "An error occurred (InternalError) when calling the PutObject operation" >&2; exit 254; fi
        if [ -n "$ifnm" ] && [ -e "$obj" ]; then precondition; fi
        if [ -n "$ifm" ]; then { [ -e "$obj" ] && [ "$(etag_of "$obj")" = "$ifm" ]; } || precondition; fi
        mkdir -p "$(dirname "$obj")" "$(dirname "$meta")"
        cp "$body" "$obj"
        printf '%s' "$sha" > "$meta"
        jq -n --arg e "$(etag_of "$obj")" '{ETag: $e}' ;;
    get-object)
        [ -e "$obj" ] || { echo "An error occurred (NoSuchKey) when calling the GetObject operation: The specified key does not exist." >&2; exit 254; }
        cp "$obj" "$out"
        jq -n --arg e "$(etag_of "$obj")" '{ETag: $e}' ;;
    head-object)
        [ -e "$obj" ] || { echo "An error occurred (404) when calling the HeadObject operation: Not Found" >&2; exit 254; }
        jq -n --arg e "$(etag_of "$obj")" --arg s "$(cat "$meta" 2>/dev/null || true)" \
            '{ETag: $e, Metadata: (if $s == "" then {} else {sha256: $s} end)}' ;;
    list-objects-v2)
        { [ -d "$FAKE_R2/objects" ] && (cd "$FAKE_R2/objects" && find . -type f | sed 's|^\./||' | grep "^$prefix" || true); } \
            | jq -R . | jq -s '{Contents: map({Key: .})}' ;;
    *) echo "fake aws: unsupported $op" >&2; exit 2 ;;
esac
```

`scripts/release/tests/test-r2.sh`:

```bash
#!/usr/bin/env bash
# Tests for scripts/release/{lib,publish,manifest-admin}.sh over a fake R2
# (tests/fake-aws) with the TEST manifest key. Needs bash, jq, md5sum,
# sha256sum and arcrelease on PATH:
#   cargo install --path ArcaneHub/crates/arcane_release --locked
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$REL/../.." && pwd)
TEST_PUB="$ROOT/ArcaneHub/crates/arcane_release/tests/keys/test-manifest-1.pub"
command -v arcrelease >/dev/null || { echo "install arcrelease: cargo install --path ArcaneHub/crates/arcane_release --locked" >&2; exit 2; }
command -v jq >/dev/null || { echo "jq is required" >&2; exit 2; }

pass=0 fail=0
ok()   { pass=$((pass + 1)); echo "ok   $1"; }
bad()  { fail=$((fail + 1)); echo "FAIL $1"; }
expect()  { local name=$1; shift; if "$@" >"$T/out.log" 2>&1; then ok "$name"; else bad "$name"; sed 's/^/     /' "$T/out.log"; fi; }
refused() { local name=$1; shift; if "$@" >"$T/out.log" 2>&1; then bad "$name (was accepted)"; else ok "$name"; fi; }
said() { grep -q -- "$1" "$T/out.log"; }

setup() {
    T=$(mktemp -d)
    export FAKE_R2="$T/r2" PATH="$HERE/fake-aws:$PATH" R2_RETRY_DELAY=0
    export R2_ACCOUNT_ID=test R2_BUCKET=arcane-downloads AWS_ACCESS_KEY_ID=fake AWS_SECRET_ACCESS_KEY=fake
    mkdir -p "$FAKE_R2/objects"
    unset FAKE_AWS_FAIL_PUT
}
obj() { echo "$FAKE_R2/objects/$1"; }
serial_of() { jq -r .serial "$(obj "$1")"; }
pointer_verifies() {
    arcrelease verify --manifest "$(obj staging/releases.json)" --sig "$(obj staging/releases.json.minisig)" --trust-key "$TEST_PUB" >/dev/null 2>&1
}
make_dist() { # DIR VERSION
    local pair os ext
    for pair in windows:zip linux:tar.gz; do
        os=${pair%%:*} ext=${pair#*:}
        mkdir -p "$1/$os"
        printf '{"version":"%s","build":"0123456789abcdef0123456789abcdef01234567","abi":0,"channel":"preview"}' "$2" > "$1/$os/engine.json"
        echo "engine $os" > "$1/$os/arcane-$2-$os-x86_64.$ext"
        echo "symbols $os" > "$1/$os/arcane-$2-$os-x86_64-symbols.$ext"
    done
}
admin() { bash "$REL/manifest-admin.sh" "$@"; }
publish() { # VERSION
    bash "$REL/publish.sh" rehearsal staging/ "$1" 2026-10-12T00:00:00Z "$REL/fixtures/rehearsal-notes.md" "$T/dist" "$T/work-$RANDOM"
}

# --- put_immutable: once, identical re-upload fine, different content refused
setup
echo a > "$T/a"; echo b > "$T/b"
expect  "put_immutable uploads a new object"       bash -c ". '$REL/lib.sh'; r2_init; put_immutable '$T/a' x/a"
expect  "an identical re-upload is accepted"       bash -c ". '$REL/lib.sh'; r2_init; put_immutable '$T/a' x/a"
said "already uploaded, identical" && ok "  and says so" || bad "  and says so"
refused "different content under the same key"    bash -c ". '$REL/lib.sh'; r2_init; put_immutable '$T/b' x/a"
said "different content" && ok "  and says why" || bad "  and says why"
refused "a failed put of a missing object fails"   bash -c "export FAKE_AWS_FAIL_PUT=x/c; . '$REL/lib.sh'; r2_init; put_immutable '$T/a' x/c"

# --- init
setup
expect  "init staging"                             admin init staging
[ "$(serial_of staging/releases.json)" = 1 ] && ok "  serial 1" || bad "  serial 1"
[ -f "$(obj staging/history/000000001.json)" ] && ok "  history copy" || bad "  history copy"
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"
refused "a second init is refused"                 admin init staging

# --- publish
setup; admin init staging >/dev/null; make_dist "$T/dist" 0.0.0-preview.1
expect  "publish a rehearsal"                      publish 0.0.0-preview.1
[ "$(find "$(obj staging/engine/0.0.0-preview.1)" -type f | wc -l)" -eq 4 ] && ok "  four archives" || bad "  four archives"
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  serial 2" || bad "  serial 2"
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"
refused "publishing the same version again"        publish 0.0.0-preview.1
said "already published" && ok "  says already published" || bad "  says already published"
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  manifest unchanged" || bad "  manifest unchanged"

# --- rerun_after_partial: archives uploaded, manifest failed; a rerun completes
setup; admin init staging >/dev/null; make_dist "$T/dist" 0.0.0-preview.2
refused "a publish failing at the history write"   env FAKE_AWS_FAIL_PUT=staging/history/000000002.json bash "$REL/publish.sh" rehearsal staging/ 0.0.0-preview.2 2026-10-12T00:00:00Z "$REL/fixtures/rehearsal-notes.md" "$T/dist" "$T/w1"
[ "$(serial_of staging/releases.json)" = 1 ] && ok "  manifest untouched" || bad "  manifest untouched"
expect  "the rerun accepts the uploaded archives"  publish 0.0.0-preview.2
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  and publishes serial 2" || bad "  and publishes serial 2"

# --- race: a manifest read before another run published is never written over
setup; admin init staging >/dev/null
bash -c ". '$REL/lib.sh'; r2_init; get_manifest '$T/stale' staging/" >/dev/null
expect  "a resign wins the race"                   admin resign staging
winner=$(md5sum "$(obj staging/releases.json)" | cut -d' ' -f1)
ARCANE_MINISIGN_KEY=$(cat "$ROOT/ArcaneHub/crates/arcane_release/tests/keys/test-manifest-1.key") ARCANE_MINISIGN_PASSWORD=arcane-test-key-not-a-secret \
    arcrelease resign --manifest "$T/stale/releases.json" --sig "$T/stale/releases.json.minisig" --trust-key "$TEST_PUB" --out-dir "$T/loser" >/dev/null
refused "the stale writer fails"                   bash -c ". '$REL/lib.sh'; r2_init; publish_manifest '$T/loser' staging/ \"\$(cat '$T/stale/etag')\""
[ "$(md5sum "$(obj staging/releases.json)" | cut -d' ' -f1)" = "$winner" ] && ok "  winner's manifest stands" || bad "  winner's manifest stands"
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"

# --- half_update_and_repair: the json updated, the signature not
setup; admin init staging >/dev/null
refused "a resign whose signature upload fails"    env FAKE_AWS_FAIL_PUT=staging/releases.json.minisig bash "$REL/manifest-admin.sh" resign staging
said "op=repair" && ok "  names the repair" || bad "  names the repair"
pointer_verifies && bad "  pointer is half-updated" || ok "  pointer is half-updated"
refused "nothing builds on the half-updated pointer" admin resign staging
expect  "repair restores the newest verified pair" admin repair staging
pointer_verifies && ok "  pointer verifies" || bad "  pointer verifies"
[ "$(serial_of staging/releases.json)" = 2 ] && ok "  at serial 2" || bad "  at serial 2"

# --- tampered: refused, and the message says so
setup; admin init staging >/dev/null
sed -i 's/"serial": 1/"serial": 7/' "$(obj staging/releases.json)"
refused "a tampered pointer is refused"            admin resign staging
said "does not verify" && ok "  says it does not verify" || bad "  says it does not verify"

# --- yank
setup; admin init staging >/dev/null; make_dist "$T/dist" 0.0.0-preview.3; publish 0.0.0-preview.3 >/dev/null
expect  "yank a staging release"                   admin yank staging 0.0.0-preview.3 "Broken fixture."
[ "$(jq -r '.releases[0].yanked' "$(obj staging/releases.json)")" = true ] && ok "  yanked" || bad "  yanked"
refused "yank without a reason"                    admin yank staging 0.0.0-preview.3

# --- an empty dist uploads nothing
setup; admin init staging >/dev/null; mkdir -p "$T/dist"
refused "a publish with no archives"               publish 0.0.0-preview.4

echo "test-r2: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
```

`scripts/release/tests/run-all.sh`:

```bash
#!/usr/bin/env bash
# Runs every scripts/release test (Linux, or WSL: needs jq and arcrelease).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
status=0
for t in "$here"/test-*.sh; do
    echo "== $(basename "$t")"
    bash "$t" || status=1
done
exit $status
```

`scripts/release/fixtures/rehearsal-notes.md`:

```markdown
Rehearsal build: a fixture, not the engine. Published to staging/ with the
test manifest key, which no Hub trusts.
```

- [ ] **Step 2: Run the test to verify it fails**

Run (Linux or WSL, with arcrelease installed): `bash scripts/release/tests/test-r2.sh`
Expected: FAIL. Every case fails because `lib.sh` and the scripts don't exist, and the summary reports 0 passed.

- [ ] **Step 3: Write `lib.sh`**

```bash
# shellcheck shell=bash
# Shared by the release scripts (spec s4, s12): R2 through aws-cli's S3 API,
# never Wrangler. Archives are written once (If-None-Match: *); the manifest
# pointer only over the exact version that was read (If-Match: <etag>); every
# manifest serial is also kept immutably under history/. Secrets come from the
# environment and are never echoed.
set -euo pipefail

RELEASE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TEST_KEY_DIR="$RELEASE_ROOT/ArcaneHub/crates/arcane_release/tests/keys"
ARCRELEASE=${ARCRELEASE:-arcrelease}
TRUST=()

die() { echo "release: $*" >&2; exit 1; }
need_env() { local v; for v in "$@"; do [ -n "${!v:-}" ] || die "$v is not set"; done; }

r2_init() {
    need_env R2_ACCOUNT_ID R2_BUCKET AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY
    export AWS_DEFAULT_REGION=auto
    # aws-cli 2.23+ computes request checksums R2 does not fully support (spec s12).
    export AWS_REQUEST_CHECKSUM_CALCULATION=WHEN_REQUIRED
    export AWS_RESPONSE_CHECKSUM_VALIDATION=WHEN_REQUIRED
    R2_ENDPOINT="https://${R2_ACCOUNT_ID}.r2.cloudflarestorage.com"
}

r2() { aws s3api --endpoint-url "$R2_ENDPOINT" "$@"; }

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

content_type_for() {
    case $1 in
        *.zip) echo application/zip ;;
        *.tar.gz) echo application/gzip ;;
        *.json) echo application/json ;;
        *.minisig|*.txt|*.md) echo text/plain ;;
        *) echo application/octet-stream ;;
    esac
}

# put_immutable FILE KEY: upload once. If KEY exists already, the same SHA-256
# counts as uploaded (a rerun); different content fails (versions are immutable).
put_immutable() {
    local file=$1 key=$2 sha existing
    sha=$(sha256_of "$file")
    if r2 put-object --bucket "$R2_BUCKET" --key "$key" --body "$file" --if-none-match '*' \
        --content-type "$(content_type_for "$key")" --cache-control 'public, max-age=31536000, immutable' \
        --metadata "sha256=$sha" >/dev/null 2>&1; then
        echo "uploaded $key"
        return 0
    fi
    existing=$(r2 head-object --bucket "$R2_BUCKET" --key "$key" 2>/dev/null | jq -r '.Metadata.sha256 // empty') || existing=""
    [ -n "$existing" ] || die "uploading $key failed and no such object exists"
    [ "$existing" = "$sha" ] || die "$key already exists with different content (sha256 $existing); versions are immutable"
    echo "already uploaded, identical: $key"
}

# upload_dist DIST PREFIX VERSION: every archive under DIST/<os>/ to PREFIXengine/VERSION/.
upload_dist() {
    local dist=$1 prefix=$2 version=$3 f n=0
    for f in "$dist"/*/arcane-"$version"-*; do
        [ -f "$f" ] || continue
        put_immutable "$f" "${prefix}engine/${version}/$(basename "$f")"
        n=$((n + 1))
    done
    [ "$n" -gt 0 ] || die "no archives for $version under $dist"
}

# get_manifest OUT PREFIX: the current pointer pair, read through the S3 API
# (never the CDN, which could be stale), and its ETag for the If-Match write.
get_manifest() {
    local out=$1 prefix=$2
    mkdir -p "$out"
    r2 get-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" "$out/releases.json" >"$out/get.json" 2>"$out/get.err" \
        || die "cannot read ${prefix}releases.json ($(tr -d '\n' <"$out/get.err")); a first publish needs manifest-admin.yml op=init"
    jq -r .ETag "$out/get.json" >"$out/etag"
    r2 get-object --bucket "$R2_BUCKET" --key "${prefix}releases.json.minisig" "$out/releases.json.minisig" >/dev/null 2>&1 \
        || die "cannot read ${prefix}releases.json.minisig; run manifest-admin.yml op=repair"
}

# put_pointer DIR PREFIX ETAG|none: releases.json only over the version read
# (or only if absent), then its signature (retried). The pair cannot be
# written atomically; history/ is what repair restores from.
put_pointer() {
    local dir=$1 prefix=$2 etag=$3 i cond
    if [ "$etag" = none ]; then cond=(--if-none-match '*'); else cond=(--if-match "$etag"); fi
    r2 put-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" --body "$dir/releases.json" "${cond[@]}" \
        --content-type application/json --cache-control no-cache >/dev/null 2>&1 \
        || die "${prefix}releases.json changed since it was read (another run published first?); nothing was overwritten. Re-run this workflow."
    for i in 1 2 3; do
        if r2 put-object --bucket "$R2_BUCKET" --key "${prefix}releases.json.minisig" --body "$dir/releases.json.minisig" \
            --content-type text/plain --cache-control no-cache >/dev/null 2>&1; then
            echo "published ${prefix}releases.json serial $(jq -r .serial "$dir/releases.json")"
            return 0
        fi
        [ "$i" -lt 3 ] && sleep "${R2_RETRY_DELAY:-5}"
    done
    die "${prefix}releases.json was updated but its signature was not; run manifest-admin.yml with op=repair (docs/releasing.md, \"Repair\")"
}

# publish_manifest DIR PREFIX ETAG|none: the history copy FIRST, write-once (an
# existing copy is refused unless byte-identical, which only a re-run of the
# same signing produces), so repair always has a signed copy; then the pointer.
publish_manifest() {
    local dir=$1 prefix=$2 etag=$3 hist
    hist=$(printf '%shistory/%09d' "$prefix" "$(jq -r .serial "$dir/releases.json")")
    put_immutable "$dir/releases.json" "$hist.json"
    put_immutable "$dir/releases.json.minisig" "$hist.json.minisig"
    put_pointer "$dir" "$prefix" "$etag"
}

# repair_manifest PREFIX: restore the pointer pair from the newest history pair
# whose signature verifies.
repair_manifest() {
    local prefix=$1 work keys k found="" etag
    work=$(mktemp -d)
    keys=$(r2 list-objects-v2 --bucket "$R2_BUCKET" --prefix "${prefix}history/" | jq -r '.Contents[]?.Key' | grep '\.json$' | sort -r) || keys=""
    [ -n "$keys" ] || die "no history under ${prefix}history/"
    for k in $keys; do
        r2 get-object --bucket "$R2_BUCKET" --key "$k" "$work/releases.json" >/dev/null 2>&1 || continue
        r2 get-object --bucket "$R2_BUCKET" --key "$k.minisig" "$work/releases.json.minisig" >/dev/null 2>&1 || continue
        if "$ARCRELEASE" verify --signature-only --manifest "$work/releases.json" --sig "$work/releases.json.minisig" "${TRUST[@]}" >/dev/null 2>&1; then
            found=$k
            break
        fi
    done
    [ -n "$found" ] || die "no history pair under ${prefix}history/ verifies"
    etag=$(r2 head-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" 2>/dev/null | jq -r .ETag) || etag=none
    put_pointer "$work" "$prefix" "$etag"
    echo "repaired ${prefix}releases.json from $found"
}

# select_signing_key MODE: production uses the release key from the
# environment and the keys built into arcrelease; staging, rehearsal and dry
# runs use the committed TEST key, which no Hub trusts.
select_signing_key() {
    case $1 in
        publish|production)
            need_env MINISIGN_SECRET_KEY MINISIGN_PASSWORD
            ARCANE_MINISIGN_KEY=$MINISIGN_SECRET_KEY
            ARCANE_MINISIGN_PASSWORD=$MINISIGN_PASSWORD
            TRUST=() ;;
        rehearsal|staging|dry-run)
            ARCANE_MINISIGN_KEY=$(cat "$TEST_KEY_DIR/test-manifest-1.key")
            ARCANE_MINISIGN_PASSWORD=arcane-test-key-not-a-secret
            TRUST=(--trust-key "$TEST_KEY_DIR/test-manifest-1.pub") ;;
        *) die "unknown signing mode $1" ;;
    esac
    export ARCANE_MINISIGN_KEY ARCANE_MINISIGN_PASSWORD
}
```

- [ ] **Step 4: Write `publish.sh`, `manifest-admin.sh`, `install-aws-cli.sh` and `list-unreferenced.sh`**

`scripts/release/publish.sh`:

```bash
#!/usr/bin/env bash
# publish.sh MODE PREFIX VERSION RELEASED_AT NOTES DIST WORK -- the publish job
# (spec s4, engine job steps 6-7): upload the archives (never overwriting),
# read the current manifest, add the release, sign, publish. MODE publish goes
# to the bucket root with the release key; rehearsal goes to staging/ with the
# test key.
. "$(dirname "$0")/lib.sh"
[ $# -eq 7 ] || die "usage: publish.sh MODE PREFIX VERSION RELEASED_AT NOTES DIST WORK"
mode=$1 prefix=$2 version=$3 released_at=$4 notes=$5 dist=$6 work=$7
case $mode in
    publish) [ -z "$prefix" ] || die "a publish goes to the bucket root" ;;
    rehearsal) [ "$prefix" = staging/ ] || die "a rehearsal goes to staging/" ;;
    *) die "unknown mode $mode" ;;
esac
r2_init
select_signing_key "$mode"
upload_dist "$dist" "$prefix" "$version"
get_manifest "$work/current" "$prefix"
"$ARCRELEASE" add --manifest "$work/current/releases.json" --sig "$work/current/releases.json.minisig" "${TRUST[@]}" \
    --dist "$dist" --version "$version" --released-at "$released_at" --notes-file "$notes" --out-dir "$work/next"
publish_manifest "$work/next" "$prefix" "$(cat "$work/current/etag")"
```

`scripts/release/manifest-admin.sh`:

```bash
#!/usr/bin/env bash
# manifest-admin.sh OP TARGET [VERSION] [REASON] -- maintains releases.json
# (spec s4 "Re-signing", "Channels and yanking"; docs/releasing.md):
#   init    write serial 1 (the first publish only; refuses if one exists)
#   resign  the next serial with a fresh 90-day expiry (monthly)
#   yank    mark VERSION yanked with REASON
#   repair  restore releases.json + .minisig from the newest verified history copy
# TARGET production is the bucket root with the release key; staging is
# staging/ with the test key.
. "$(dirname "$0")/lib.sh"
op=${1:?usage: manifest-admin.sh OP TARGET [VERSION] [REASON]}
target=${2:?usage: manifest-admin.sh OP TARGET [VERSION] [REASON]}
version=${3:-} reason=${4:-}
case $target in
    production) prefix="" ;;
    staging) prefix="staging/" ;;
    *) die "unknown target $target" ;;
esac
r2_init
select_signing_key "$target"
work=$(mktemp -d)
case $op in
    init)
        if r2 head-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" >/dev/null 2>&1; then
            die "${prefix}releases.json already exists; init is for the first publish only"
        fi
        "$ARCRELEASE" init "${TRUST[@]}" --base-url "https://downloads.starworks.dev/${prefix}" --out-dir "$work/next"
        publish_manifest "$work/next" "$prefix" none ;;
    resign)
        get_manifest "$work/current" "$prefix"
        "$ARCRELEASE" resign --manifest "$work/current/releases.json" --sig "$work/current/releases.json.minisig" "${TRUST[@]}" --out-dir "$work/next"
        publish_manifest "$work/next" "$prefix" "$(cat "$work/current/etag")" ;;
    yank)
        [ -n "$version" ] && [ -n "$reason" ] || die "yank needs a version and a reason"
        get_manifest "$work/current" "$prefix"
        "$ARCRELEASE" yank --manifest "$work/current/releases.json" --sig "$work/current/releases.json.minisig" "${TRUST[@]}" \
            --version "$version" --reason "$reason" --out-dir "$work/next"
        publish_manifest "$work/next" "$prefix" "$(cat "$work/current/etag")" ;;
    repair)
        repair_manifest "$prefix" ;;
    *) die "unknown op $op" ;;
esac
```

`scripts/release/list-unreferenced.sh`:

```bash
#!/usr/bin/env bash
# list-unreferenced.sh PREFIX VERSION -- after a failed publish: the archives
# uploaded for VERSION that the current manifest doesn't reference (spec s9:
# they stay, unreferenced, and are listed). Prints Markdown for the job summary.
. "$(dirname "$0")/lib.sh"
prefix=$1 version=$2
r2_init
work=$(mktemp -d)
listed=""
if r2 get-object --bucket "$R2_BUCKET" --key "${prefix}releases.json" "$work/r.json" >/dev/null 2>&1; then
    listed=$(jq -r --arg v "$version" '.releases[] | select(.version == $v) | .artifacts[].path' "$work/r.json")
fi
echo "### Archives uploaded for $version that releases.json does not reference"
r2 list-objects-v2 --bucket "$R2_BUCKET" --prefix "${prefix}engine/${version}/" | jq -r '.Contents[]?.Key' | while read -r key; do
    grep -qxF "${key#"$prefix"}" <<<"$listed" || echo "- \`$key\`"
done
```

`scripts/release/install-aws-cli.sh` (the hash is recorded in Step 5):

```bash
#!/usr/bin/env bash
# install-aws-cli.sh -- the pinned aws-cli v2 on a Linux runner (spec s12: the
# version is pinned, and lib.sh sets the checksum settings R2 needs).
set -euo pipefail
VERSION=2.37.13
SHA256=RECORD_IN_TASK_7_STEP_5
dir="${RUNNER_TEMP:?}/awscli"
curl -fsSLo "$dir.zip" "https://awscli.amazonaws.com/awscli-exe-linux-x86_64-${VERSION}.zip"
echo "$SHA256  $dir.zip" | sha256sum -c -
unzip -q -o "$dir.zip" -d "$dir"
"$dir/aws/install" --install-dir "$dir/install" --bin-dir "$dir/bin" --update
echo "$dir/bin" >>"${GITHUB_PATH:?}"
"$dir/bin/aws" --version | grep -q "aws-cli/$VERSION" || { echo "aws-cli is not $VERSION" >&2; exit 1; }
```

- [ ] **Step 5: Pin the aws-cli download's hash**

Run: `curl -fsSL https://awscli.amazonaws.com/awscli-exe-linux-x86_64-2.37.13.zip | sha256sum`
Replace `RECORD_IN_TASK_7_STEP_5` in `install-aws-cli.sh` with the 64-hex hash it prints. Then run `grep -c RECORD_IN scripts/release/install-aws-cli.sh`. Expected: `0`.

- [ ] **Step 6: Make the scripts executable and run the tests**

```bash
git add --chmod=+x scripts/release/*.sh scripts/release/tests/*.sh scripts/release/tests/fake-aws/aws
cargo install --path ArcaneHub/crates/arcane_release --locked
bash scripts/release/tests/test-r2.sh
```

Expected: `test-r2: N passed, 0 failed`. Run it on Linux or WSL; Git Bash lacks `jq` by default.

- [ ] **Step 7: Commit**

```bash
git add scripts/release
git commit -m "feat(release): R2 scripts -- immutable archives, If-Match manifest writes, history copies and repair; tests over a fake R2"
```

---

### Task 8: Deciding a run — tag check, notes, and the Linux image

**Files:**
- Create: `scripts/release/check-version.sh`
- Create: `scripts/release/resolve-ci-image.sh`
- Create: `scripts/release/prepare.sh`
- Create: `docs/releases/README.md`
- Test: `scripts/release/tests/test-prepare.sh`

**Interfaces:**
- Consumes: the phase 1 contract for `Version.hpp` (`kVersionPatch`, `kVersionPrerelease`), and plan A's `scripts/ci/image-tag.sh`, which prints `build-<16 hex>`.
- Produces:
  - `check-version.sh TAG HEADER` prints the version, or fails;
  - `resolve-ci-image.sh` prints `ghcr.io/starworksdev/arcane-ci@sha256:<digest>`, or fails if the image isn't published;
  - `prepare.sh` appends `version`, `channel`, `mode` (`publish|dry-run|rehearsal`), `prefix`, `released_at`, `notes` and `ci_image` to `$GITHUB_OUTPUT`. It reads `GITHUB_EVENT_NAME`, `GITHUB_REF`, `GITHUB_RUN_NUMBER` and `INPUT_MODE`; tests override with `VERSION_HPP`, `NOTES_DIR`, `IMAGE_TAG_SCRIPT` and `RELEASED_AT_OVERRIDE`.

- [ ] **Step 1: Write the failing test**

`scripts/release/tests/test-prepare.sh`:

```bash
#!/usr/bin/env bash
# Tests for check-version.sh, resolve-ci-image.sh and prepare.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
T=$(mktemp -d)
pass=0 fail=0
accepts() { local name=$1; shift; if "$@" >/dev/null 2>&1; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; fi; }
refuses() { local name=$1; shift; if "$@" >/dev/null 2>&1; then fail=$((fail + 1)); echo "FAIL $name (accepted)"; else pass=$((pass + 1)); echo "ok   $name"; fi; }
same() { if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "ok   $1"; else fail=$((fail + 1)); echo "FAIL $1: got '$2', want '$3'"; fi; }

header() { # MAJOR MINOR PATCH PRERELEASE
    cat >"$T/Version.hpp" <<EOF
    inline constexpr int kVersionMajor = $1;
    inline constexpr int kVersionMinor = $2;
    inline constexpr int kVersionPatch = $3;
    inline constexpr const char* kVersionPrerelease = "$4";
EOF
}
cv() { bash "$REL/check-version.sh" "$1" "$T/Version.hpp"; }

# --- check-version
header 0 3 1 ""
accepts "a stable tag matching the header"    cv v0.3.1
same    "it prints the version"               "$(cv v0.3.1)" 0.3.1
refuses "another patch"                       cv v0.3.2
refuses "a preview tag against a stable header" cv v0.3.1-preview.1
refuses "not MAJOR.MINOR.PATCH"               cv v0.3
refuses "a leading zero"                      cv v0.03.1
refuses "a beta prerelease"                   cv v0.3.1-beta.1
header 0 4 0 "preview.2"
accepts "a preview tag matching the header"   cv v0.4.0-preview.2
refuses "the stable tag of a preview header"  cv v0.4.0
printf '    inline constexpr int kVersionMajor = 0;\n    inline constexpr int kVersionMinor = 1;\n' >"$T/Version.hpp"
refuses "today's header (no patch, no prerelease)" cv v0.1.0

# --- resolve-ci-image
mkdir -p "$T/bin"
printf '#!/bin/sh\necho build-0123456789abcdef\n' >"$T/image-tag.sh"
cat >"$T/bin/docker" <<'EOF'
#!/bin/sh
# fake: docker buildx imagetools inspect REF --format ...; published only if REF = $FAKE_PUBLISHED
if [ "$4" = "${FAKE_PUBLISHED:-}" ]; then echo '{"digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}'; else echo "ERROR: not found" >&2; exit 1; fi
EOF
chmod +x "$T/bin/docker"
export PATH="$T/bin:$PATH" IMAGE_TAG_SCRIPT="$T/image-tag.sh"
export FAKE_PUBLISHED=ghcr.io/starworksdev/arcane-ci:build-0123456789abcdef
same    "a published image resolves to its digest" "$(bash "$REL/resolve-ci-image.sh")" \
        "ghcr.io/starworksdev/arcane-ci@sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
export FAKE_PUBLISHED=nothing
refuses "an unpublished image fails closed"   bash "$REL/resolve-ci-image.sh"
refuses "a missing image-tag script fails"    env IMAGE_TAG_SCRIPT="$T/nope.sh" bash "$REL/resolve-ci-image.sh"

# --- prepare
export FAKE_PUBLISHED=ghcr.io/starworksdev/arcane-ci:build-0123456789abcdef
mkdir -p "$T/notes"
prep() { : >"$T/out"; GITHUB_OUTPUT="$T/out" VERSION_HPP="$T/Version.hpp" NOTES_DIR="$T/notes" RELEASED_AT_OVERRIDE=2026-10-12T00:00:00Z bash "$REL/prepare.sh"; }
out() { grep "^$1=" "$T/out" | tail -1 | cut -d= -f2-; }

header 0 3 1 ""
refuses "a tag without its notes file"        env GITHUB_EVENT_NAME=push GITHUB_REF=refs/tags/v0.3.1 bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
echo "Notes." >"$T/notes/0.3.1.md"
accepts "a tag with its notes"                env GITHUB_EVENT_NAME=push GITHUB_REF=refs/tags/v0.3.1 bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
same "  version" "$(out version)" 0.3.1
same "  mode" "$(out mode)" publish
same "  channel" "$(out channel)" stable
same "  prefix" "$(out prefix)" ""
same "  released_at" "$(out released_at)" 2026-10-12T00:00:00Z
same "  ci_image" "$(out ci_image)" "ghcr.io/starworksdev/arcane-ci@sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
accepts "a dry run from any branch"           env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/topic GITHUB_RUN_NUMBER=7 INPUT_MODE=dry-run bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
same "  version" "$(out version)" 0.0.0-preview.7
same "  prefix" "$(out prefix)" staging/
same "  channel" "$(out channel)" preview
refuses "a rehearsal from a branch"           env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/topic GITHUB_RUN_NUMBER=7 INPUT_MODE=rehearsal bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
accepts "a rehearsal from main"               env GITHUB_EVENT_NAME=workflow_dispatch GITHUB_REF=refs/heads/main GITHUB_RUN_NUMBER=8 INPUT_MODE=rehearsal bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
same "  mode" "$(out mode)" rehearsal
refuses "a pull_request event"                env GITHUB_EVENT_NAME=pull_request GITHUB_REF=refs/pull/1/merge bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"
refuses "a branch push"                       env GITHUB_EVENT_NAME=push GITHUB_REF=refs/heads/main bash -c "$(declare -f prep); T='$T'; REL='$REL'; prep"

echo "test-prepare: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash scripts/release/tests/test-prepare.sh`
Expected: FAIL. Most cases fail with `No such file` for the three scripts.

- [ ] **Step 3: Write the scripts and the notes README**

`scripts/release/check-version.sh`:

```bash
#!/usr/bin/env bash
# check-version.sh TAG HEADER -- prints the version if the tag (vMAJOR.MINOR.PATCH
# or vMAJOR.MINOR.PATCH-preview.N) matches Arcane/Version.hpp (spec s4). Phase 1
# adds kVersionPatch and kVersionPrerelease; until then every tag fails here.
set -euo pipefail
die() { echo "check-version: $*" >&2; exit 1; }
tag=${1:?usage: check-version.sh TAG HEADER}
header=${2:?usage: check-version.sh TAG HEADER}
n='(0|[1-9][0-9]*)'
[[ $tag =~ ^v$n\.$n\.$n(-preview\.$n)?$ ]] || die "tag $tag is not vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-preview.N"
version=${tag#v}
num() { sed -n "s/.*inline constexpr int $1 = \([0-9][0-9]*\);.*/\1/p" "$header" | head -1; }
major=$(num kVersionMajor) minor=$(num kVersionMinor) patch=$(num kVersionPatch)
[ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ] || die "$header lacks kVersionMajor/kVersionMinor/kVersionPatch (phase 1 adds kVersionPatch)"
grep -q 'kVersionPrerelease = "' "$header" || die "$header lacks kVersionPrerelease (phase 1 adds it; \"\" for a stable release)"
pre=$(sed -n 's/.*kVersionPrerelease = "\([^"]*\)";.*/\1/p' "$header" | head -1)
want="$major.$minor.$patch${pre:+-$pre}"
[ "$want" = "$version" ] || die "tag $tag does not match $header ($want)"
echo "$version"
```

`scripts/release/resolve-ci-image.sh`:

```bash
#!/usr/bin/env bash
# resolve-ci-image.sh -- prints plan A's Linux CI image pinned by digest. The
# release workflow never builds the image: it must already be published by
# ci.yml's ci-image job on main (spec s4, Runners). Fails closed otherwise.
set -euo pipefail
die() { echo "resolve-ci-image: $*" >&2; exit 1; }
IMAGE=ghcr.io/starworksdev/arcane-ci
script=${IMAGE_TAG_SCRIPT:-scripts/ci/image-tag.sh}
[ -f "$script" ] || die "$script is missing (plan A, the local CI pipeline, must be on this branch)"
tag=$(sh "$script") || die "$script failed"
digest=$(docker buildx imagetools inspect "$IMAGE:$tag" --format '{{json .Manifest}}' 2>/dev/null | jq -r '.digest // empty') || digest=""
[ -n "$digest" ] || die "$IMAGE:$tag is not published; ci.yml's ci-image job on main publishes it. The release workflow never builds it."
echo "$IMAGE@$digest"
```

`scripts/release/prepare.sh`:

```bash
#!/usr/bin/env bash
# prepare.sh -- decides what a release run is (spec s4) and writes it to
# $GITHUB_OUTPUT: version, channel, mode, prefix, released_at, notes, ci_image.
#   push of tag v*        -> mode publish: the tag must match Version.hpp,
#                            docs/releases/<version>.md must exist, and the
#                            Linux CI image must already be published.
#   dispatch dry-run      -> a fixture build, nothing uploaded (any branch).
#   dispatch rehearsal    -> a fixture build published to staging/ (main only).
# released_at is now: the run's start, shared by the archives and the manifest.
set -euo pipefail
die() { echo "prepare: $*" >&2; exit 1; }
event=${GITHUB_EVENT_NAME:?} ref=${GITHUB_REF:?} out=${GITHUB_OUTPUT:?}
here=$(cd "$(dirname "$0")" && pwd)
released_at=${RELEASED_AT_OVERRIDE:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}
ci_image=""
case $event in
    push)
        [[ $ref == refs/tags/v* ]] || die "a release runs only on v* tags, not $ref"
        header=${VERSION_HPP:-$(git ls-files '*/Arcane/Version.hpp' | head -1)}
        [ -n "$header" ] || die "no Arcane/Version.hpp in the checkout"
        version=$(bash "$here/check-version.sh" "${ref#refs/tags/}" "$header")
        notes="${NOTES_DIR:-docs/releases}/$version.md"
        [ -s "$notes" ] || die "missing or empty $notes: the release notes ship with the tag"
        mode=publish prefix=""
        ci_image=$(bash "$here/resolve-ci-image.sh") ;;
    workflow_dispatch)
        mode=${INPUT_MODE:?}
        case $mode in
            dry-run) ;;
            rehearsal) [ "$ref" = refs/heads/main ] || die "a rehearsal runs only from main" ;;
            *) die "unknown mode $mode" ;;
        esac
        version="0.0.0-preview.${GITHUB_RUN_NUMBER:?}" prefix="staging/" notes="$here/fixtures/rehearsal-notes.md" ;;
    *) die "unexpected event $event" ;;
esac
channel=stable
[[ $version == *-preview.* ]] && channel=preview
{
    echo "version=$version"
    echo "channel=$channel"
    echo "mode=$mode"
    echo "prefix=$prefix"
    echo "released_at=$released_at"
    echo "notes=$notes"
    echo "ci_image=$ci_image"
} >>"$out"
```

`docs/releases/README.md`:

```markdown
# Release notes

One file per release, named for the version without the `v`:
`0.3.1.md` or `0.4.0-preview.2.md`. It must exist at the tag; the release
workflow fails without it. The same text becomes the GitHub Release body and
the `notes` field in `releases.json`, which the Hub and
starworks.dev/arcane/releases show. Write for users: what changed, what to
watch for when upgrading, and whether the ABI changed.
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
git add --chmod=+x scripts/release/check-version.sh scripts/release/resolve-ci-image.sh scripts/release/prepare.sh scripts/release/tests/test-prepare.sh
bash scripts/release/tests/test-prepare.sh
```

Expected: `test-prepare: N passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add scripts/release docs/releases
git commit -m "feat(release): prepare -- tag vs Version.hpp, required notes, the CI image resolved never built, dry-run/rehearsal modes"
```

---

### Task 9: Windows signing action and SHA pinning tools

**Files:**
- Create: `.github/actions/sign-windows/action.yml`
- Create: `.github/actions/sign-windows/check-signatures.ps1`
- Create: `.github/actions/sign-windows/tests/test-check-signatures.ps1`
- Create: `.github/actions/arcrelease/action.yml`
- Create: `scripts/release/check-pinned.sh`
- Create: `scripts/release/pin-actions.sh`
- Test: `scripts/release/tests/test-pin.sh`

**Interfaces:**
- Produces:
  - the composite action `./.github/actions/sign-windows`, with inputs `folder`, `client-id`, `tenant-id`, `subscription-id`, `endpoint`, `account` and `profile`. It signs every `.exe` and `.dll` under `folder` recursively, then fails unless each is `Valid` and timestamped;
  - `./.github/actions/arcrelease`, which builds `arcrelease` from the checkout with `--locked` and puts it on PATH;
  - `check-pinned.sh FILE...`, which exits 1 naming each unpinned `uses:`;
  - `pin-actions.sh FILE...`, which rewrites `uses: o/r@tag` to `uses: o/r@<sha> # tag` using `gh`.

- [ ] **Step 1: Write the failing tests**

`scripts/release/tests/test-pin.sh`:

```bash
#!/usr/bin/env bash
# Tests for check-pinned.sh and pin-actions.sh (roadmap D26: third-party
# actions pinned to commit SHAs).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
T=$(mktemp -d)
pass=0 fail=0
accepts() { local name=$1; shift; if "$@" >"$T/log" 2>&1; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; cat "$T/log"; fi; }
refuses() { local name=$1; shift; if "$@" >"$T/log" 2>&1; then fail=$((fail + 1)); echo "FAIL $name (accepted)"; else pass=$((pass + 1)); echo "ok   $name"; fi; }
SHA=0123456789abcdef0123456789abcdef01234567

cat >"$T/wf.yml" <<EOF
jobs:
  a:
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/arcrelease
      - uses: azure/login@$SHA # v3
      - name: x
        uses: dtolnay/rust-toolchain@stable
EOF
refuses "unpinned uses are reported"     bash "$REL/check-pinned.sh" "$T/wf.yml"
grep -q "actions/checkout@v4" "$T/log" && grep -q "dtolnay/rust-toolchain@stable" "$T/log" && pass=$((pass + 1)) && echo "ok     and named" || { fail=$((fail + 1)); echo "FAIL   and named"; }

mkdir -p "$T/bin"
printf '#!/bin/sh\necho %s\n' "$SHA" >"$T/bin/gh"
chmod +x "$T/bin/gh"
accepts "pin-actions pins them"          env PATH="$T/bin:$PATH" bash "$REL/pin-actions.sh" "$T/wf.yml"
accepts "and the file then passes"       bash "$REL/check-pinned.sh" "$T/wf.yml"
grep -q "uses: actions/checkout@$SHA # v4" "$T/wf.yml" && pass=$((pass + 1)) && echo "ok     keeps the tag as a comment" || { fail=$((fail + 1)); echo "FAIL   keeps the tag as a comment"; }
grep -q "uses: ./.github/actions/arcrelease" "$T/wf.yml" && pass=$((pass + 1)) && echo "ok     leaves local actions alone" || { fail=$((fail + 1)); echo "FAIL   leaves local actions alone"; }

echo "test-pin: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
```

`.github/actions/sign-windows/tests/test-check-signatures.ps1`:

```powershell
# Tests for check-signatures.ps1 (Windows): a signed and timestamped binary
# passes; an unsigned one, one in a subfolder, and an empty folder fail.
$ErrorActionPreference = 'Stop'
$check = Join-Path $PSScriptRoot '..\check-signatures.ps1'
$shell = (Get-Process -Id $PID).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ("sigtest-" + [guid]::NewGuid())
$failures = 0

function Invoke-Check($folder) {
    & $shell -NoProfile -ExecutionPolicy Bypass -File $check -Folder $folder *> $null
    return $LASTEXITCODE
}
function Expect($name, $folder, $code) {
    $got = Invoke-Check $folder
    if (($code -eq 0) -eq ($got -eq 0)) { Write-Host "ok   $name" } else { Write-Host "FAIL $name (exit $got)"; $script:failures++ }
}

# A signed, timestamped exe from the machine: PowerShell 7 or Git, both embed signatures.
$signed = @("$env:ProgramFiles\PowerShell\7\pwsh.exe", "$env:ProgramFiles\Git\cmd\git.exe") |
    Where-Object { (Test-Path $_) -and ((Get-AuthenticodeSignature $_).Status -eq 'Valid') -and ($null -ne (Get-AuthenticodeSignature $_).TimeStamperCertificate) } |
    Select-Object -First 1

New-Item -ItemType Directory -Force "$root\good\sub", "$root\unsigned", "$root\nested\sub", "$root\empty" | Out-Null
if ($signed) {
    Copy-Item $signed "$root\good\sub\signed.exe"
    Expect 'a signed, timestamped exe passes' "$root\good" 0
    Copy-Item $signed "$root\nested\top.exe"
} else {
    Write-Host 'skip a signed, timestamped exe passes (no embedded-signed exe found on this machine)'
}
[IO.File]::WriteAllBytes("$root\unsigned\fake.dll", [byte[]](0x4D, 0x5A, 0, 0))
[IO.File]::WriteAllBytes("$root\nested\sub\fake.dll", [byte[]](0x4D, 0x5A, 0, 0))
Expect 'an unsigned dll fails' "$root\unsigned" 1
Expect 'an unsigned dll in a subfolder fails' "$root\nested" 1
Expect 'an empty folder fails' "$root\empty" 1

Remove-Item -Recurse -Force $root
if ($failures) { exit 1 }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `bash scripts/release/tests/test-pin.sh` and `powershell -NoProfile -File .github/actions/sign-windows/tests/test-check-signatures.ps1`
Expected: both FAIL because the scripts don't exist.

- [ ] **Step 3: Write the scripts and actions**

`scripts/release/check-pinned.sh`:

```bash
#!/usr/bin/env bash
# check-pinned.sh FILE... -- every `uses:` that is not a local ./ action must
# be pinned to a 40-hex commit SHA (roadmap D26, spec s4). Exit 1 names each.
set -euo pipefail
bad=0
for f in "$@"; do
    while IFS= read -r ref; do
        case $ref in ./*) continue ;; esac
        [[ $ref =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}$ ]] || { echo "$f: not pinned to a commit SHA: $ref" >&2; bad=1; }
    done < <(sed -n 's/^[[:space:]]*-\{0,1\}[[:space:]]*uses:[[:space:]]*\([^[:space:]#]*\).*/\1/p' "$f")
done
exit $bad
```

`scripts/release/pin-actions.sh`:

```bash
#!/usr/bin/env bash
# pin-actions.sh FILE... -- rewrites each `uses: owner/repo[/path]@ref` that is
# not already a commit SHA to `uses: owner/repo[/path]@<sha> # <ref>`. Needs an
# authenticated gh. Run it after adding or bumping an action; check-pinned.sh
# enforces the result in CI.
set -euo pipefail
for f in "$@"; do
    for ref in $(sed -n 's/^[[:space:]]*-\{0,1\}[[:space:]]*uses:[[:space:]]*\([^[:space:]#]*\).*/\1/p' "$f" | sort -u); do
        case $ref in ./*) continue ;; esac
        action=${ref%@*} tag=${ref#*@}
        [[ $tag =~ ^[0-9a-f]{40}$ ]] && continue
        repo=$(cut -d/ -f1-2 <<<"$action")
        sha=$(gh api "repos/$repo/commits/$tag" --jq .sha)
        [[ $sha =~ ^[0-9a-f]{40}$ ]] || { echo "pin-actions: could not resolve $ref" >&2; exit 1; }
        sed -i "s|uses:[[:space:]]*$ref[[:space:]]*\(#.*\)\{0,1\}\$|uses: $action@$sha # $tag|" "$f"
        echo "$f: $ref -> $sha"
    done
done
```

`.github/actions/sign-windows/check-signatures.ps1`:

```powershell
# check-signatures.ps1 -Folder DIR: every .exe and .dll under DIR must be
# Authenticode-signed (Status Valid) and timestamped, or the release stops
# (spec s4 step 4; s12, Smart App Control). An empty folder is an error.
param([Parameter(Mandatory)][string]$Folder)
$files = @(Get-ChildItem -LiteralPath $Folder -Recurse -File -Include *.exe, *.dll)
if ($files.Count -eq 0) { Write-Host "::error::no .exe or .dll under $Folder; nothing was signed"; exit 1 }
$bad = @()
foreach ($f in $files) {
    $s = Get-AuthenticodeSignature -LiteralPath $f.FullName
    if ($s.Status -ne 'Valid') { $bad += "$($f.FullName): $($s.Status) $($s.StatusMessage)"; continue }
    if ($null -eq $s.TimeStamperCertificate) { $bad += "$($f.FullName): signed but not timestamped" }
}
if ($bad.Count -gt 0) { $bad | ForEach-Object { Write-Host "::error::$_" }; exit 1 }
Write-Host "$($files.Count) file(s) signed and timestamped"
```

`.github/actions/sign-windows/action.yml`:

```yaml
name: Sign Windows binaries
description: >-
  Authenticode-signs every .exe and .dll under a folder with Azure Artifact
  Signing, authenticating by GitHub OIDC (no stored secret), then checks that
  each one is signed and timestamped. Spec s3 (Credentials), s4 step 4.
inputs:
  folder: { description: Folder to sign recursively, required: true }
  client-id: { description: Entra app (client) ID with the federated credential, required: true }
  tenant-id: { description: Entra tenant ID, required: true }
  subscription-id: { description: Azure subscription ID, required: true }
  endpoint: { description: Artifact Signing endpoint for the account's region, required: true }
  account: { description: Artifact Signing account name, required: true }
  profile: { description: Certificate profile name, required: true }
runs:
  using: composite
  steps:
    - uses: azure/login@v3
      with:
        client-id: ${{ inputs.client-id }}
        tenant-id: ${{ inputs.tenant-id }}
        subscription-id: ${{ inputs.subscription-id }}
    - uses: azure/artifact-signing-action@v2
      with:
        endpoint: ${{ inputs.endpoint }}
        signing-account-name: ${{ inputs.account }}
        certificate-profile-name: ${{ inputs.profile }}
        files-folder: ${{ inputs.folder }}
        files-folder-filter: exe,dll
        files-folder-recurse: true
        file-digest: SHA256
        timestamp-rfc3161: http://timestamp.acs.microsoft.com
        timestamp-digest: SHA256
        # Only the azure/login session (Azure CLI credential) may authenticate.
        exclude-environment-credential: true
        exclude-workload-identity-credential: true
        exclude-managed-identity-credential: true
        exclude-shared-token-cache-credential: true
        exclude-visual-studio-credential: true
        exclude-visual-studio-code-credential: true
        exclude-azure-powershell-credential: true
        exclude-azure-developer-cli-credential: true
        exclude-interactive-browser-credential: true
    - name: Check every signature
      shell: pwsh
      run: '& "${{ github.action_path }}/check-signatures.ps1" -Folder "${{ inputs.folder }}"'
```

`.github/actions/arcrelease/action.yml`:

```yaml
name: Install arcrelease
description: Builds arcrelease (ArcaneHub/crates/arcane_release) from this checkout, Cargo.lock pinned, and puts it on PATH.
runs:
  using: composite
  steps:
    - uses: dtolnay/rust-toolchain@686976e191b89faba57d3206551f0f330d8cb249 # stable
    - name: cargo install arcrelease
      shell: bash
      run: |
        cargo install --path ArcaneHub/crates/arcane_release --locked --root "$RUNNER_TEMP/arcrelease"
        echo "$RUNNER_TEMP/arcrelease/bin" >> "$GITHUB_PATH"
```

- [ ] **Step 4: Pin the actions and run the tests**

```bash
git add --chmod=+x scripts/release/check-pinned.sh scripts/release/pin-actions.sh scripts/release/tests/test-pin.sh
bash scripts/release/pin-actions.sh .github/actions/sign-windows/action.yml .github/actions/arcrelease/action.yml
bash scripts/release/check-pinned.sh .github/actions/sign-windows/action.yml .github/actions/arcrelease/action.yml
bash scripts/release/tests/test-pin.sh
powershell -NoProfile -ExecutionPolicy Bypass -File .github/actions/sign-windows/tests/test-check-signatures.ps1
```

Expected:
- `pin-actions` prints two `-> <sha>` lines (`azure/login`, `azure/artifact-signing-action`; the toolchain is already pinned). First confirm each is at its current major: `gh release view -R Azure/login` and `gh release view -R Azure/artifact-signing-action`;
- `check-pinned` exits 0;
- `test-pin` reports `0 failed`;
- the PowerShell test prints only `ok`/`skip` lines and exits 0.

- [ ] **Step 5: Commit**

```bash
git add .github/actions scripts/release
git commit -m "feat(release): sign-windows composite (Artifact Signing over OIDC + signature check), arcrelease action, SHA pin/check tools"
```

---

### Task 10: CI for the release tooling

**Files:**
- Create: `.github/workflows/release-tooling.yml`

**Interfaces:**
- Consumes: the crate tests (Tasks 1–6), `scripts/release/tests/run-all.sh` (Tasks 7–9), `test-check-signatures.ps1` and `check-pinned.sh` (Task 9).
- Produces: a required check for every change to the release tooling.

- [ ] **Step 1: Write the workflow**

```yaml
# Tests the release tooling (distribution spec s10, "Release workflow"): the
# arcane_release crate on Windows and Linux, the release scripts over a fake
# R2, the signature checker, and the workflows' SHA pins and lint. It holds no
# secrets and never touches the release environment.
name: release-tooling

on:
  push:
    branches: [main]
    paths:
      - 'ArcaneHub/crates/arcane_release/**'
      - 'scripts/release/**'
      - '.github/actions/**'
      - '.github/workflows/release.yml'
      - '.github/workflows/manifest-admin.yml'
      - '.github/workflows/release-tooling.yml'
  pull_request:
    paths:
      - 'ArcaneHub/crates/arcane_release/**'
      - 'scripts/release/**'
      - '.github/actions/**'
      - '.github/workflows/release.yml'
      - '.github/workflows/manifest-admin.yml'
      - '.github/workflows/release-tooling.yml'

permissions:
  contents: read

concurrency:
  group: release-tooling-${{ github.ref }}
  cancel-in-progress: true

jobs:
  crate:
    name: arcane_release (${{ matrix.runner }})
    strategy:
      fail-fast: false
      matrix:
        runner: [windows-2025, ubuntu-24.04]
    runs-on: ${{ matrix.runner }}
    defaults:
      run:
        working-directory: ArcaneHub/crates/arcane_release
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: dtolnay/rust-toolchain@686976e191b89faba57d3206551f0f330d8cb249 # stable
        with:
          components: clippy
      - run: cargo test --locked
      - name: The Hub's build (no tools)
        run: cargo check --locked --no-default-features --lib
      - run: cargo clippy --locked --all-targets -- -D warnings

  scripts:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: ./.github/actions/arcrelease
      - run: bash scripts/release/tests/run-all.sh

  signature-check:
    runs-on: windows-2025
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - shell: pwsh
        run: ./.github/actions/sign-windows/tests/test-check-signatures.ps1

  workflows:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - name: Third-party actions pinned to commit SHAs
        run: >-
          bash scripts/release/check-pinned.sh
          .github/workflows/release.yml .github/workflows/manifest-admin.yml
          .github/workflows/release-tooling.yml .github/actions/*/action.yml
      - name: actionlint
        run: >-
          docker run --rm -v "$PWD:/repo" -w /repo rhysd/actionlint:1.7.7 -color
          .github/workflows/release.yml .github/workflows/manifest-admin.yml .github/workflows/release-tooling.yml
```

- [ ] **Step 2: Pin it and lint it locally**

```bash
bash scripts/release/pin-actions.sh .github/workflows/release-tooling.yml
bash scripts/release/check-pinned.sh .github/workflows/release-tooling.yml
docker run --rm -v "$PWD:/repo" -w /repo rhysd/actionlint:1.7.7 -color .github/workflows/release-tooling.yml
```

Expected: `check-pinned` exits 0, and actionlint prints nothing and exits 0. The `workflows` job lists `release.yml` and `manifest-admin.yml`, which Tasks 11 and 12 add; it goes green once they land.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/release-tooling.yml
git commit -m "ci(release): release-tooling workflow -- crate on Windows+Linux, scripts over a fake R2, signature checker, pins and actionlint"
```

---

### Task 11: The release workflow (dry run, rehearsal, publish) and the runbook

**Files:**
- Create: `.github/workflows/release.yml`
- Create: `scripts/release/stage-fixture.sh`
- Create: `scripts/release/check-stage.sh`
- Create: `scripts/release/pack.sh`
- Create: `scripts/release/dry-run-manifest.sh`
- Create: `docs/releasing.md`
- Modify: `CLAUDE.md` (one line in the repo map, beside `ArcaneHub/`)
- Test: `scripts/release/tests/test-dry-run.sh`

**Interfaces:**
- Consumes: `prepare.sh` (Task 8), `publish.sh` and `list-unreferenced.sh` (Task 7), `install-aws-cli.sh` (Task 7), the `sign-windows` and `arcrelease` actions (Task 9).
- Produces:
  - `release.yml`, triggered by `v[0-9]*` tags and by `workflow_dispatch` with `mode` in `dry-run|rehearsal`. Its jobs are `prepare` → `stage` (matrix) → `sign-windows` → `package` (matrix) → `publish` | `dry-run-manifest`;
  - `stage-fixture.sh VERSION OS STAGE`;
  - `check-stage.sh STAGE VERSION OS`;
  - `pack.sh STAGE OUTDIR VERSION OS`;
  - `dry-run-manifest.sh DIST VERSION RELEASED_AT NOTES`.

  The stage layout `STAGE/engine/...` (with `engine.json`) and `STAGE/symbols/...` is the slot phase 1's `stage-engine` fills.

- [ ] **Step 1: Write the failing test**

`scripts/release/tests/test-dry-run.sh`:

```bash
#!/usr/bin/env bash
# The dry-run path end to end, locally: a fixture stage for both OSes, checked,
# packed, and turned into a verified manifest with the test key. Nothing is
# uploaded. Needs arcrelease on PATH.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REL=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$REL/../.." && pwd)
T=$(mktemp -d)
V=0.0.0-preview.99
cd "$ROOT"
for os in windows linux; do
    bash "$REL/stage-fixture.sh" "$V" "$os" "$T/stage-$os"
    bash "$REL/check-stage.sh" "$T/stage-$os" "$V" "$os" >/dev/null
    bash "$REL/pack.sh" "$T/stage-$os" "$T/dist/$os" "$V" "$os" >/dev/null
done
[ -f "$T/dist/windows/arcane-$V-windows-x86_64.zip" ] || { echo "FAIL no windows zip"; exit 1; }
[ -f "$T/dist/linux/arcane-$V-linux-x86_64-symbols.tar.gz" ] || { echo "FAIL no linux symbols"; exit 1; }
GITHUB_STEP_SUMMARY="$T/summary.md" bash "$REL/dry-run-manifest.sh" "$T/dist" "$V" 2026-10-12T00:00:00Z "$REL/fixtures/rehearsal-notes.md"
grep -q "\"version\": \"$V\"" "$T/summary.md" || { echo "FAIL the summary lacks the release"; exit 1; }
if bash "$REL/check-stage.sh" "$T/stage-linux" 9.9.9 linux >/dev/null 2>&1; then echo "FAIL check-stage accepted a wrong version"; exit 1; fi
echo "test-dry-run: ok"
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash scripts/release/tests/test-dry-run.sh`
Expected: FAIL with `No such file` for `stage-fixture.sh`.

- [ ] **Step 3: Write the four scripts**

`scripts/release/stage-fixture.sh`:

```bash
#!/usr/bin/env bash
# stage-fixture.sh VERSION OS STAGE -- a stand-in engine stage for dry runs and
# rehearsals until phase 1's stage-engine exists. The "engine" is the
# arcrelease binary (a real exe, so Windows signing is exercised for real),
# plus engine.json, the LICENSE, and a symbols folder.
set -euo pipefail
version=$1 os=$2 stage=$3
exe=arcrelease
[ "$os" = windows ] && exe=arcrelease.exe
bin=${ARCRELEASE_BIN:-$(command -v arcrelease)}
[ -f "$bin" ] || bin="$bin.exe"
[ -f "$bin" ] || { echo "stage-fixture: arcrelease is not installed" >&2; exit 1; }
mkdir -p "$stage/engine/bin" "$stage/symbols"
cp "$bin" "$stage/engine/bin/$exe"
cp LICENSE "$stage/engine/LICENSE"
printf '{"version":"%s","build":"%s","abi":0,"channel":"preview"}\n' "$version" \
    "${GITHUB_SHA:-0000000000000000000000000000000000000000}" >"$stage/engine/engine.json"
echo "Fixture symbols for $version ($os): a rehearsal has no debug files." >"$stage/symbols/README.txt"
```

`scripts/release/check-stage.sh`:

```bash
#!/usr/bin/env bash
# check-stage.sh STAGE VERSION OS -- before packing: engine.json names this
# version, and the staged binary still runs after signing (spec s4 step 4).
# Phase 1 replaces the fixture run with --print-engine-info compared against
# engine.json (version, commit, engineAbi).
set -euo pipefail
stage=$1 version=$2 os=$3
die() { echo "check-stage: $*" >&2; exit 1; }
jq -e --arg v "$version" '.version == $v' "$stage/engine/engine.json" >/dev/null || die "$stage/engine/engine.json is not $version"
exe="$stage/engine/bin/arcrelease"
[ "$os" = windows ] && exe="$exe.exe"
[ -f "$exe" ] || die "no staged binary at $exe (phase 1: the engine's probe goes here)"
"$exe" --version
```

`scripts/release/pack.sh`:

```bash
#!/usr/bin/env bash
# pack.sh STAGE OUTDIR VERSION OS -- the engine and symbols archives and a copy
# of engine.json, named as the publish step expects (dist::archive_name).
set -euo pipefail
stage=$1 out=$2 version=$3 os=$4
ext=tar.gz
[ "$os" = windows ] && ext=zip
mkdir -p "$out"
arcrelease pack --stage "$stage/engine" --root "arcane-$version" --out "$out/arcane-$version-$os-x86_64.$ext"
arcrelease pack --stage "$stage/symbols" --root "arcane-$version-symbols" --out "$out/arcane-$version-$os-x86_64-symbols.$ext"
cp "$stage/engine/engine.json" "$out/engine.json"
```

`scripts/release/dry-run-manifest.sh`:

```bash
#!/usr/bin/env bash
# dry-run-manifest.sh DIST VERSION RELEASED_AT NOTES -- the manifest a release
# would publish, built against an empty test manifest with the TEST key,
# verified, and written to the job summary. No network, no secrets.
. "$(dirname "$0")/lib.sh"
dist=$1 version=$2 released_at=$3 notes=$4
select_signing_key dry-run
work=$(mktemp -d)
"$ARCRELEASE" init "${TRUST[@]}" --out-dir "$work/m1" >/dev/null
"$ARCRELEASE" add --manifest "$work/m1/releases.json" --sig "$work/m1/releases.json.minisig" "${TRUST[@]}" \
    --dist "$dist" --version "$version" --released-at "$released_at" --notes-file "$notes" --out-dir "$work/m2" >/dev/null
"$ARCRELEASE" verify --manifest "$work/m2/releases.json" --sig "$work/m2/releases.json.minisig" "${TRUST[@]}"
{
    echo "### Dry run: the manifest entry $version would publish"
    echo '~~~json'
    cat "$work/m2/releases.json"
    echo '~~~'
} >>"${GITHUB_STEP_SUMMARY:-/dev/stdout}"
```

Then make them executable and run the test:

```bash
git add --chmod=+x scripts/release/stage-fixture.sh scripts/release/check-stage.sh scripts/release/pack.sh scripts/release/dry-run-manifest.sh scripts/release/tests/test-dry-run.sh
bash scripts/release/tests/test-dry-run.sh
```

Expected: `test-dry-run: ok`.

- [ ] **Step 4: Write `release.yml`**

```yaml
# Publishes an Arcane engine release (docs/superpowers/specs/2026-10-12-arcane-distribution-design.md s4).
#   push of a v* tag  -> publish: build, sign, package, upload to R2, sign releases.json,
#                        GitHub Release, rebuild the website.
#   dispatch dry-run  -> a fixture build, packed and turned into a manifest locally;
#                        nothing uploaded, no secrets, any branch.
#   dispatch rehearsal (main only) -> a fixture build, really signed, published to
#                        staging/ with the TEST manifest key, which no Hub trusts.
# Rules (spec s4, roadmap D26): every third-party `uses:` pinned to a commit SHA
# (release-tooling checks); a read-only token unless a job needs more; secrets only
# in the `release` environment, whose jobs wait for the owner's approval; no
# pull_request trigger. The engine stage is phase 1; until it lands a tag fails
# closed at that step. Runbook: docs/releasing.md.
name: release

on:
  push:
    tags: ['v[0-9]*']
  workflow_dispatch:
    inputs:
      mode:
        description: dry-run (nothing uploaded, no secrets) or rehearsal (main only; publishes a fixture to staging/)
        type: choice
        options: [dry-run, rehearsal]
        default: dry-run

permissions:
  contents: read

concurrency:
  group: arcane-manifest
  cancel-in-progress: false

jobs:
  prepare:
    runs-on: ubuntu-24.04
    permissions:
      contents: read
      packages: read
    outputs:
      version: ${{ steps.run.outputs.version }}
      channel: ${{ steps.run.outputs.channel }}
      mode: ${{ steps.run.outputs.mode }}
      prefix: ${{ steps.run.outputs.prefix }}
      released_at: ${{ steps.run.outputs.released_at }}
      notes: ${{ steps.run.outputs.notes }}
      ci_image: ${{ steps.run.outputs.ci_image }}
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - name: Log in to GHCR (plan A's CI image is read, never built)
        if: github.event_name == 'push'
        run: echo "$GITHUB_TOKEN" | docker login ghcr.io -u "$GITHUB_ACTOR" --password-stdin
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
      - name: Decide the run
        id: run
        env:
          INPUT_MODE: ${{ inputs.mode }}
        run: bash scripts/release/prepare.sh

  stage:
    needs: prepare
    strategy:
      fail-fast: true
      matrix:
        include:
          - { os: windows, runner: windows-2025 }
          - { os: linux, runner: ubuntu-24.04 }
    runs-on: ${{ matrix.runner }}
    env:
      VERSION: ${{ needs.prepare.outputs.version }}
      MODE: ${{ needs.prepare.outputs.mode }}
      OS: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: ./.github/actions/arcrelease
      - name: Engine stage (phase 1)
        if: env.MODE == 'publish'
        shell: bash
        run: |
          echo "::error::Staging the real engine is phase 1 of the distribution spec (scripts/release/stage-engine.*), which has not landed; this tag cannot publish."
          exit 1
      - name: Fixture stage
        if: env.MODE != 'publish'
        shell: bash
        run: bash scripts/release/stage-fixture.sh "$VERSION" "$OS" stage
      - uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: stage-${{ matrix.os }}
          path: stage/
          if-no-files-found: error
          retention-days: 7

  sign-windows:
    needs: [prepare, stage]
    if: needs.prepare.outputs.mode != 'dry-run'
    runs-on: windows-2025
    environment: release
    permissions:
      contents: read
      id-token: write
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: actions/download-artifact@v7
        with:
          name: stage-windows
          path: stage
      - uses: ./.github/actions/sign-windows
        with:
          folder: ${{ github.workspace }}\stage\engine
          client-id: ${{ vars.AZURE_CLIENT_ID }}
          tenant-id: ${{ vars.AZURE_TENANT_ID }}
          subscription-id: ${{ vars.AZURE_SUBSCRIPTION_ID }}
          endpoint: ${{ vars.ARTIFACT_SIGNING_ENDPOINT }}
          account: ${{ vars.ARTIFACT_SIGNING_ACCOUNT }}
          profile: ${{ vars.ARTIFACT_SIGNING_PROFILE }}
      - uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: stage-windows-signed
          path: stage/
          if-no-files-found: error
          retention-days: 7

  package:
    needs: [prepare, stage, sign-windows]
    # Unsigned Windows binaries may be packed only in a dry run (fail closed).
    if: >-
      ${{ !cancelled() && needs.stage.result == 'success' &&
      (needs.sign-windows.result == 'success' ||
      (needs.sign-windows.result == 'skipped' && needs.prepare.outputs.mode == 'dry-run')) }}
    strategy:
      fail-fast: true
      matrix:
        include:
          - { os: windows, runner: windows-2025 }
          - { os: linux, runner: ubuntu-24.04 }
    runs-on: ${{ matrix.runner }}
    env:
      VERSION: ${{ needs.prepare.outputs.version }}
      OS: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: ./.github/actions/arcrelease
      - uses: actions/download-artifact@v7
        with:
          name: ${{ (matrix.os == 'windows' && needs.sign-windows.result == 'success') && 'stage-windows-signed' || format('stage-{0}', matrix.os) }}
          path: stage
      - name: Check the stage (the staged binary still runs after signing)
        shell: bash
        run: bash scripts/release/check-stage.sh stage "$VERSION" "$OS"
      - name: Pack
        shell: bash
        run: bash scripts/release/pack.sh stage "dist/$OS" "$VERSION" "$OS"
      - uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: dist-${{ matrix.os }}
          path: dist/
          if-no-files-found: error
          retention-days: 30

  dry-run-manifest:
    needs: [prepare, package]
    if: needs.prepare.outputs.mode == 'dry-run'
    runs-on: ubuntu-24.04
    env:
      VERSION: ${{ needs.prepare.outputs.version }}
      RELEASED_AT: ${{ needs.prepare.outputs.released_at }}
      NOTES: ${{ needs.prepare.outputs.notes }}
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: ./.github/actions/arcrelease
      - uses: actions/download-artifact@v7
        with:
          pattern: dist-*
          merge-multiple: true
          path: dist
      - run: bash scripts/release/dry-run-manifest.sh dist "$VERSION" "$RELEASED_AT" "$NOTES"

  publish:
    needs: [prepare, package]
    if: needs.prepare.outputs.mode != 'dry-run'
    runs-on: ubuntu-24.04
    environment: release
    permissions:
      contents: write
    env:
      MODE: ${{ needs.prepare.outputs.mode }}
      PREFIX: ${{ needs.prepare.outputs.prefix }}
      VERSION: ${{ needs.prepare.outputs.version }}
      CHANNEL: ${{ needs.prepare.outputs.channel }}
      RELEASED_AT: ${{ needs.prepare.outputs.released_at }}
      NOTES: ${{ needs.prepare.outputs.notes }}
      R2_ACCOUNT_ID: ${{ vars.R2_ACCOUNT_ID }}
      R2_BUCKET: ${{ vars.R2_BUCKET }}
      AWS_ACCESS_KEY_ID: ${{ secrets.R2_ACCESS_KEY_ID }}
      AWS_SECRET_ACCESS_KEY: ${{ secrets.R2_SECRET_ACCESS_KEY }}
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: ./.github/actions/arcrelease
      - name: Install the pinned aws-cli
        run: bash scripts/release/install-aws-cli.sh
      - uses: actions/download-artifact@v7
        with:
          pattern: dist-*
          merge-multiple: true
          path: dist
      - name: Upload the archives, then sign and publish releases.json
        env:
          MINISIGN_SECRET_KEY: ${{ secrets.MINISIGN_SECRET_KEY }}
          MINISIGN_PASSWORD: ${{ secrets.MINISIGN_PASSWORD }}
        run: bash scripts/release/publish.sh "$MODE" "$PREFIX" "$VERSION" "$RELEASED_AT" "$NOTES" dist "$RUNNER_TEMP/publish"
      - name: List archives left unreferenced
        if: failure()
        run: bash scripts/release/list-unreferenced.sh "$PREFIX" "$VERSION" >> "$GITHUB_STEP_SUMMARY"
      - name: GitHub Release (a permanent date record, no files)
        if: env.MODE == 'publish'
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          flags=()
          if [ "$CHANNEL" = preview ]; then flags=(--prerelease); fi
          gh release create "$GITHUB_REF_NAME" --verify-tag --title "Arcane $VERSION" --notes-file "$NOTES" "${flags[@]}"
      # The website rebuilds itself (its workflow, Task 16); this only tells it to.
      # A StarworksDev GitHub App token scoped to the website repo, never a personal token.
      - name: Website token
        id: site-token
        if: env.MODE == 'publish'
        uses: actions/create-github-app-token@v2
        with:
          client-id: ${{ vars.STARWORKS_APP_CLIENT_ID }}
          private-key: ${{ secrets.STARWORKS_APP_PRIVATE_KEY }}
          owner: StarworksDev
          repositories: StarworksWebsite
      - name: Rebuild starworks.dev
        if: env.MODE == 'publish'
        env:
          GH_TOKEN: ${{ steps.site-token.outputs.token }}
        run: |
          gh api repos/StarworksDev/StarworksWebsite/dispatches -f event_type=arcane-release \
            -F "client_payload[version]=$VERSION" -F "client_payload[channel]=$CHANNEL" \
            || echo "::warning::The website dispatch failed. The release is published; run the website's arcane-release workflow by hand."
```

- [ ] **Step 5: Write `docs/releasing.md`** and the CLAUDE.md pointer

`docs/releasing.md`:

````markdown
# Releasing Arcane

How an engine release reaches `downloads.starworks.dev` (design:
`docs/superpowers/specs/2026-10-12-arcane-distribution-design.md`; plan:
`docs/superpowers/plans/2026-10-12-arcane-distribution-phase2-pipeline.md`).

## Cutting a release

1. Set the version in `Arcane/Version.hpp` (`kVersionMajor`, `kVersionMinor`,
   `kVersionPatch`, `kVersionPrerelease`: `""` for stable, `"preview.N"` for
   a preview).
2. Write `docs/releases/<version>.md` (see `docs/releases/README.md`).
3. Merge both to `main`.
4. Tag and push it:

   ```bash
   git tag -a v0.3.1 -m "Arcane 0.3.1"
   git push origin v0.3.1
   ```
5. In Actions → release, approve the `release` environment's jobs (signing,
   then publishing). The run builds, signs, packs, uploads, signs
   `releases.json`, creates the GitHub Release and rebuilds the website.

Everything fails closed. A tag that doesn't match `Version.hpp`, missing
notes, an unpublished CI image, a failed test, signing or signature check, an
object that already exists with different content, or a manifest that doesn't
verify each stops the run before `releases.json` changes. Archives uploaded
before a failure stay and are never overwritten: the job summary lists them,
and re-running the workflow accepts them if they're identical.

`released_at` is when the run started. The archive's LICENSE (phase 1) and the
manifest share it, and it is never rewritten (spec s12).

## Dry runs and rehearsals

Actions → release → Run workflow:
- **dry-run** (any branch): builds a fixture, packs it and prints the manifest
  entry it would publish. No secrets, no upload.
- **rehearsal** (main only): the same fixture, really signed with Artifact
  Signing, published to `downloads.starworks.dev/staging/` with the TEST
  manifest key. Use it after changing the pipeline or the hosting.
````

Add this line to `CLAUDE.md`'s repo map, directly under the `ArcaneHub/` line:

```
ArcaneHub/crates/arcane_release/  # releases.json: schema, signing, verification, archives; `arcrelease` CLI (docs/releasing.md)
```

- [ ] **Step 6: Pin, lint and verify**

```bash
bash scripts/release/pin-actions.sh .github/workflows/release.yml
bash scripts/release/check-pinned.sh .github/workflows/release.yml
docker run --rm -v "$PWD:/repo" -w /repo rhysd/actionlint:1.7.7 -color .github/workflows/release.yml
bash scripts/release/tests/run-all.sh
```

Before pinning, confirm that `actions/download-artifact` and `actions/create-github-app-token` are written at their current majors (`gh release view -R actions/download-artifact`, `gh release view -R actions/create-github-app-token`). Download-artifact's major should match upload-artifact's v7.

Expected: pins written, `check-pinned` exits 0, actionlint is silent, and every test file reports `0 failed` or `ok`. A real dry-run needs the workflow on `main`, because `workflow_dispatch` only dispatches workflows on the default branch. That run happens after merge, in Task 15 Step 1.

- [ ] **Step 7: Commit**

```bash
git add .github/workflows/release.yml scripts/release docs/releasing.md CLAUDE.md
git commit -m "feat(release): release workflow -- dry run, rehearsal to staging/, publish behind approval; runbook"
```

---

### Task 12: Manifest maintenance workflow (monthly re-sign, yank, init, repair)

**Files:**
- Create: `.github/workflows/manifest-admin.yml`
- Modify: `docs/releasing.md` (add "Maintaining the manifest")

**Interfaces:**
- Consumes: `manifest-admin.sh` (Task 7), `install-aws-cli.sh`, the `arcrelease` action.
- Produces: a monthly re-sign behind approval, plus manual `resign|yank|init|repair` against `production|staging`.

- [ ] **Step 1: Write the workflow**

```yaml
# Maintains releases.json (spec s4 "Re-signing" and "Channels and yanking"; s9).
# The 1st of each month it re-signs production: the manifest expires 90 days
# after signing, so two missed approvals are survivable. By hand: resign, yank
# (the archives stay), init (the first publish only) or repair (restore
# releases.json + its signature from the newest verified history/ copy).
# Every run waits for the owner's approval in the `release` environment.
name: manifest-admin

on:
  schedule:
    - cron: '17 6 1 * *'
  workflow_dispatch:
    inputs:
      op:
        description: resign, yank, init (first publish only) or repair
        type: choice
        options: [resign, yank, init, repair]
        default: resign
      target:
        description: production (the bucket root, the release key) or staging (staging/, the test key)
        type: choice
        options: [production, staging]
        default: production
      version:
        description: 'yank only: the version'
        type: string
        required: false
      reason:
        description: 'yank only: the reason users see'
        type: string
        required: false

permissions:
  contents: read

concurrency:
  group: arcane-manifest
  cancel-in-progress: false

jobs:
  admin:
    if: github.ref == 'refs/heads/main'
    runs-on: ubuntu-24.04
    environment: release
    env:
      OP: ${{ inputs.op || 'resign' }}
      TARGET: ${{ inputs.target || 'production' }}
      VERSION: ${{ inputs.version }}
      REASON: ${{ inputs.reason }}
      R2_ACCOUNT_ID: ${{ vars.R2_ACCOUNT_ID }}
      R2_BUCKET: ${{ vars.R2_BUCKET }}
      AWS_ACCESS_KEY_ID: ${{ secrets.R2_ACCESS_KEY_ID }}
      AWS_SECRET_ACCESS_KEY: ${{ secrets.R2_SECRET_ACCESS_KEY }}
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          persist-credentials: false
      - uses: ./.github/actions/arcrelease
      - name: Install the pinned aws-cli
        run: bash scripts/release/install-aws-cli.sh
      - name: ${{ env.OP }} ${{ env.TARGET }}
        env:
          MINISIGN_SECRET_KEY: ${{ secrets.MINISIGN_SECRET_KEY }}
          MINISIGN_PASSWORD: ${{ secrets.MINISIGN_PASSWORD }}
        run: bash scripts/release/manifest-admin.sh "$OP" "$TARGET" "$VERSION" "$REASON"
```

- [ ] **Step 2: Append "Maintaining the manifest" to `docs/releasing.md`**

````markdown
## Maintaining the manifest

`releases.json` expires 90 days after it is signed, and Hubs refuse an
expired manifest (installed engines keep working; installs and updates
stop). On the 1st of each month `manifest-admin` re-signs it. **Approve that
run.** Two missed months are still survivable; three are not.

GitHub disables scheduled workflows in a public repository after 60 days
without repository activity. If Arcane goes quiet that long, re-enable
manifest-admin (Actions → manifest-admin → Enable) or run it by hand.

By hand (Actions → manifest-admin → Run workflow, from `main`):
- **resign**: sign again now.
- **yank** (version and reason): marks it yanked. The archives stay, so
  projects pinned to it still install it; the Hub and the website show the
  reason. `released_at` is never touched.
- **init**: writes serial 1. For the first publish to a target only; it
  refuses if a manifest exists.
- **repair**: if `releases.json` and its `.minisig` disagree (a publish failed
  between the two writes, and the run says so), this restores both from the
  newest `history/<serial>.json` pair that verifies. Then re-run whatever
  failed.

Every serial is also stored, immutably, at `history/<serial>.json(.minisig)`:
a record of every manifest ever published.
````

- [ ] **Step 3: Pin, lint and commit**

```bash
bash scripts/release/pin-actions.sh .github/workflows/manifest-admin.yml
bash scripts/release/check-pinned.sh .github/workflows/release.yml .github/workflows/manifest-admin.yml .github/workflows/release-tooling.yml .github/actions/*/action.yml
docker run --rm -v "$PWD:/repo" -w /repo rhysd/actionlint:1.7.7 -color .github/workflows/release.yml .github/workflows/manifest-admin.yml .github/workflows/release-tooling.yml
git add .github/workflows/manifest-admin.yml docs/releasing.md
git commit -m "feat(release): manifest-admin workflow -- monthly re-sign behind approval, yank, init, repair"
```

Expected: `check-pinned` exits 0 and actionlint is silent.

After this commit, open the PR. `release-tooling` must be green on it: the crate on both OSes, the scripts, the signature checker, and the pins and lint.

---

### Task 13 (owner): Keys and the `release` environment

The owner does this by hand: the keys are generated on the owner's machine, never in CI. An agent prepares the commands and code change, and the owner runs the ceremony.

**Files:**
- Create: `ArcaneHub/crates/arcane_release/keys/arcane-manifest-1.pub`
- Modify: `ArcaneHub/crates/arcane_release/src/keys.rs`
- Modify: `ArcaneHub/crates/arcane_release/tests/verify_test.rs`
- Modify: `docs/releasing.md` (add "Keys")

- [ ] **Step 1: Generate the production manifest key (owner, own machine)**

```bash
cd ArcaneHub/crates/arcane_release
read -rs ARCANE_MINISIGN_PASSWORD && export ARCANE_MINISIGN_PASSWORD   # a long passphrase from your password manager
cargo run --release --bin arcrelease -- keygen --out-dir "$HOME/arcane-keys" --name arcane-manifest-1
unset ARCANE_MINISIGN_PASSWORD
```

Expected: a base64 public key is printed, and `~/arcane-keys/arcane-manifest-1.{key,pub}` exist.

- [ ] **Step 2: Generate the Tauri updater key** (used in phase 4; spec s11 puts the keys in phase 2)

```bash
cd ArcaneHub && npx tauri signer generate -w "$HOME/arcane-keys/arcane-hub-updater.key"
```

- [ ] **Step 3: Back up both (owner)**

Store each `.key` file and its passphrase in your password manager. Also put the key files on an offline USB drive, kept separately from the passphrases. Spec s12: lose the manifest key and every installed Hub refuses new manifests; lose the Tauri key and installed Hubs can never self-update.

- [ ] **Step 4: Trust the production key**

Copy `~/arcane-keys/arcane-manifest-1.pub` to `ArcaneHub/crates/arcane_release/keys/arcane-manifest-1.pub`. In `src/keys.rs`, replace the comment inside `TRUSTED_MANIFEST_KEYS` with the file's base64 line as a string literal:

```rust
pub const TRUSTED_MANIFEST_KEYS: &[&str] = &[
    // keys/arcane-manifest-1.pub (2026 key ceremony)
    "RW...the base64 line...",
];
```

Append to `tests/verify_test.rs`:

```rust
#[test]
fn the_production_key_file_is_the_trusted_key() {
    let text = std::fs::read_to_string(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("keys/arcane-manifest-1.pub")).unwrap();
    let line = text.lines().map(str::trim).rfind(|l| !l.is_empty()).unwrap();
    assert_eq!(keys::TRUSTED_MANIFEST_KEYS, [line]);
}
```

Run: `cd ArcaneHub/crates/arcane_release && cargo test --test verify_test`
Expected: PASS.

- [ ] **Step 5: Create the `release` environment (owner, GitHub)**

In StarworksDev/Arcane → Settings → Environments → **New environment** `release`:
- **Required reviewers:** yourself. Leave **Prevent self-review** off, because you approve your own runs.
- **Deployment branches and tags:** "Selected branches and tags": branch `main` and tag `v[0-9]*`.

Then add the secrets:

```bash
gh secret set MINISIGN_SECRET_KEY --env release --repo StarworksDev/Arcane < "$HOME/arcane-keys/arcane-manifest-1.key"
gh secret set MINISIGN_PASSWORD --env release --repo StarworksDev/Arcane          # paste the passphrase
gh secret set TAURI_SIGNING_PRIVATE_KEY --env release --repo StarworksDev/Arcane < "$HOME/arcane-keys/arcane-hub-updater.key"
gh secret set TAURI_SIGNING_PRIVATE_KEY_PASSWORD --env release --repo StarworksDev/Arcane
```

Finally, under Settings → Actions → General → "Fork pull request workflows from outside collaborators", choose "Require approval for all outside collaborators" (roadmap D26).

- [ ] **Step 6: Append "Keys" to `docs/releasing.md`**

````markdown
## Keys

- **Manifest key** `arcane-manifest-1`: signs `releases.json`. Secret:
  `MINISIGN_SECRET_KEY` + `MINISIGN_PASSWORD` in the `release` environment.
  Its public half is `ArcaneHub/crates/arcane_release/keys/` and
  `keys::TRUSTED_MANIFEST_KEYS`, built into every Hub.
- **Tauri updater key** `arcane-hub-updater`: signs Hub updates (phase 4).
  Secret: `TAURI_SIGNING_PRIVATE_KEY` + `_PASSWORD`.
- Both have offline backups (password manager + offline drive). Losing either
  strands installed Hubs (spec s12).

**Rotating the manifest key:**
1. Generate `arcane-manifest-2`.
2. Add it to `TRUSTED_MANIFEST_KEYS` beside key 1.
3. Ship a Hub release.
4. Wait until the Hub's `min_supported` is at or past that release.
5. Switch the `release` secrets to key 2.
6. Remove key 1 in a later Hub release.

Never put a test key in the trusted list (a test enforces this).
````

- [ ] **Step 7: Commit**

```bash
git add ArcaneHub/crates/arcane_release/keys ArcaneHub/crates/arcane_release/src/keys.rs ArcaneHub/crates/arcane_release/tests/verify_test.rs docs/releasing.md
git commit -m "feat(release): trust the production manifest key arcane-manifest-1; key runbook"
```

---

### Task 14 (owner): Cloudflare R2, the download domain and Cache Rules

**Files:**
- Create: `scripts/release/check-hosting.sh`
- Modify: `docs/releasing.md` (add "Hosting")

- [ ] **Step 1: Create the bucket and domain (owner, Cloudflare dashboard)**

1. R2 → **Create bucket** `arcane-downloads`, location automatic.
2. Bucket → Settings → **Custom Domains** → Connect `downloads.starworks.dev`. The starworks.dev zone is on the same account.
3. Leave the bucket's **r2.dev** public URL **disabled**: it is rate-limited and must never be the path (spec s3).

- [ ] **Step 2: Cache Rules (owner; zone starworks.dev → Caching → Cache Rules)**

Create two rules, in this order:
1. **"arcane manifests: bypass".** When hostname equals `downloads.starworks.dev` AND (URI path ends with `.json` OR ends with `.minisig`), then **Bypass cache**.
2. **"arcane archives: cache".** When hostname equals `downloads.starworks.dev` AND the URI path ends with any of `.zip`, `.gz`, `.AppImage`, `.deb`, `.exe` or `.sig`, then **Eligible for cache**, with Edge TTL "Use cache-control header if present". The scripts upload archives with `public, max-age=31536000, immutable`.

- [ ] **Step 3: The R2 token (owner)**

Go to R2 → Manage R2 API Tokens → **Create API token**:
- permission **Object Read & Write**;
- **Apply to specific buckets only**: `arcane-downloads`;
- no expiry.

Then store the values:

```bash
gh variable set R2_ACCOUNT_ID --env release --repo StarworksDev/Arcane --body "<account id>"
gh variable set R2_BUCKET --env release --repo StarworksDev/Arcane --body arcane-downloads
gh secret set R2_ACCESS_KEY_ID --env release --repo StarworksDev/Arcane
gh secret set R2_SECRET_ACCESS_KEY --env release --repo StarworksDev/Arcane
```

- [ ] **Step 4: Prove R2 honours the conditional writes (owner, own machine)**

Cloudflare's docs list `If-None-Match` and `If-Match` on PutObject but don't document the 412 behaviour, so check it against the real bucket. Put the token in the environment and run:

```bash
export R2_ACCOUNT_ID=... R2_BUCKET=arcane-downloads AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=...
echo one > /tmp/one; echo two > /tmp/two
bash -c '. scripts/release/lib.sh; r2_init; put_immutable /tmp/one staging/smoke/once.txt'
bash -c '. scripts/release/lib.sh; r2_init; put_immutable /tmp/one staging/smoke/once.txt'     # "already uploaded, identical"
bash -c '. scripts/release/lib.sh; r2_init; put_immutable /tmp/two staging/smoke/once.txt'     # must FAIL: different content
bash -c '. scripts/release/lib.sh; r2_init; r2 put-object --bucket "$R2_BUCKET" --key staging/smoke/once.txt --body /tmp/two --if-match "\"nope\""' # must FAIL (412)
```

Expected: the first succeeds, the second says "identical", and the third and fourth fail. If the third or fourth succeeds, STOP. R2 is not honouring conditional writes, and the immutability guarantees need a different mechanism before anything is published.

- [ ] **Step 5: Initialise the manifests**

Run Actions → manifest-admin from `main` twice and approve each: once with `op=init`, `target=staging`, then with `op=init`, `target=production`.
Expected: `https://downloads.starworks.dev/releases.json` serves serial 1. `arcrelease verify` (without `--trust-key`, since the production key is now built in) accepts it after downloading the pair:

```bash
curl -fsSO https://downloads.starworks.dev/releases.json && curl -fsSO https://downloads.starworks.dev/releases.json.minisig
arcrelease verify --manifest releases.json --sig releases.json.minisig
```

- [ ] **Step 6: The cross-repo token for the website rebuild (owner)**

The publish job sends a `repository_dispatch` to the website repo, using a token minted from a StarworksDev **GitHub App**: the StarworksBuilder decision, never a personal token. A `repository_dispatch` needs the app to have **Contents: Read and write** on the target repository (`StarworksDev/StarworksWebsite`).

1. If the org already has the StarworksBuilder app, open it (StarworksDev → Settings → Developer settings → GitHub Apps). Otherwise create it there as **New GitHub App**:
   - name `StarworksBuilder`;
   - no webhook (untick Active);
   - **Repository permissions:** Contents: Read and write, and Metadata: Read-only (required);
   - "Only on this account".
2. Install it on StarworksDev, choosing **Only select repositories** and including `StarworksWebsite`.
3. Generate a private key (App settings → Private keys), and store the app's **Client ID** (starts with `Iv`, on the app's General page; not the numeric App ID) and key in Arcane's `release` environment:

```bash
gh variable set STARWORKS_APP_CLIENT_ID --env release --repo StarworksDev/Arcane --body "<client id>"
gh secret set STARWORKS_APP_PRIVATE_KEY --env release --repo StarworksDev/Arcane < ~/Downloads/starworksbuilder.*.private-key.pem
```

Delete the downloaded `.pem` after storing it. Back it up only if you want to avoid generating a new key later; a lost app key is replaced by generating another.

- [ ] **Step 7: Write `check-hosting.sh`** and append "Hosting" to `docs/releasing.md`

`scripts/release/check-hosting.sh`:

```bash
#!/usr/bin/env bash
# check-hosting.sh [BASE] -- checks downloads.starworks.dev serves the bucket as
# spec s3/s12 need: the manifest pair never edge-cached, archives cached.
# BASE defaults to the staging area; run it after a rehearsal.
set -euo pipefail
base=${1:-https://downloads.starworks.dev/staging/}
status() { curl -fsS -o /dev/null -D - "$1" | tr -d '\r' | awk -F': ' 'tolower($1) == "cf-cache-status" { print toupper($2) }'; }
fail=0
for f in releases.json releases.json.minisig; do
    curl -fsS -o /dev/null "$base$f"
    s=$(status "$base$f")
    if [ "$s" = HIT ]; then echo "FAIL $f is edge-cached; the bypass Cache Rule is missing"; fail=1; else echo "ok   $f (cf-cache-status ${s:-none})"; fi
done
archive=$(curl -fsS "${base}releases.json" | jq -r '.releases[0].artifacts[0].path // empty')
[ -n "$archive" ] || { echo "FAIL no release in ${base}releases.json (run a rehearsal first)"; exit 1; }
curl -fsS -o /dev/null "$base$archive"
s=$(status "$base$archive")
if [ "$s" = HIT ]; then echo "ok   $archive cached (HIT)"; else echo "FAIL $archive not cached on a second request (${s:-none})"; fail=1; fi
exit $fail
```

```bash
git add --chmod=+x scripts/release/check-hosting.sh
```

````markdown
## Hosting

- Bucket `arcane-downloads`, served only at `https://downloads.starworks.dev/`
  (r2.dev disabled).
- Cache Rules on starworks.dev: `.json` and `.minisig` bypass the cache (a
  cached manifest beside a fresh signature, or the reverse, would fail
  verification); archives are cached, immutable.
- `staging/` holds rehearsals, signed with the test key no Hub trusts.
- `history/<serial>.json(.minisig)` holds every manifest ever published.
- The R2 token can read and write `arcane-downloads` only. CI uploads through
  the S3 API (aws-cli, pinned in `scripts/release/install-aws-cli.sh`), never
  Wrangler, whose `r2 object put` caps objects at 315 MB.
- Check the setup after a rehearsal: `bash scripts/release/check-hosting.sh`.
````

- [ ] **Step 8: Commit**

```bash
git add scripts/release/check-hosting.sh docs/releasing.md
git commit -m "feat(release): hosting check and runbook -- R2 bucket, custom domain, cache bypass for the manifest pair"
```

---

### Task 15 (owner): Artifact Signing, OIDC, and the first rehearsal

This step costs money: Artifact Signing Basic is about $9.99 a month, billed for the whole month even when started part-way through. Do it when you're ready to keep paying, and not before Tasks 1–14 are merged.

**Files:**
- Modify: `docs/releasing.md` (add "Signing")

- [ ] **Step 1: A dry run first (free)**

After the PR merges, run Actions → release → Run workflow → `dry-run` from `main`.
Expected: green. No job enters the `release` environment, and the summary shows the manifest entry for `0.0.0-preview.<run>`.

- [ ] **Step 2: Azure account (owner, portal)**

1. Upgrade the subscription from the free trial to **pay-as-you-go** (Subscriptions → your subscription → Upgrade). Use billing account type **Individual**, with your legal name and address exactly as on your government ID. Artifact Signing does not run on free or trial subscriptions.
2. Register the resource provider: `az provider register --namespace Microsoft.CodeSigning`. Alternatively: Subscriptions → Resource providers → `Microsoft.CodeSigning` → Register.
3. Create resource group `rg-arcane-signing` in **East US**. Create an **Artifact Signing account** `arcane-signing` in it with the **Basic** SKU. Its endpoint is `https://eus.codesigning.azure.net/`.
4. Give yourself the **Artifact Signing Identity Verifier** role on the account (Access control (IAM) → Add role assignment). Then do Identity validation → New → **Public** → **Individual**, and complete the Microsoft Verified ID flow (government ID and a selfie in Microsoft Authenticator). Wait for **Completed**.
5. Certificate profiles → Create → **Public Trust**: select the completed validation and name it `arcane-public-trust`.

- [ ] **Step 3: OIDC federation, so there's no stored secret (owner)**

1. Go to Entra ID → App registrations → New registration, and register `arcane-release-signing` (single tenant).
2. On that app, go to Certificates & secrets → Federated credentials → Add. Choose "GitHub Actions deploying Azure resources" and fill in:
   - organization `StarworksDev`;
   - repository `Arcane`;
   - entity **Environment**, named `release`.

   This produces the subject `repo:StarworksDev/Arcane:environment:release`.
3. On the Artifact Signing account (or the certificate profile), go to Access control → Add role assignment. Give the app's service principal **Artifact Signing Certificate Profile Signer**; older portal views call it "Trusted Signing Certificate Profile Signer". If `azure/login` later reports no subscriptions, assign the role at the resource group scope instead.
4. Store the IDs. They are not secrets, so they go in as variables:

```bash
gh variable set AZURE_CLIENT_ID --env release --repo StarworksDev/Arcane --body "<app (client) id>"
gh variable set AZURE_TENANT_ID --env release --repo StarworksDev/Arcane --body "<tenant id>"
gh variable set AZURE_SUBSCRIPTION_ID --env release --repo StarworksDev/Arcane --body "<subscription id>"
gh variable set ARTIFACT_SIGNING_ENDPOINT --env release --repo StarworksDev/Arcane --body https://eus.codesigning.azure.net/
gh variable set ARTIFACT_SIGNING_ACCOUNT --env release --repo StarworksDev/Arcane --body arcane-signing
gh variable set ARTIFACT_SIGNING_PROFILE --env release --repo StarworksDev/Arcane --body arcane-public-trust
```

- [ ] **Step 4: The rehearsal**

Run Actions → release → Run workflow → `rehearsal` from `main`, and approve the signing and publishing jobs.
Expected: green, and then:

```bash
bash scripts/release/check-hosting.sh                              # ok on all three lines
curl -fsSO https://downloads.starworks.dev/staging/releases.json && curl -fsSO https://downloads.starworks.dev/staging/releases.json.minisig
arcrelease verify --manifest releases.json --sig releases.json.minisig --trust-key ArcaneHub/crates/arcane_release/tests/keys/test-manifest-1.pub
```

Then download the Windows zip named in that manifest, extract it, and check the signature in PowerShell:

```powershell
(Get-AuthenticodeSignature .\arcane-0.0.0-preview.<run>\bin\arcrelease.exe) | Format-List Status, SignerCertificate, TimeStamperCertificate
```

Expected: `Status : Valid`, with the signer naming you (the individual) and a timestamp certificate present.

- [ ] **Step 5: Book the legal review (owner)**

Spec s11 puts the legal review in phase 2, and it must finish before the
first real `v*` tag (spec s12). Its scope:
- the BSL 1.1 parameters and change dates;
- whether a preview is the "first public distribution". The interim rule in
  `ops::change_date` assumes yes;
- the third-party notices (spec s7.6).

Record the outcome in `LICENSING.md`. Rehearsals to `staging/` don't need
to wait for it.

- [ ] **Step 6: Append "Signing" to `docs/releasing.md` and commit**

````markdown
## Signing

Windows binaries are signed by Azure Artifact Signing (Basic, East US,
account `arcane-signing`, profile `arcane-public-trust`). CI authenticates
through GitHub OIDC: the `release` environment's federated credential on the
Entra app `arcane-release-signing`. No secret is stored. The IDs live in the
environment's variables. A signing outage stops a release (it fails closed);
nothing ships unsigned (spec s12, Smart App Control).
````

```bash
git add docs/releasing.md
git commit -m "docs(release): signing runbook -- Artifact Signing over OIDC"
```

---

### Task 16: The website rebuilds on an Arcane release (website repo)

This task is in **`D:\dev\web\Starworks`**, not Arcane. Do it on a branch there once no other agent is working in that checkout, and merge through a PR like any website change.

**Files (website repo):**
- Create: `.github/workflows/arcane-release.yml`
- Modify: `docs/deployment.md` (one paragraph)

**Interfaces:**
- Consumes: the `repository_dispatch` with `event_type: arcane-release` and `client_payload: { version, channel }` that Arcane's publish job sends (Task 11).
- Produces: a fresh build of starworks.dev deployed to the `starworksdev` Pages project. Phase 6's loader then reads the new `releases.json` at build time.

- [ ] **Step 1: Find out how the Pages project deploys (owner)**

Open Cloudflare → Workers & Pages → `starworksdev` → Settings:
- If it shows a connected Git repository, it is **Git-integrated**. Create a deploy hook (Settings → Builds → Deploy hooks → `arcane-release`, branch `main`) and store it in the website repo: `gh secret set PAGES_DEPLOY_HOOK_URL --repo StarworksDev/StarworksWebsite`.
- Otherwise it is **Direct Upload**. Create a Cloudflare API token with the "Cloudflare Pages: Edit" permission for this account only, and store it:

```bash
gh secret set CLOUDFLARE_API_TOKEN --repo StarworksDev/StarworksWebsite
gh variable set CLOUDFLARE_ACCOUNT_ID --repo StarworksDev/StarworksWebsite --body "<account id>"
```

The workflow handles both cases: it POSTs the hook when one exists, and otherwise builds and runs `wrangler pages deploy`.

- [ ] **Step 2: Write the workflow**

`.github/workflows/arcane-release.yml`:

```yaml
# Rebuilds and deploys starworks.dev when Arcane publishes a release: Arcane's
# release workflow sends a repository_dispatch (arcane-release) with a token
# from the StarworksDev GitHub App. Also runnable by hand. From phase 6 the
# build reads releases.json; before that a rebuild is harmless.
# Git-integrated Pages project: POST its deploy hook. Direct Upload: build,
# check, and deploy with the repo's pinned wrangler.
name: arcane-release

on:
  repository_dispatch:
    types: [arcane-release]
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: site-deploy
  cancel-in-progress: false

jobs:
  deploy:
    runs-on: ubuntu-24.04
    env:
      HOOK: ${{ secrets.PAGES_DEPLOY_HOOK_URL }}
      VERSION: ${{ github.event.client_payload.version }}
    steps:
      - name: Which release
        run: echo "Rebuilding for Arcane ${VERSION:-(manual run)}"
      - name: Deploy hook (Git-integrated project)
        if: env.HOOK != ''
        run: curl -fsS -X POST "$HOOK" >/dev/null
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        if: env.HOOK == ''
        with:
          persist-credentials: false
      - uses: actions/setup-node@949feb2413d6458794dcd2491c4babbbce0c15c1 # v7.1.0
        if: env.HOOK == ''
        with:
          node-version-file: .nvmrc
          cache: npm
      - if: env.HOOK == ''
        run: npm ci
      - if: env.HOOK == ''
        run: npm run build
      - name: Deploy check (skips the gitignored private/ paths under CI)
        if: env.HOOK == ''
        run: npm run deploy:verify
      - name: Deploy (Direct Upload)
        if: env.HOOK == ''
        env:
          CLOUDFLARE_API_TOKEN: ${{ secrets.CLOUDFLARE_API_TOKEN }}
          CLOUDFLARE_ACCOUNT_ID: ${{ vars.CLOUDFLARE_ACCOUNT_ID }}
        run: npx wrangler pages deploy build --project-name starworksdev --branch main
```

- [ ] **Step 3: Lint, document, commit**

Run: `docker run --rm -v "$PWD:/repo" -w /repo rhysd/actionlint:1.7.7 -color .github/workflows/arcane-release.yml`
Expected: silent.

Add to `docs/deployment.md`:

```markdown
## Rebuilds on an Arcane release

`.github/workflows/arcane-release.yml` rebuilds and deploys the site when
Arcane publishes a release (a `repository_dispatch` from Arcane's release
workflow, sent with a StarworksDev GitHub App token), or by hand from the
Actions tab. It POSTs the Pages deploy hook if `PAGES_DEPLOY_HOOK_URL` is set
(a Git-integrated project); otherwise it builds and runs `wrangler pages
deploy` with `CLOUDFLARE_API_TOKEN`.
```

```bash
git add .github/workflows/arcane-release.yml docs/deployment.md
git commit -m "ci: rebuild and deploy the site on an Arcane release (repository_dispatch)"
```

- [ ] **Step 4: Prove it end to end (after Task 14 Step 6 and this task are merged)**

Run the workflow by hand (Actions → arcane-release → Run workflow).
Expected: green, and a new Pages deployment listed in Cloudflare.

Then from Arcane's side, send the dispatch the release job sends:

```bash
gh api repos/StarworksDev/StarworksWebsite/dispatches -f event_type=arcane-release -F "client_payload[version]=0.0.0-test"
```

Expected: an `arcane-release` run starts in the website repo. This uses your own `gh` login; the release job uses the app token.

---

### Task 17: A release-manifest watchdog in Starworks Ops (starworks-ops repo)

GitHub disables a public repository's scheduled workflows after 60 days without activity. A disabled schedule produces no failure and no notification; silence looks like health. With a 90-day expiry and monthly re-signs, the margin can be about 30 days. So a check outside GitHub's scheduler runs daily on the desk: the Starworks Ops bot, as a new feature module.

This task is in **`D:\dev\starworks\starworks-ops`**.

**Files (starworks-ops):**
- Create: `starworks_ops/features/release_watch/__init__.py`
- Create: `starworks_ops/features/release_watch/service.py`
- Modify: `starworks_ops/config.py` (add `ReleaseWatchConfig`, parse `[release_watch]`)
- Modify: `config.toml` (enable it only once the production manifest exists: Step 6)
- Modify: `README.md` (the feature table)
- Test: `tests/test_release_watch.py`; `tests/test_config.py` (two cases)

**Interfaces:**
- Consumes:
  - `Alerter.post_alert(key, text)`, which suppresses a repeat of the same key within 24 hours, so a daily check alerts once a day;
  - `Http.get_json(url) -> (status, body)`, which raises `TransientError` on network errors;
  - `util.parse_ts`, which reads `2027-01-10T00:00:00Z`.
- Produces:
  - `ReleaseWatchConfig(manifest_urls: tuple[str, ...], warn_days: int = 30, check_hours: int = 24)`;
  - `ReleaseWatchService.check() -> list[Reading]`;
  - the alert keys `release-watch:expiry:<url>` and `release-watch:read:<url>`.

- [ ] **Step 1: Write the failing tests**

`tests/test_release_watch.py`:

```python
from datetime import datetime, timedelta, timezone
from pathlib import Path

from starworks_ops.alerts import Alerter
from starworks_ops.config import ReleaseWatchConfig
from starworks_ops.features.release_watch.service import ReleaseWatchService
from starworks_ops.http import TransientError
from starworks_ops.state import StateStore
from tests.fakes import FakeClock, FakeHttp

T0 = datetime(2026, 10, 12, 12, 0, tzinfo=timezone.utc)
URL = "https://downloads.starworks.dev/releases.json"


def make(tmp_path: Path, responses, clock: FakeClock | None = None, warn_days: int = 30):
    clock = clock or FakeClock(T0)
    sent: list[str] = []

    async def send(text: str) -> None:
        sent.append(text)

    alerter = Alerter(send, StateStore(tmp_path / "alerts.json"), now=clock)
    cfg = ReleaseWatchConfig(manifest_urls=(URL,), warn_days=warn_days)
    return ReleaseWatchService(cfg, FakeHttp(responses), alerter, now=clock), sent


def manifest(expires: datetime) -> dict:
    return {"schema": 1, "serial": 7, "expires": expires.strftime("%Y-%m-%dT%H:%M:%SZ")}


async def test_plenty_of_time_is_quiet(tmp_path: Path):
    service, sent = make(tmp_path, [(200, manifest(T0 + timedelta(days=60)))])
    [reading] = await service.check()
    assert reading.problem is None
    assert round(reading.days_left) == 60
    assert sent == []


async def test_fewer_than_30_days_alerts_with_the_fix(tmp_path: Path):
    service, sent = make(tmp_path, [(200, manifest(T0 + timedelta(days=29)))])
    await service.check()
    assert len(sent) == 1
    assert "expires in 29 day(s)" in sent[0]
    assert "manifest-admin" in sent[0]


async def test_an_expired_manifest_says_so(tmp_path: Path):
    service, sent = make(tmp_path, [(200, manifest(T0 - timedelta(days=1)))])
    await service.check()
    assert "has EXPIRED" in sent[0]


async def test_alerts_repeat_daily_not_every_check(tmp_path: Path):
    clock = FakeClock(T0)
    soon = manifest(T0 + timedelta(days=10))
    service, sent = make(tmp_path, [(200, soon), (200, soon), (200, soon)], clock)
    await service.check()
    clock.advance(hours=6)
    await service.check()
    clock.advance(hours=18)
    await service.check()
    assert len(sent) == 2


async def test_unreadable_manifests_alert(tmp_path: Path):
    cases = [
        (TransientError("network error (ClientError)"), "could not be fetched"),
        ((404, None), "returned HTTP 404"),
        ((200, {"schema": 1}), "has no expires field"),
        ((200, {"expires": "next tuesday"}), "unreadable expires"),
        ((200, ["not", "an", "object"]), "has no expires field"),
    ]
    for i, (response, words) in enumerate(cases):
        service, sent = make(tmp_path / str(i), [response])  # its own alert history
        [reading] = await service.check()
        assert reading.problem is not None
        assert len(sent) == 1 and words in sent[0], (words, sent)
```

Append to `tests/test_config.py`:

```python
def test_release_watch_defaults_when_enabled():
    data = tomllib.loads(VALID.replace('"presence", "social_feed"', '"presence", "social_feed", "release_watch"'))
    rw = parse_config(data).release_watch
    assert rw is not None
    assert rw.manifest_urls == ("https://downloads.starworks.dev/releases.json",)
    assert rw.warn_days == 30
    assert rw.check_hours == 24


def test_release_watch_urls_must_be_https():
    text = VALID.replace('"presence", "social_feed"', '"presence", "social_feed", "release_watch"')
    text += '\n[release_watch]\nmanifest_urls = ["http://downloads.starworks.dev/releases.json"]\n'
    with pytest.raises(ConfigError, match="manifest_urls"):
        parse_config(tomllib.loads(text))
```

- [ ] **Step 2: Run them to verify they fail**

Run: `.venv\Scripts\python -m pytest tests/test_release_watch.py tests/test_config.py -q`
Expected: FAIL. You'll see `ModuleNotFoundError: starworks_ops.features.release_watch` and `ImportError: cannot import name 'ReleaseWatchConfig'`.

- [ ] **Step 3: Add the config**

In `starworks_ops/config.py`, add after `SocialFeedConfig`:

```python
DEFAULT_MANIFEST_URL = "https://downloads.starworks.dev/releases.json"


@dataclass(frozen=True)
class ReleaseWatchConfig:
    manifest_urls: tuple[str, ...] = (DEFAULT_MANIFEST_URL,)
    warn_days: int = 30
    check_hours: int = 24
```

Give `AppConfig` a third field, `release_watch: ReleaseWatchConfig | None = None`, after `social_feed`. In `parse_config`, before the `return`, add:

```python
    release_watch = None
    if "release_watch" in bot.features:
        table = data.get("release_watch", {})
        if not isinstance(table, dict):
            raise ConfigError("[release_watch] must be a table")
        where = "release_watch"
        urls = _opt(table, "manifest_urls", list, where, [DEFAULT_MANIFEST_URL])
        if not urls or not all(isinstance(u, str) and u.startswith("https://") for u in urls):
            raise ConfigError("[release_watch].manifest_urls must be a non-empty list of https URLs")
        release_watch = ReleaseWatchConfig(
            manifest_urls=tuple(urls),
            warn_days=_opt(table, "warn_days", int, where, 30),
            check_hours=_opt(table, "check_hours", int, where, 24),
        )
        for key in ("warn_days", "check_hours"):
            if getattr(release_watch, key) < 1:
                raise ConfigError(f"[release_watch].{key} must be at least 1")
```

Change the `return` to `return AppConfig(bot=bot, social_feed=social_feed, release_watch=release_watch)`.

- [ ] **Step 4: Write the feature**

`starworks_ops/features/release_watch/service.py`:

```python
"""Release watch: alerts when Arcane's releases.json is close to expiring.

The manifest expires 90 days after it is signed, and a monthly GitHub workflow
re-signs it behind an approval. GitHub silently disables a public repository's
schedules after 60 days without activity, and a missed approval looks the same
from outside, so this check runs on the desk, outside GitHub. Once a day it
alerts the ops channel when fewer than warn_days remain, or when the manifest
can't be read. It only watches the clock: the Hub verifies the signature.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Callable

from ...alerts import Alerter
from ...config import ReleaseWatchConfig
from ...http import Http, TransientError
from ...util import parse_ts, utcnow

FIX = (
    "Approve the pending manifest-admin run in Arcane's Actions, or run it with op=resign, "
    "and check that its monthly schedule isn't disabled (Arcane docs/releasing.md)."
)


@dataclass(frozen=True)
class Reading:
    url: str
    expires: datetime | None
    days_left: float | None
    problem: str | None


class ReleaseWatchService:
    def __init__(self, cfg: ReleaseWatchConfig, http: Http, alerter: Alerter, now: Callable[[], datetime] = utcnow):
        self.cfg = cfg
        self.http = http
        self.alerter = alerter
        self.now = now
        self.last: list[Reading] = []

    async def read(self, url: str) -> Reading:
        try:
            status, body = await self.http.get_json(url)
        except TransientError as exc:
            return Reading(url, None, None, f"could not be fetched ({exc})")
        if status != 200:
            return Reading(url, None, None, f"returned HTTP {status}")
        expires = body.get("expires") if isinstance(body, dict) else None
        if not isinstance(expires, str):
            return Reading(url, None, None, "has no expires field")
        try:
            when = parse_ts(expires)
        except ValueError:
            return Reading(url, None, None, f"has an unreadable expires ({expires!r})")
        return Reading(url, when, (when - self.now()).total_seconds() / 86400, None)

    async def check(self) -> list[Reading]:
        readings = [await self.read(url) for url in self.cfg.manifest_urls]
        for r in readings:
            if r.problem is not None:
                await self.alerter.post_alert(
                    f"release-watch:read:{r.url}",
                    f"Arcane's release manifest {r.url} {r.problem}. If this lasts, Hubs see no new releases.",
                )
            elif r.days_left is not None and r.days_left < self.cfg.warn_days:
                state = "has EXPIRED" if r.days_left <= 0 else f"expires in {int(r.days_left)} day(s)"
                await self.alerter.post_alert(
                    f"release-watch:expiry:{r.url}",
                    f"Arcane's release manifest {r.url} {state} ({r.expires:%Y-%m-%d}); Hubs refuse it once expired. {FIX}",
                )
        self.last = readings
        return readings
```

`starworks_ops/features/release_watch/__init__.py`:

```python
"""Release watch: a daily expiry check of Arcane's releases.json (see service.py)."""
from __future__ import annotations

import logging
from typing import TYPE_CHECKING

import aiohttp
from discord.ext import tasks
from discord.ext.commands import Cog

from ...config import ReleaseWatchConfig
from ...http import AiohttpHttp
from .service import ReleaseWatchService

if TYPE_CHECKING:
    from ...bot import StarworksOps

log = logging.getLogger(__name__)


class ReleaseWatchCog(Cog):
    def __init__(self, bot: "StarworksOps", service: ReleaseWatchService, session: aiohttp.ClientSession, cfg: ReleaseWatchConfig):
        self.bot = bot
        self.service = service
        self.session = session
        self.check_loop = tasks.loop(hours=cfg.check_hours)(self._check)
        self.check_loop.before_loop(self._wait_ready)

    async def cog_load(self) -> None:
        self.check_loop.start()

    async def cog_unload(self) -> None:
        self.check_loop.cancel()
        await self.session.close()

    async def _wait_ready(self) -> None:
        await self.bot.wait_until_ready()

    async def _check(self) -> None:
        try:
            await self.service.check()
        except Exception:
            log.exception("release watch crashed; retrying next cycle")


async def setup(bot: "StarworksOps") -> None:
    cfg = bot.config.release_watch
    if cfg is None:
        raise RuntimeError("the [release_watch] config is missing")
    session = aiohttp.ClientSession(headers={"User-Agent": "StarworksOps release watch"})
    service = ReleaseWatchService(cfg, AiohttpHttp(session), bot.alerter)
    await bot.add_cog(ReleaseWatchCog(bot, service, session, cfg))
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `.venv\Scripts\python -m pytest -q`
Expected: the whole suite passes, the new tests included.

- [ ] **Step 6: Turn it on once the production manifest exists, and commit**

Leave `config.toml` alone until Task 14 Step 5 has published the production `releases.json`; before that, the check would alert "HTTP 404" every day. Then:
- add `"release_watch"` to `[bot].features`;
- add the table:

  ```toml
  [release_watch]
  manifest_urls = ["https://downloads.starworks.dev/releases.json"]
  warn_days = 30
  check_hours = 24
  ```
- add a row to the README's feature table: `` `release_watch` `` | "Checks Arcane's releases.json daily; alerts in #💬・dev-general when fewer than 30 days remain before it expires, or when it can't be read."

Restart the bot (`schtasks /End` then `/Run` on "Starworks Ops presence"). Expected: the log shows `feature loaded: release_watch`, and no alert while the manifest has more than 30 days left.

```bash
git add starworks_ops config.toml README.md tests
git commit -m "feat: release_watch -- daily expiry check of Arcane's releases.json, alerts under 30 days"
```

---

## What phase 1 plugs in

When phase 1's plan is written, it fills these slots without changing anything above:

- **`release.yml`, `stage` job:**
  - replace the "Engine stage (phase 1)" step with `scripts/release/stage-engine.{ps1,sh}`. These build Dist with debug info, split and match symbols, run the tests and the probe, and lay out `stage/engine` (engine.json, LICENSE stamped with `arcrelease change-date`, `THIRD_PARTY_NOTICES`) and `stage/symbols`;
  - run the Linux leg in `container: ${{ needs.prepare.outputs.ci_image }}`.
- **`check-stage.sh`:** compare `--print-engine-info` (`version`, `commit`, `engineAbi`) with `engine.json`, on the signed binaries.
- **`ci.yml`'s Dist legs:** run `stage-engine` + `check-stage.sh` + `pack.sh` on every pull request, with no signing and no upload. This is spec s10's packaging check, and it does not rebuild Dist.
- **`Version.hpp`:** add `kVersionPatch` and `kVersionPrerelease`.

## Rules phase 3 inherits

- **Release notes are untrusted text inside the Hub.** The Hub shows `notes` inside a Tauri webview, in an app that can launch processes. Render them as escaped text or through a Markdown renderer that sanitizes, never as raw HTML. The manifest is signed, but this is defense in depth.
- **Verify with `arcane_release`** (`default-features = false`), against `keys::TRUSTED_MANIFEST_KEYS`. Test against the goldens in `tests/golden/` at their documented `now` and highest serial.
- **Fetch the manifest pair as served.** The edge doesn't cache it. A signature that doesn't match is retried once, then the cached copy is kept, per spec s9.
