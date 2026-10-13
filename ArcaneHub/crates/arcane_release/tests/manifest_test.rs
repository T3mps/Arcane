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

#[test]
fn unknown_fields_survive_a_rewrite() {
    // A schema-1 manifest may gain fields; an older arcrelease that re-signs it
    // must carry them over, not drop them from the signed bytes.
    let mut v: serde_json::Value = serde_json::from_slice(&manifest(vec![release("0.3.1")]).to_bytes()).unwrap();
    v["added_later"] = serde_json::json!({ "x": 1 });
    v["hub"]["hub_later"] = serde_json::json!(2);
    v["channels"]["lts"] = serde_json::json!("0.3.1");
    v["releases"][0]["release_later"] = serde_json::json!(true);
    v["releases"][0]["artifacts"][0]["artifact_later"] = serde_json::json!("y");
    let m = Manifest::parse(&serde_json::to_vec(&v).unwrap()).unwrap();
    let again: serde_json::Value = serde_json::from_slice(&m.to_bytes()).unwrap();
    assert_eq!(again["added_later"], serde_json::json!({ "x": 1 }));
    assert_eq!(again["hub"]["hub_later"], 2);
    assert_eq!(again["channels"]["lts"], "0.3.1");
    assert_eq!(again["releases"][0]["release_later"], true);
    assert_eq!(again["releases"][0]["artifacts"][0]["artifact_later"], "y");
}
