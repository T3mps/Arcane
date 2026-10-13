//! Arcane's release manifest, `releases.json` (spec
//! docs/superpowers/specs/2026-10-12-arcane-distribution-design.md s5): its
//! schema, the operations CI applies to it, minisign signing and verification,
//! and the engine archive packer. CI drives it through the `arcrelease`
//! binary; the Hub links the library with `default-features = false`.

pub mod keys;
pub mod manifest;
pub mod ops;
pub mod verify;
pub mod version;

#[cfg(feature = "tools")]
pub mod sign;
