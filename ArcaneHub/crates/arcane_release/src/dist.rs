//! The dist folder the publish step reads: one folder per OS, each holding
//! the engine and symbols archives plus engine.json (spec s4, step 6).
//! Anything else in it is refused, not ignored.

use std::fs;
use std::path::Path;

use anyhow::{bail, ensure, Context, Result};

use crate::digest::digest_file;
use crate::manifest::{Artifact, EngineInfo};
use crate::version;

pub const ARCH: &str = "x86_64";
pub const OSES: [&str; 2] = ["windows", "linux"];

pub fn extension(os: &str) -> &'static str {
    if os == "windows" {
        "zip"
    } else {
        "tar.gz"
    }
}

pub fn archive_name(version: &str, os: &str, symbols: bool) -> String {
    format!("arcane-{version}-{os}-{ARCH}{}.{}", if symbols { "-symbols" } else { "" }, extension(os))
}

pub fn read_dist(dist: &Path, version: &str) -> Result<(EngineInfo, Vec<Artifact>)> {
    let channel = version::channel_of(&version::parse(version)?)?;
    let mut info: Option<EngineInfo> = None;
    let mut artifacts = Vec::new();
    for os in OSES {
        let dir = dist.join(os);
        ensure!(dir.is_dir(), "{} is missing: a release ships Windows and Linux", dir.display());
        let engine_json = dir.join("engine.json");
        let bytes = fs::read(&engine_json).with_context(|| format!("reading {}", engine_json.display()))?;
        let this: EngineInfo = serde_json::from_slice(&bytes).with_context(|| format!("parsing {}", engine_json.display()))?;
        ensure!(this.version == version, "{} says {}, not {version}", engine_json.display(), this.version);
        ensure!(this.channel == channel, "{} says {:?}, but {version} is {:?}", engine_json.display(), this.channel, channel);
        if info.is_none() {
            info = Some(this.clone());
        }
        ensure!(info.as_ref() == Some(&this), "engine.json differs between OSes: {:?} vs {this:?}", info.as_ref().unwrap());
        let expected = [archive_name(version, os, false), archive_name(version, os, true)];
        for entry in fs::read_dir(&dir)? {
            let name = entry?.file_name().to_string_lossy().into_owned();
            if name != "engine.json" && !expected.contains(&name) {
                bail!("unexpected file {name} in {}", dir.display());
            }
        }
        for (name, kind) in expected.iter().zip(["engine", "symbols"]) {
            let path = dir.join(name);
            ensure!(path.is_file(), "{} is missing", path.display());
            let d = digest_file(&path)?;
            artifacts.push(Artifact {
                os: os.to_string(),
                arch: ARCH.to_string(),
                kind: kind.to_string(),
                path: format!("engine/{version}/{name}"),
                size: d.size,
                sha256: d.sha256,
            });
        }
    }
    Ok((info.expect("OSES is not empty"), artifacts))
}
