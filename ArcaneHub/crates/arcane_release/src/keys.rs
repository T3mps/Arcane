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
