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
