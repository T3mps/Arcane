//! File digests and `manifest.txt` (spec s7.1; the Hub's Verify, s6). The
//! listing is sha256sum's format, "<sha256>  <path>", one line per file,
//! '/'-separated, sorted, omitting manifest.txt itself, so
//! `sha256sum -c manifest.txt` checks an extracted engine by hand.

use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::Path;

use serde::Serialize;
use sha2::{Digest, Sha256};
use walkdir::WalkDir;

pub const MANIFEST_TXT: &str = "manifest.txt";

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct FileDigest {
    pub size: u64,
    pub sha256: String,
}

pub fn digest_file(path: &Path) -> io::Result<FileDigest> {
    let mut file = File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut size = 0u64;
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        size += n as u64;
    }
    Ok(FileDigest { size, sha256: hex::encode(hasher.finalize()) })
}

/// Every regular file under `root` as a sorted '/'-separated relative path,
/// omitting a root-level manifest.txt. Symlinks are an error: a stage holds
/// real files (phase 1 copies a library under its soname).
pub fn list_files(root: &Path) -> io::Result<Vec<String>> {
    let mut out = Vec::new();
    for entry in WalkDir::new(root).follow_links(false) {
        let entry = entry.map_err(io::Error::other)?;
        let kind = entry.file_type();
        if kind.is_symlink() {
            return Err(io::Error::other(format!("{} is a symlink; a stage holds real files", entry.path().display())));
        }
        if !kind.is_file() {
            continue;
        }
        let rel = entry.path().strip_prefix(root).expect("walkdir stays under its root");
        let rel = rel.components().map(|c| c.as_os_str().to_string_lossy().into_owned()).collect::<Vec<_>>().join("/");
        if rel != MANIFEST_TXT {
            out.push(rel);
        }
    }
    out.sort();
    Ok(out)
}

pub fn tree_listing(root: &Path) -> io::Result<String> {
    let mut listing = String::new();
    for rel in list_files(root)? {
        let d = digest_file(&root.join(&rel))?;
        listing.push_str(&d.sha256);
        listing.push_str("  ");
        listing.push_str(&rel);
        listing.push('\n');
    }
    Ok(listing)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TreeProblem {
    Missing(String),
    Changed(String),
    Unlisted(String),
}

/// Checks `root` against its manifest.txt.
pub fn verify_tree(root: &Path) -> io::Result<Vec<TreeProblem>> {
    let listing = fs::read_to_string(root.join(MANIFEST_TXT))?;
    let mut problems = Vec::new();
    let mut listed = HashSet::new();
    for line in listing.lines().filter(|l| !l.is_empty()) {
        let (sha, rel) = line.split_once("  ").ok_or_else(|| io::Error::other(format!("bad manifest.txt line: {line}")))?;
        if rel.split('/').any(|c| c.is_empty() || c == "." || c == "..") || rel.contains('\\') {
            return Err(io::Error::other(format!("manifest.txt names a path outside its folder: {rel}")));
        }
        listed.insert(rel.to_string());
        let path = root.join(rel);
        if !path.is_file() {
            problems.push(TreeProblem::Missing(rel.to_string()));
        } else if digest_file(&path)?.sha256 != sha {
            problems.push(TreeProblem::Changed(rel.to_string()));
        }
    }
    for rel in list_files(root)? {
        if !listed.contains(&rel) {
            problems.push(TreeProblem::Unlisted(rel));
        }
    }
    Ok(problems)
}
