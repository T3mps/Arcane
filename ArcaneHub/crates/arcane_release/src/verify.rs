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
