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
