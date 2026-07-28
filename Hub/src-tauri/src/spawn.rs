// The process-spawning half of the Hub: running ArcaneEditor.exe and reading
// its identity probe. Split out of lib.rs when the command surface grew past
// ~550 lines -- lib.rs keeps ONLY the #[tauri::command] IPC surface, and
// everything that spawns or waits lives here (resolve.rs holds the
// path-resolution half). Not unit-tested, same rule as the rest of the IO
// skin; the PARSING half stays pure and tested in engine.rs.

use std::os::windows::process::CommandExt;
use std::path::Path;
use std::process::{Command, Stdio};

use crate::engine;

// ArcaneEditor is a ConsoleApp, so without this every spawn flashes a console.
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;

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

    let deadline = std::time::Instant::now() + PROBE_TIMEOUT;
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) => {
                if std::time::Instant::now() >= deadline {
                    let _ = child.kill();
                    let _ = child.wait(); // reap, so we leave no zombie
                    return Err(format!(
                        "{} did not answer --print-engine-info within {}s. \
                         Is it really an Arcane engine?",
                        exe.display(),
                        PROBE_TIMEOUT.as_secs()
                    ));
                }
                std::thread::sleep(PROBE_POLL);
            }
            Err(e) => return Err(format!("could not wait for {}: {e}", exe.display())),
        }
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
