// The process-spawning half of the Hub: running ArcaneEditor.exe and reading
// its identity probe. Split out of lib.rs when the command surface grew past
// ~550 lines -- lib.rs keeps ONLY the #[tauri::command] IPC surface, and
// everything that spawns or waits lives here (resolve.rs holds the
// path-resolution half). The PARSING half stays pure and tested in engine.rs;
// here, the one branch a healthy engine never exercises -- the timeout that
// kills a hung probe -- is extracted as wait_with_deadline and tested against
// a deliberately hung child, because an untested kill path is exactly the
// kind that quietly breaks.

use std::os::windows::process::CommandExt;
use std::path::Path;
use std::process::{Command, Stdio};

use crate::engine;

// ArcaneEditor is a ConsoleApp, so without this every spawn flashes a console.
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;

/// The taskbar family id. MIRRORED constant: ArcaneEditor's main.cpp claims
/// the same id (`kAppUserModelId`) unconditionally at boot -- change BOTH or
/// the Hub's and the editors' taskbar buttons stop stacking. Windows groups
/// by AppUserModelID, not by who spawned whom.
pub const APP_USER_MODEL_ID: &str = "dev.starworks.arcanehub";

/// Claim the family AUMID for THIS process. Must run before the first window
/// exists, so lib.rs calls it at the top of run().
pub fn claim_app_user_model_id() {
    use windows_sys::Win32::UI::Shell::SetCurrentProcessExplicitAppUserModelID;
    let wide: Vec<u16> = APP_USER_MODEL_ID
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect();
    unsafe {
        let _ = SetCurrentProcessExplicitAppUserModelID(wide.as_ptr());
    }
}

/// Bring the first visible top-level window of `pid` to the foreground.
/// False when the process has no such window (usually: it already exited) --
/// the caller treats that as a stale entry, not an error.
pub fn focus_process_window(pid: u32) -> bool {
    use windows_sys::core::BOOL;
    use windows_sys::Win32::Foundation::{HWND, LPARAM};
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GetWindowThreadProcessId, IsWindowVisible, SetForegroundWindow,
        ShowWindow, SW_RESTORE,
    };

    struct Target {
        pid: u32,
        hwnd: HWND,
    }
    unsafe extern "system" fn find(hwnd: HWND, lparam: LPARAM) -> BOOL {
        let t = unsafe { &mut *(lparam as *mut Target) };
        let mut owner = 0u32;
        unsafe { GetWindowThreadProcessId(hwnd, &mut owner) };
        if owner == t.pid && unsafe { IsWindowVisible(hwnd) } != 0 {
            t.hwnd = hwnd;
            return 0; // found -- stop enumerating
        }
        1
    }

    let mut t = Target { pid, hwnd: std::ptr::null_mut() };
    unsafe {
        let _ = EnumWindows(Some(find), &mut t as *mut Target as LPARAM);
    }
    if t.hwnd.is_null() {
        return false;
    }
    unsafe {
        // Restore first: SetForegroundWindow on a minimized window succeeds
        // without actually surfacing it.
        let _ = ShowWindow(t.hwnd, SW_RESTORE);
        SetForegroundWindow(t.hwnd) != 0
    }
}

// How long the identity probe may take before we give up and kill it.
//
// The probe answers before booting a window, a device, or a registry
// (ArcaneEditor main.cpp:27-34), so a healthy engine replies in milliseconds
// even from a cold disk. The timeout exists because the Hub runs a binary the
// USER chose: any exe that happens to be named ArcaneEditor.exe gets executed,
// and a plain `output()` would block this command thread forever while the UI
// sat latched in its busy state with no way to cancel.
const PROBE_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(10);
const PROBE_POLL: std::time::Duration = std::time::Duration::from_millis(25);

/// Wait for `child` to exit within `timeout`, polling at `poll`. On expiry the
/// child is KILLED and reaped (no zombie left behind): Ok(true) = it exited on
/// its own, Ok(false) = it was killed at the deadline.
///
/// Extracted from probe_engine so this branch is testable with a deliberately
/// hung child -- a healthy engine answers in milliseconds and never takes it.
fn wait_with_deadline(
    child: &mut std::process::Child,
    timeout: std::time::Duration,
    poll: std::time::Duration,
) -> Result<bool, std::io::Error> {
    let deadline = std::time::Instant::now() + timeout;
    loop {
        match child.try_wait() {
            Ok(Some(_)) => return Ok(true),
            Ok(None) => {
                if std::time::Instant::now() >= deadline {
                    let _ = child.kill();
                    let _ = child.wait(); // reap, so we leave no zombie
                    return Ok(false);
                }
                std::thread::sleep(poll);
            }
            Err(e) => return Err(e),
        }
    }
}

// Run the engine's identity probe. Failure modes the UI must be able to show
// verbatim: not an Arcane engine, exe missing, probe printed nothing, hung.
pub fn probe_engine(exe: &Path) -> Result<engine::EngineInfo, String> {
    if !exe.exists() {
        return Err(format!("no {} at {}", engine::EDITOR_EXE, exe.display()));
    }

    let mut child = Command::new(exe)
        .arg("--print-engine-info")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("could not run {}: {e}", exe.display()))?;

    match wait_with_deadline(&mut child, PROBE_TIMEOUT, PROBE_POLL) {
        Ok(true) => {}
        Ok(false) => {
            return Err(format!(
                "{} did not answer --print-engine-info within {}s. \
                 Is it really an Arcane engine?",
                exe.display(),
                PROBE_TIMEOUT.as_secs()
            ));
        }
        Err(e) => return Err(format!("could not wait for {}: {e}", exe.display())),
    }

    // Safe after try_wait reported exit: the status is cached, and the probe
    // writes one short line, so nothing can be blocked on a full pipe.
    let out = child
        .wait_with_output()
        .map_err(|e| format!("could not read from {}: {e}", exe.display()))?;

    if !out.status.success() {
        return Err(format!(
            "{} exited with {} -- not an Arcane engine?",
            exe.display(),
            out.status
        ));
    }
    engine::parse_probe_output(&String::from_utf8_lossy(&out.stdout))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    // cmd.exe is spawnable directly (a real exe, unlike a .bat), and `ping -n`
    // is the standard Windows sleep that needs no extra tooling.
    fn spawn_cmd(args: &str) -> std::process::Child {
        Command::new("cmd")
            .args(["/C", args])
            .creation_flags(CREATE_NO_WINDOW)
            .spawn()
            .expect("cmd.exe must spawn")
    }

    #[test]
    fn a_hung_child_is_killed_at_the_deadline_and_reaped() {
        // ~30s of ping stands in for a probe that will never answer.
        let mut child = spawn_cmd("ping -n 30 127.0.0.1 >nul");
        let started = std::time::Instant::now();
        let exited = wait_with_deadline(
            &mut child,
            Duration::from_millis(200),
            Duration::from_millis(25),
        )
        .expect("waiting must not error");
        assert!(!exited, "a hung child must report the timeout, not an exit");
        assert!(
            started.elapsed() < Duration::from_secs(5),
            "the kill must happen at the deadline, not after the child tires"
        );
        // Reaped: the status is cached, so asking again answers immediately.
        assert!(child.try_wait().expect("reaped child").is_some());
    }

    #[test]
    fn a_child_that_exits_in_time_is_reported_as_exited() {
        let mut child = spawn_cmd("exit 0");
        let exited = wait_with_deadline(
            &mut child,
            Duration::from_secs(10),
            Duration::from_millis(10),
        )
        .expect("waiting must not error");
        assert!(exited, "a prompt exit must not read as a timeout");
    }
}
