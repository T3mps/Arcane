// The Hub's half of the editor lock: read <root>/Saved/editor.lock and
// decide whether the editor it names is STILL RUNNING. The editor writes the
// lock while it holds a project (Arcane's Project.cpp, EditorLock namespace
// -- MIRRORED FORMAT, change both) and clears it on clean exit; this is how
// a fresh Hub instance knows which projects are open after close-mode killed
// the in-memory pid map with the process.
//
// Unity's lockfile model minus the flaw its users hate: the lock names
// {pid, process CREATION time}, and read_live only believes it when that
// exact pid with that exact birth is alive. A crash's stale lock fails the
// check and is ignored -- never a false "already open"; a recycled pid has
// a different birth and fails the same way.

use std::path::Path;

/// What a lock file claims. Pure parse so the format contract is testable.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Lock {
    pub pid: u32,
    /// Process creation FILETIME as u64; 0 = "no time recorded" (validation
    /// then degrades to pid-exists, still better than trusting the file).
    pub start: u64,
}

pub fn parse(text: &str) -> Option<Lock> {
    let doc: serde_json::Value = serde_json::from_str(text).ok()?;
    let pid = u32::try_from(doc.get("pid")?.as_u64()?).ok()?;
    if pid == 0 {
        return None;
    }
    let start = doc.get("start").and_then(|s| s.as_u64()).unwrap_or(0);
    Some(Lock { pid, start })
}

/// The creation time of a live process, or None when it does not exist (or
/// cannot be asked, which for this purpose is the same answer).
pub fn process_start_time(pid: u32) -> Option<u64> {
    use windows_sys::Win32::Foundation::{CloseHandle, FILETIME};
    use windows_sys::Win32::System::Threading::{
        GetProcessTimes, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
    };
    unsafe {
        let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() {
            return None;
        }
        let mut created: FILETIME = std::mem::zeroed();
        let mut exited: FILETIME = std::mem::zeroed();
        let mut kernel: FILETIME = std::mem::zeroed();
        let mut user: FILETIME = std::mem::zeroed();
        let ok = GetProcessTimes(h, &mut created, &mut exited, &mut kernel, &mut user);
        CloseHandle(h);
        if ok == 0 {
            return None;
        }
        Some(((created.dwHighDateTime as u64) << 32) | created.dwLowDateTime as u64)
    }
}

/// Some(pid) only when the project's lock names a process that is still the
/// process it described. Every other shape -- no file, unparseable, dead
/// pid, recycled pid with a different birth -- reads as "not running".
pub fn read_live(project_root: &Path) -> Option<u32> {
    let file = project_root.join("Saved").join("editor.lock");
    let text = std::fs::read_to_string(file).ok()?;
    let lock = parse(&text)?;
    let start = process_start_time(lock.pid)?;
    if lock.start != 0 && lock.start != start {
        return None;
    }
    Some(lock.pid)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn parse_mirrors_the_engine_side_format() {
        // The same cases ProjectTest.cpp pins C++-side -- one format, two
        // parsers, and these tests are the tripwire between them.
        let l = parse(r#"{"pid":4242,"start":1311768467463790320}"#).unwrap();
        assert_eq!(l, Lock { pid: 4242, start: 0x1234_5678_9ABC_DEF0 });
        assert!(parse("not json").is_none());
        assert!(parse("{}").is_none());
        assert!(parse(r#"{"pid":0}"#).is_none(), "pid 0 is no process");
        assert!(parse(r#"{"pid":-3}"#).is_none());
        assert_eq!(parse(r#"{"pid":7}"#), Some(Lock { pid: 7, start: 0 }),
                   "missing start degrades to 0, not a refusal");
    }

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-lock-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("Saved")).unwrap();
        dir
    }

    fn write_lock(root: &Path, pid: u32, start: u64) {
        std::fs::write(
            root.join("Saved").join("editor.lock"),
            format!(r#"{{"pid":{pid},"start":{start}}}"#),
        )
        .unwrap();
    }

    #[test]
    fn a_lock_naming_this_live_process_reads_as_running() {
        let root = scratch("live");
        let pid = std::process::id();
        let start = process_start_time(pid).expect("own process must be queryable");
        write_lock(&root, pid, start);
        assert_eq!(read_live(&root), Some(pid));
    }

    #[test]
    fn a_stale_lock_with_the_wrong_birth_reads_as_not_running() {
        // THE Unity failure this design exists to avoid: same pid, different
        // creation time = a recycled pid or a crash's leftovers, never a
        // reason to refuse a launch.
        let root = scratch("stale");
        let pid = std::process::id();
        let start = process_start_time(pid).unwrap();
        write_lock(&root, pid, start ^ 1);
        assert_eq!(read_live(&root), None);
    }

    #[test]
    fn no_lock_file_reads_as_not_running() {
        let root = scratch("none");
        assert_eq!(read_live(&root), None);
    }
}
