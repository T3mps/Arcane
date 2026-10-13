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
