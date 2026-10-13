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
