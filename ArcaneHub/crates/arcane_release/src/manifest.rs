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
