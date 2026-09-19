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
//
// THREE tells, matching C++ EditorLock::ReadLive (Project.cpp). Tells 1+2
// (pid opens, birth matches) are not enough: a process OBJECT outlives the
// process while any handle remains, and the Hub's wait thread holds exactly
// that handle (`Child`). Exit time is the tell that cannot be faked by a
// lingering handle -- nonzero means it exited. sweep_stale then deletes a
// file read_live has proven dead, so Saved/ does not keep a crash's lock
// forever (the Hub never owned Clear; only the editor did).

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

pub fn lock_path(project_root: &Path) -> std::path::PathBuf {
    project_root.join("Saved").join("editor.lock")
}

struct ProcessTimes {
    start: u64,
    /// Nonzero FILETIME means the process has exited; the object may still
    /// be queryable because a handle (the Hub's Child) is keeping it around.
    exited: bool,
}

/// Times of a live-or-zombie process object, or None when it does not exist
/// (or cannot be asked, which for this purpose is the same answer).
fn process_times(pid: u32) -> Option<ProcessTimes> {
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
        Some(ProcessTimes {
            start: ((created.dwHighDateTime as u64) << 32) | created.dwLowDateTime as u64,
            exited: exited.dwHighDateTime != 0 || exited.dwLowDateTime != 0,
        })
    }
}

/// Some(pid) only when the project's lock names a process that is still the
/// process it described. Every other shape -- no file, unparseable, dead
/// pid, recycled pid with a different birth, exited-but-handle-held --
/// reads as "not running".
pub fn read_live(project_root: &Path) -> Option<u32> {
    let text = std::fs::read_to_string(lock_path(project_root)).ok()?;
    let lock = parse(&text)?;
    let times = process_times(lock.pid)?;
    if lock.start != 0 && lock.start != times.start {
        return None;
    }
    if times.exited {
        return None;
    }
    Some(lock.pid)
}

/// Delete `editor.lock` only when read_live has proven it dead. Never
/// touches a lock a live editor still holds. Returns true when a file was
/// removed. Called on Hub start (close-mode leftover) and after the wait
/// thread's child.wait() (the handle is still held -- tell 3 is why this
/// is safe then).
pub fn sweep_stale(project_root: &Path) -> bool {
    let file = lock_path(project_root);
    if !file.is_file() {
        return false;
    }
    if read_live(project_root).is_some() {
        return false;
    }
    std::fs::remove_file(&file).is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::windows::process::CommandExt;
    use std::path::PathBuf;
    use std::process::Command;

    const CREATE_NO_WINDOW: u32 = 0x0800_0000;

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
            lock_path(root),
            format!(r#"{{"pid":{pid},"start":{start}}}"#),
        )
        .unwrap();
    }

    #[test]
    fn a_lock_naming_this_live_process_reads_as_running() {
        let root = scratch("live");
        let pid = std::process::id();
        let times = process_times(pid).expect("own process must be queryable");
        assert!(!times.exited, "the test process has not exited");
        write_lock(&root, pid, times.start);
        assert_eq!(read_live(&root), Some(pid));
        assert!(!sweep_stale(&root), "must not delete a live editor's lock");
        assert!(lock_path(&root).is_file());
    }

    #[test]
    fn a_stale_lock_with_the_wrong_birth_reads_as_not_running() {
        // THE Unity failure this design exists to avoid: same pid, different
        // creation time = a recycled pid or a crash's leftovers, never a
        // reason to refuse a launch.
        let root = scratch("stale");
        let pid = std::process::id();
        let times = process_times(pid).unwrap();
        write_lock(&root, pid, times.start ^ 1);
        assert_eq!(read_live(&root), None);
        assert!(sweep_stale(&root), "a birth-mismatch lock is dead; delete it");
        assert!(!lock_path(&root).is_file());
    }

    #[test]
    fn no_lock_file_reads_as_not_running() {
        let root = scratch("none");
        assert_eq!(read_live(&root), None);
        assert!(!sweep_stale(&root));
    }

    #[test]
    fn an_exited_process_is_stale_even_while_a_handle_keeps_the_pid_reserved() {
        // The 2026-09-08 desk-pass zombie, Hub-side. C++ ReadLive grew tell 3
        // (nonzero exit time); this file did not, so a Hub holding `Child`
        // after the editor died kept OpenProcess succeeding with the original
        // birth and the project read as open forever.
        let root = scratch("zombie");
        let mut child = Command::new("cmd.exe")
            .args(["/C", "exit", "0"])
            .creation_flags(CREATE_NO_WINDOW)
            .spawn()
            .expect("cmd.exe /C exit");
        let pid = child.id();
        child.wait().expect("child exits");
        // `child` is still in scope -- that handle is the zombie.
        let times = process_times(pid).expect("handle keeps the process object queryable");
        assert!(times.exited, "it really did exit");
        write_lock(&root, pid, times.start);
        assert_eq!(read_live(&root), None, "tell 3: exited is not live");
        assert!(sweep_stale(&root));
        assert!(!lock_path(&root).is_file());
    }
}
