//! `arcrelease`: the release tooling CI runs (spec s4, s5, s7.1). A command
//! that writes a manifest first verifies the one it replaces (its signature
//! and contents; expiry is not checked, so an expired manifest can be
//! re-signed), then verifies what it signed before writing anything, so a
//! wrong or untrusted key writes nothing. Secrets come only from the
//! environment: ARCANE_MINISIGN_KEY (the .key file's text) and
//! ARCANE_MINISIGN_PASSWORD.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{ensure, Context, Result};
use arcane_release::digest::{self, MANIFEST_TXT};
use arcane_release::manifest::{self, Manifest, BASE_URL};
use arcane_release::ops::{self, NewRelease};
use arcane_release::{dist, keys, pack, sign, verify};
use clap::{Args, Parser, Subcommand};
use minisign_verify::PublicKey;
use time::OffsetDateTime;

const KEY_ENV: &str = "ARCANE_MINISIGN_KEY";
const PASSWORD_ENV: &str = "ARCANE_MINISIGN_PASSWORD";

#[derive(Parser)]
#[command(name = "arcrelease", version, about = "Arcane release tooling: manifests, signing, archives")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Args)]
struct Trust {
    /// Trust this public key file instead of the keys built into this binary (staging and tests).
    #[arg(long)]
    trust_key: Option<PathBuf>,
}

#[derive(Args)]
struct Current {
    /// The releases.json being read or replaced.
    #[arg(long)]
    manifest: PathBuf,
    /// Its signature, releases.json.minisig.
    #[arg(long)]
    sig: PathBuf,
}

#[derive(Args)]
struct Clock {
    /// Use this RFC 3339 UTC time as "now" (tests).
    #[arg(long)]
    now: Option<String>,
}

#[derive(Args)]
struct Serial {
    /// The lowest serial the new manifest may take. CI passes 1 + the highest
    /// serial under history/, so a serial an interrupted run left there is skipped
    /// (Hubs only need serials to rise).
    #[arg(long)]
    min_serial: Option<u64>,
}

#[derive(Subcommand)]
enum Cmd {
    /// Generate an encrypted minisign key pair; the password comes from ARCANE_MINISIGN_PASSWORD.
    Keygen {
        #[arg(long)]
        out_dir: PathBuf,
        #[arg(long)]
        name: String,
    },
    /// Print a file's size and SHA-256 as JSON.
    Digest { file: PathBuf },
    /// Write manifest.txt into a stage folder, then pack it (.zip or .tar.gz) under one root folder.
    Pack {
        #[arg(long)]
        stage: PathBuf,
        #[arg(long)]
        root: String,
        #[arg(long)]
        out: PathBuf,
    },
    /// Check a folder against its manifest.txt (what the Hub's Verify does).
    VerifyTree {
        #[arg(long)]
        stage: PathBuf,
    },
    /// Write a new manifest at serial 1 (the first publish only).
    Init {
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[command(flatten)]
        serial: Serial,
        #[arg(long, default_value = BASE_URL)]
        base_url: String,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Add a release from a dist folder (dist/<os>/engine.json and its archives).
    Add {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[command(flatten)]
        serial: Serial,
        #[arg(long)]
        dist: PathBuf,
        #[arg(long)]
        version: String,
        #[arg(long)]
        released_at: String,
        #[arg(long)]
        notes_file: PathBuf,
        #[arg(long)]
        security: bool,
        #[arg(long)]
        min_hub_version: Option<String>,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Mark a release yanked, with the reason users see.
    Yank {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[command(flatten)]
        serial: Serial,
        #[arg(long)]
        version: String,
        #[arg(long)]
        reason: String,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Re-sign with the next serial and a fresh 90-day expiry.
    Resign {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[command(flatten)]
        serial: Serial,
        #[arg(long)]
        out_dir: PathBuf,
    },
    /// Verify a manifest the way the Hub does.
    Verify {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[command(flatten)]
        clock: Clock,
        #[arg(long)]
        highest_serial: Option<u64>,
        /// Check the signature and contents only, as CI does before replacing a manifest.
        #[arg(long)]
        signature_only: bool,
    },
    /// Print the BSL change date a version released at the given time would get.
    ChangeDate {
        #[command(flatten)]
        current: Current,
        #[command(flatten)]
        trust: Trust,
        #[arg(long)]
        version: String,
        #[arg(long)]
        released_at: String,
    },
}

fn main() -> Result<()> {
    match Cli::parse().cmd {
        Cmd::Keygen { out_dir, name } => keygen(&out_dir, &name),
        Cmd::Digest { file } => {
            println!("{}", serde_json::to_string(&digest::digest_file(&file)?)?);
            Ok(())
        }
        Cmd::Pack { stage, root, out } => {
            println!("{}", serde_json::to_string(&pack::pack(&stage, &root, &out)?)?);
            Ok(())
        }
        Cmd::VerifyTree { stage } => {
            let problems = digest::verify_tree(&stage).with_context(|| format!("checking against {}", stage.join(MANIFEST_TXT).display()))?;
            for problem in &problems {
                eprintln!("{problem:?}");
            }
            ensure!(problems.is_empty(), "{} file(s) differ from manifest.txt", problems.len());
            println!("ok");
            Ok(())
        }
        Cmd::Init { trust, clock, serial, base_url, out_dir } => {
            let now = now(&clock)?;
            let m = ops::empty(&base_url, now);
            m.validate()?;
            write_signed(floor(m, &serial), &keys_for(&trust)?, &out_dir, now)
        }
        Cmd::Add { current, trust, clock, serial, dist, version, released_at, notes_file, security, min_hub_version, out_dir } => {
            let now = now(&clock)?;
            let keys = keys_for(&trust)?;
            let cur = read_current(&current, &keys)?;
            let (info, artifacts) = dist::read_dist(&dist, &version)?;
            let notes = fs::read_to_string(&notes_file).with_context(|| format!("reading {}", notes_file.display()))?;
            let new = NewRelease {
                version,
                build: info.build,
                abi: info.abi,
                released_at: manifest::parse_ts(&released_at)?,
                min_hub_version,
                security,
                notes,
                artifacts,
            };
            write_signed(floor(ops::add_release(&cur, new, now)?, &serial), &keys, &out_dir, now)
        }
        Cmd::Yank { current, trust, clock, serial, version, reason, out_dir } => {
            let now = now(&clock)?;
            let keys = keys_for(&trust)?;
            let cur = read_current(&current, &keys)?;
            write_signed(floor(ops::yank(&cur, &version, &reason, now)?, &serial), &keys, &out_dir, now)
        }
        Cmd::Resign { current, trust, clock, serial, out_dir } => {
            let now = now(&clock)?;
            let keys = keys_for(&trust)?;
            let cur = read_current(&current, &keys)?;
            write_signed(floor(ops::resign(&cur, now)?, &serial), &keys, &out_dir, now)
        }
        Cmd::Verify { current, trust, clock, highest_serial, signature_only } => {
            let keys = keys_for(&trust)?;
            let bytes = fs::read(&current.manifest).with_context(|| format!("reading {}", current.manifest.display()))?;
            let sig = fs::read_to_string(&current.sig).with_context(|| format!("reading {}", current.sig.display()))?;
            let m = if signature_only {
                verify::verify_signed(&bytes, &sig, &keys)?
            } else {
                verify::verify_manifest(&bytes, &sig, &keys, highest_serial, now(&clock)?)?
            };
            println!("ok: serial {}, expires {}", m.serial, m.expires);
            Ok(())
        }
        Cmd::ChangeDate { current, trust, version, released_at } => {
            let cur = read_current(&current, &keys_for(&trust)?)?;
            let v = arcane_release::version::parse(&version)?;
            let released = manifest::parse_ts(&released_at)?.date();
            println!("{}", manifest::format_date(ops::change_date(&cur, &v, released)));
            Ok(())
        }
    }
}

/// Raises the new manifest's serial to `--min-serial` if it is lower.
fn floor(mut m: Manifest, serial: &Serial) -> Manifest {
    if let Some(min) = serial.min_serial {
        m.serial = m.serial.max(min);
    }
    m
}

fn now(clock: &Clock) -> Result<OffsetDateTime> {
    Ok(match &clock.now {
        Some(s) => manifest::parse_ts(s)?,
        None => OffsetDateTime::now_utc(),
    })
}

fn keys_for(trust: &Trust) -> Result<Vec<PublicKey>> {
    if let Some(path) = &trust.trust_key {
        let text = fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
        let key = verify::parse_public_key(&text).with_context(|| format!("{} is not a minisign public key", path.display()))?;
        return Ok(vec![key]);
    }
    let built_in = keys::trusted();
    ensure!(!built_in.is_empty(), "this arcrelease trusts no manifest keys yet (keys::TRUSTED_MANIFEST_KEYS is empty); pass --trust-key");
    Ok(built_in)
}

fn read_current(current: &Current, keys: &[PublicKey]) -> Result<Manifest> {
    let bytes = fs::read(&current.manifest).with_context(|| format!("reading {}", current.manifest.display()))?;
    let sig = fs::read_to_string(&current.sig).with_context(|| format!("reading {}", current.sig.display()))?;
    verify::verify_signed(&bytes, &sig, keys).with_context(|| {
        format!(
            "the current manifest {} does not verify, so nothing is built on it (docs/releasing.md, \"Repair\")",
            current.manifest.display()
        )
    })
}

fn write_signed(m: Manifest, keys: &[PublicKey], out_dir: &Path, now: OffsetDateTime) -> Result<()> {
    let key_text = std::env::var(KEY_ENV).with_context(|| format!("{KEY_ENV} is not set"))?;
    let password = std::env::var(PASSWORD_ENV).with_context(|| format!("{PASSWORD_ENV} is not set"))?;
    let sk = sign::open_secret_key(&key_text, &password)?;
    let bytes = m.to_bytes();
    let sig = sign::sign(&sk, &bytes, &format!("arcane releases.json serial:{}", m.serial))?;
    verify::verify_manifest(&bytes, &sig, keys, Some(m.serial), now)
        .context("the manifest just signed does not verify against the trusted keys (is ARCANE_MINISIGN_KEY one of them?); nothing was written")?;
    let json = out_dir.join("releases.json");
    ensure!(!json.exists(), "{} already exists; give an empty --out-dir", json.display());
    fs::create_dir_all(out_dir)?;
    fs::write(&json, &bytes)?;
    fs::write(out_dir.join("releases.json.minisig"), sig)?;
    println!("signed serial {} into {}", m.serial, out_dir.display());
    Ok(())
}

fn keygen(out_dir: &Path, name: &str) -> Result<()> {
    let password = std::env::var(PASSWORD_ENV).with_context(|| format!("set {PASSWORD_ENV} to the key's password"))?;
    let (pub_path, key_path) = (out_dir.join(format!("{name}.pub")), out_dir.join(format!("{name}.key")));
    ensure!(!pub_path.exists() && !key_path.exists(), "{name}.pub or {name}.key already exists in {}", out_dir.display());
    let generated = sign::generate(name, &password)?;
    fs::create_dir_all(out_dir)?;
    fs::write(&pub_path, &generated.public_key_file)?;
    fs::write(&key_path, &generated.secret_key_file)?;
    println!("{}", generated.public_key_base64);
    Ok(())
}
