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
