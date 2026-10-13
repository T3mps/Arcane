//! Engine and symbols archives (spec s7.1): .zip on Windows, .tar.gz on
//! Linux, every file under one root folder, manifest.txt written into the
//! stage first. Deterministic: sorted entries, fixed timestamps and owners, so
//! the same stage always packs to the same bytes and SHA-256.

use std::fs::{self, File};
use std::io;
use std::path::Path;

use anyhow::{bail, Context, Result};

use crate::digest::{digest_file, list_files, tree_listing, FileDigest, MANIFEST_TXT};

pub fn pack(stage: &Path, root_name: &str, out: &Path) -> Result<FileDigest> {
    if root_name.is_empty() || root_name.contains(['/', '\\']) || root_name.starts_with('.') {
        bail!("the archive root must be one folder name, not {root_name:?}");
    }
    let name = out.file_name().and_then(|n| n.to_str()).unwrap_or_default();
    let is_zip = name.ends_with(".zip");
    if !is_zip && !name.ends_with(".tar.gz") {
        bail!("{} must end in .zip or .tar.gz", out.display());
    }
    fs::write(stage.join(MANIFEST_TXT), tree_listing(stage)?)
        .with_context(|| format!("writing {}", stage.join(MANIFEST_TXT).display()))?;
    let mut files = list_files(stage)?;
    files.push(MANIFEST_TXT.to_string());
    files.sort();
    if let Some(dir) = out.parent() {
        fs::create_dir_all(dir)?;
    }
    if is_zip {
        write_zip(stage, root_name, &files, out)?;
    } else {
        write_tar_gz(stage, root_name, &files, out)?;
    }
    Ok(digest_file(out)?)
}

fn write_zip(stage: &Path, root: &str, files: &[String], out: &Path) -> Result<()> {
    let mut zip = zip::ZipWriter::new(File::create(out)?);
    for rel in files {
        let path = stage.join(rel);
        let options = zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .last_modified_time(zip::DateTime::default())
            .unix_permissions(mode_of(&fs::metadata(&path)?));
        zip.start_file(format!("{root}/{rel}"), options)?;
        io::copy(&mut File::open(&path)?, &mut zip)?;
    }
    zip.finish()?;
    Ok(())
}

fn write_tar_gz(stage: &Path, root: &str, files: &[String], out: &Path) -> Result<()> {
    let gz = flate2::GzBuilder::new().mtime(0).write(File::create(out)?, flate2::Compression::best());
    let mut tar = tar::Builder::new(gz);
    for rel in files {
        let path = stage.join(rel);
        let meta = fs::metadata(&path)?;
        let mut header = tar::Header::new_gnu();
        header.set_size(meta.len());
        header.set_mode(mode_of(&meta));
        header.set_mtime(0);
        header.set_uid(0);
        header.set_gid(0);
        tar.append_data(&mut header, format!("{root}/{rel}"), File::open(&path)?)?;
    }
    tar.into_inner()?.finish()?;
    Ok(())
}

#[cfg(unix)]
fn mode_of(meta: &fs::Metadata) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    if meta.permissions().mode() & 0o111 != 0 {
        0o755
    } else {
        0o644
    }
}

#[cfg(not(unix))]
fn mode_of(_meta: &fs::Metadata) -> u32 {
    0o644
}
