// The launch lifecycle: everything between "open this project" and "the
// editor exited". Carved out of lib.rs 2026-07-29, when the "thin IPC skin"
// had quietly accreted the most decision-rich code in the app -- the launch
// core and its wait thread, the boot watchdog, close-mode's delayed exit,
// the shell router, the running-set merge -- while its header still promised
// it held none. One story, one file:
//
//   do_open_project   the single launch brain, whatever door the launch came
//                     through (IPC command or a double-clicked .arcproj)
//   shell_open        the file-association door: engine choice + outcome
//                     surfacing for a launch nobody clicked a card for
//   running_keys      "which projects have a live editor", map + locks
//
// The RunningEditors map is managed Tauri state owned by run(); this module
// reads it through the AppHandle.

use std::collections::HashMap;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;

use tauri::Manager;

use crate::{editorlock, engine, project, resolve, settings, spawn, state, tray};

/// Live editors, keyed by normalised PROJECT path -> pid. One editor per
/// project: the dangerous case is the same project open twice (two editors
/// saving one project's files clobber each other); two different projects on
/// one engine build stay legal. The pid, not the Child, lives here -- the
/// wait thread owns the Child for its whole lifetime.
pub struct RunningEditors(pub Mutex<HashMap<String, u32>>);

impl RunningEditors {
    pub fn new() -> Self {
        Self(Mutex::new(HashMap::new()))
    }
}

impl Default for RunningEditors {
    fn default() -> Self {
        Self::new()
    }
}

/// What open_project actually did. A launch refused because a recorded path
/// vanished is an OUTCOME, not an error: the frontend answers it by refreshing
/// the list -- the row flips to its missing treatment, which already carries
/// the explanation -- instead of raising the error banner over a page that is
/// about to change under it. (Caught live 2026-07-29: a stale "project not
/// found" banner sat over a row still offering Launch, and clicking it read
/// as a jarring page reload.) Probe failures stay hard errors: they are facts
/// about a binary that IS there but cannot answer for itself.
#[derive(Debug, Clone, Copy, PartialEq, serde::Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum OpenOutcome {
    Launched,
    Focused,
    ProjectMissing,
    EngineMissing,
}

/// How long a spawned editor gets to prove it can boot. The wait thread
/// reports any exit inside this window as a refusal (the project gate exits
/// before a window exists); close-mode's delayed exit waits this out plus a
/// margin so the report always has a Hub left to show it.
const BOOT_WATCHDOG: std::time::Duration = std::time::Duration::from_secs(2);

pub fn now_utc_iso() -> String {
    // Seconds since the epoch is enough to sort "last opened" and avoids
    // pulling a date crate in for a display string.
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    secs.to_string()
}

/// Which projects have a live editor: the in-memory map (editors THIS Hub
/// spawned, including mid-boot ones with no lock yet) merged with each
/// listed project's editor lock (editors that outlived a Hub restart, or
/// were launched by hand). Sorted so set-comparison and the emitted payload
/// are deterministic. Keys are `state::normalise_path` of the recorded
/// project path -- format.ts `normalisePath` mirrors that fold exactly.
pub fn running_keys(app: &tauri::AppHandle) -> Vec<String> {
    let mut keys: std::collections::HashSet<String> = app
        .state::<RunningEditors>()
        .0
        .lock()
        .unwrap()
        .keys()
        .cloned()
        .collect();
    for e in state::load().recents {
        if editorlock::read_live(&resolve::project_dir(Path::new(&e.path))).is_some() {
            keys.insert(state::normalise_path(&e.path));
        }
    }
    let mut keys: Vec<String> = keys.into_iter().collect();
    keys.sort();
    keys
}

/// Tell the frontend which projects have a live editor right now. Emitted on
/// every spawn and every exit (the disk watcher covers lock transitions the
/// map cannot see); the payload is the full key set rather than a delta, so
/// a missed event costs one stale badge until the next, not a permanently
/// wrong count.
pub fn emit_running(app: &tauri::AppHandle) {
    use tauri::Emitter;
    let _ = app.emit("running-changed", running_keys(app));
}

// The launch core, callable from BOTH entry points: the open_project command
// and shell_open (a double-clicked .arcproj). One brain -- the probe, the
// one-editor-per-project focus, the args, the wait thread -- whatever door
// the launch came through.
pub fn do_open_project(
    app: &tauri::AppHandle,
    project_path: String,
    engine_path: String,
) -> Result<OpenOutcome, String> {
    let proj = PathBuf::from(&project_path);
    let exe = engine::resolve_editor_exe(Path::new(&engine_path));
    if !proj.exists() {
        return Ok(OpenOutcome::ProjectMissing);
    }
    if !exe.exists() {
        return Ok(OpenOutcome::EngineMissing);
    }

    // One editor per project, checked BEFORE the probe: focusing a running
    // editor must not depend on the engine binary being able to answer for
    // itself (it may be mid-rebuild while its editor runs on). Two sources
    // of truth, in order:
    //  1. The in-memory map -- editors THIS Hub spawned, including one still
    //     mid-boot whose lock is not on disk yet. A pid whose window cannot
    //     be found is a stale entry (the wait thread races this) and falls
    //     through.
    //  2. The editor's own lock in the project (pid + start-time validated,
    //     editorlock.rs) -- which survives Hub restarts, so close-mode still
    //     guards. A live lock refuses the rival spawn even when its window
    //     cannot be focused yet: an editor mid-boot has no window, and
    //     spawning a second editor against it is exactly the
    //     two-editors-one-project case this exists to prevent.
    let project_key = state::normalise_path(&project_path);
    {
        let editors = app.state::<RunningEditors>();
        let mut live = editors.0.lock().unwrap();
        if let Some(&pid) = live.get(&project_key) {
            if spawn::focus_process_window(pid) {
                return Ok(OpenOutcome::Focused);
            }
            live.remove(&project_key);
        }
    }
    if let Some(pid) = editorlock::read_live(&resolve::project_dir(&proj)) {
        let _ = spawn::focus_process_window(pid);
        return Ok(OpenOutcome::Focused);
    }

    // Probe RIGHT BEFORE spawning, not only at registration or Hub launch: a
    // long-lived Hub spans in-place rebuilds, and this is the last moment the
    // compatibility story can be made true. A probe failure refuses the launch
    // with a reason -- an engine that cannot answer its own identity probe
    // (mid-rebuild, missing DLLs) is not about to open a project, and spawning
    // it anyway would produce a silent nothing.
    let info = spawn::probe_engine(&exe)?;

    // Loaded before the spawn so this project's saved extra arguments can go on
    // the command line. Passed as an argv ARRAY, never a joined string: each
    // token reaches the child exactly as typed, with no shell in between.
    let mut s = state::load();
    let key = state::normalise_path(&project_path);
    let extra: Vec<String> = s
        .recents
        .iter()
        .find(|e| state::normalise_path(&e.path) == key)
        .map(|e| project::split_args(&e.args))
        .unwrap_or_default();

    // Adopt what the probe just said: the entry's cached abi/build refresh so
    // the UI's next load speaks about the binary that actually launched.
    let exe_key = state::normalise_path(&exe.to_string_lossy());
    if let Some(e) = s.engines.iter_mut().find(|e| e.id == exe_key) {
        e.engine_abi = info.engine_abi;
        e.build = info.build.clone();
    }

    let mut child = Command::new(&exe)
        .arg("--project")
        .arg(&proj)
        // AFTER --project, so a later flag wins if the editor takes the last
        // occurrence -- the user's own switches should override, not be
        // overridden by, the one the Hub always passes.
        .args(&extra)
        // The editor resolves shaders and plugin DLLs relative to its own
        // directory, so it must start there (same reason scripts/launch.ps1
        // sets the working directory).
        .current_dir(exe.parent().unwrap_or(Path::new(".")))
        .creation_flags(spawn::CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("could not launch the editor: {e}"))?;

    app.state::<RunningEditors>().0.lock().unwrap().insert(project_key.clone(), child.id());
    emit_running(app);

    // Hand the screen to the editor, per the configured behaviour. Checked
    // AFTER a successful spawn so a refused launch never hides the window the
    // error banner lives in. `stay` does nothing by definition.
    match settings::load().launch_behavior.as_str() {
        settings::BEHAVIOR_TRAY => {
            let _ = tray::park(app);
        }
        settings::BEHAVIOR_CLOSE => {
            // Really close -- but not this instant: hide now (the window is
            // gone from the user's view immediately), then exit only after
            // the boot watchdog has had its window. If the editor dies
            // inside it, the wait thread removes the pid and shows the Hub
            // with the reason, and the check below finds the key gone and
            // spares the process. Editors always outlive the Hub; the launch
            // guard still holds afterwards through the editor's own lock
            // (editorlock.rs), which is how close-mode stays safe at all.
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.hide();
            }
            let app = app.clone();
            let key = project_key.clone();
            std::thread::spawn(move || {
                std::thread::sleep(BOOT_WATCHDOG + std::time::Duration::from_millis(500));
                let alive = app
                    .state::<RunningEditors>()
                    .0
                    .lock()
                    .unwrap()
                    .contains_key(&key);
                if alive {
                    app.exit(0);
                }
            });
        }
        _ => {}
    }

    // The wait thread owns the child for its WHOLE lifetime (this is what
    // makes the live-editors map truthful). Three jobs on exit: drop the map
    // entry; surface a boot-refusal (an exit inside 2s -- the project gate
    // exits before any window, and a detached launch would otherwise be
    // click-then-nothing); and when the LAST editor is gone, restore the Hub
    // (re-read the setting at exit time, so toggling it mid-run behaves).
    // stderr is deliberately NOT piped: a pipe nobody drains would eventually
    // BLOCK the editor mid-log -- the exit code has to carry the story.
    {
        let app = app.clone();
        let shown = project::display_name(&project_path);
        let key = project_key.clone();
        std::thread::spawn(move || {
            let started = std::time::Instant::now();
            let status = child.wait().ok();

            use tauri::Emitter;
            let none_left = {
                let editors = app.state::<RunningEditors>();
                let mut live = editors.0.lock().unwrap();
                live.remove(&key);
                live.is_empty()
            };
            emit_running(&app);

            if started.elapsed() < BOOT_WATCHDOG {
                let why = match status.and_then(|s| s.code()) {
                    // Exit 2 is the editor's own project gate (ArcaneEditor
                    // main.cpp): wrong abi, unreadable manifest, refused boot.
                    Some(2) => "the editor refused the project (engine/abi gate)".to_string(),
                    Some(c) => format!("the editor exited immediately with code {c}"),
                    None => "the editor was killed before it opened".to_string(),
                };
                let _ = app.emit("launch-failed", format!("{shown}: {why}."));
            }

            // A hidden window ALWAYS restores when the last editor exits,
            // whatever hid it. Tray hands the Hub back by design (user call
            // 2026-07-29: the icon is for DURING the run); close-mode only
            // reaches here when the delayed exit was spared by a
            // boot-refusal, where showing the Hub with the reason is
            // exactly right. show_hub clears the tray icon along the way.
            // `stay` windows are never hidden, so the guard alone keeps
            // them from stealing focus from whatever the user moved on to.
            if none_left {
                let hidden = app
                    .get_webview_window("main")
                    .map(|w| !w.is_visible().unwrap_or(true))
                    .unwrap_or(false);
                if hidden {
                    tray::show_hub(&app);
                }
                // Regardless of the window: the list's opened-times and
                // missing flags deserve a refresh when an editor ends.
                let _ = app.emit("editors-idle", ());
            }
        });
    }
    // display_name, not file_name(): project_path may be a .arcproj FILE (what
    // the Open dialog now yields) or a folder (what older entries hold), and
    // both must record the same name.
    let name = project::display_name(&project_path);

    // The PROJECT's abi, from its manifest -- NOT the abi of the engine we just
    // launched. Recording the launching engine's abi made the card claim the
    // project was compatible with the very engine that had just refused it
    // (Runtime.cpp:387), so an incompatible project could never look
    // incompatible. 0 = unknown, which isCompatible treats as "no conflict
    // provable" rather than as a fault.
    let abi = resolve::manifest_abi(&proj).unwrap_or(0);
    state::touch_recent(
        &mut s.recents,
        state::RecentProject {
            path: proj.to_string_lossy().to_string(),
            name,
            last_opened_utc: now_utc_iso(),
            engine_abi: abi,
            // None, NOT the engine just used: launching is not the same as
            // choosing. touch_recent carries an existing pin across, so a
            // pinned project stays pinned and an unpinned one keeps following
            // the default instead of silently pinning itself on first launch.
            engine_id: None,
            // Empty for the same reason: touch_recent carries the saved
            // arguments across, so launching never wipes them.
            args: String::new(),
            // False for the same reason: touch_recent ORs the previous star
            // in, so launching never unstars.
            favorite: false,
            // The project was just found on disk to launch; load() re-stamps
            // this on every read regardless.
            missing: false,
        },
    );
    state::save(&s)?;
    Ok(OpenOutcome::Launched)
}

/// The shell route's engine choice, mirroring what a fresh Hub session's
/// card click would use: the project's pin when it is listed and the pin
/// resolves, else the first registered engine (the sidebar's default
/// selection is SESSION state, not persisted, so the first engine is exactly
/// what a new session's launch would pick). None = nothing registered.
/// Pure, and extracted precisely so this policy is TESTED -- it used to live
/// inline in untestable skin.
pub fn shell_engine<'a>(s: &'a state::HubState, path: &str) -> Option<&'a state::EngineEntry> {
    let key = state::project_dir_key(path);
    s.recents
        .iter()
        .find(|e| state::project_dir_key(&e.path) == key)
        .and_then(|e| e.engine_id.as_ref())
        .and_then(|id| s.engines.iter().find(|en| &en.id == id))
        .or_else(|| s.engines.first())
}

// Route a shell-opened .arcproj (double-click, Open With, drag onto the exe)
// through the SAME launch core a card click takes -- probe, focus-existing,
// auto-listing via touch_recent, the lifecycle. No engines at all -> surface
// the window and say so; a launcher with nothing to launch with must not eat
// a double-click silently.
fn shell_open(app: &tauri::AppHandle, path: &str) {
    use tauri::Emitter;
    let s = state::load();
    let Some(engine) = shell_engine(&s, path) else {
        tray::show_hub(app);
        let _ = app.emit(
            "launch-failed",
            format!(
                "{}: register an engine in the Hub before opening projects.",
                project::display_name(path)
            ),
        );
        return;
    };
    let (engine_path, engine_shown) = (engine.path.clone(), engine.build.clone());

    match do_open_project(app, path.to_string(), engine_path.clone()) {
        Ok(OpenOutcome::Launched) | Ok(OpenOutcome::Focused) => {
            // The list may have just gained this project. The disk watcher
            // would notice within a tick; telling the (possibly hidden but
            // alive) frontend now keeps the list honest the moment the
            // window comes back.
            let _ = app.emit("state-changed", &state::load());
            // On a shell route the window starts hidden, and only `stay`
            // earns a show: tray/close launches go straight to the editor
            // with no Hub flash (the editor's own window takes the
            // foreground when it appears, moments after this).
            if settings::load().launch_behavior == settings::BEHAVIOR_STAY {
                tray::show_hub(app);
            }
        }
        Ok(OpenOutcome::ProjectMissing) => {
            tray::show_hub(app);
            let _ = app.emit(
                "launch-failed",
                format!("{path}: the file vanished before it could open."),
            );
        }
        Ok(OpenOutcome::EngineMissing) => {
            tray::show_hub(app);
            let _ = app.emit(
                "launch-failed",
                format!(
                    "{}: {engine_shown} is no longer at {engine_path}. See Engines.",
                    project::display_name(path)
                ),
            );
        }
        Err(e) => {
            tray::show_hub(app);
            let _ = app.emit("launch-failed", e);
        }
    }
}

/// Spawn shell_open on its own thread for an argv that carries a .arcproj
/// (the probe may block up to its 10s timeout, and neither setup() nor the
/// single-instance callback may stall that long). Returns whether it did, so
/// the single-instance callback can tell a plain "show me the Hub" relaunch
/// from one that was really a file open.
pub fn route_shell_args<'a>(
    app: &tauri::AppHandle,
    args: impl IntoIterator<Item = &'a str>,
) -> bool {
    let Some(p) = project::arcproj_in_args(args) else {
        return false;
    };
    let app = app.clone();
    let path = p.to_string();
    std::thread::spawn(move || shell_open(&app, &path));
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hub(recents: Vec<state::RecentProject>, engines: Vec<state::EngineEntry>) -> state::HubState {
        state::HubState { recents, engines, warnings: Vec::new() }
    }

    fn rp(path: &str, pin: Option<&str>) -> state::RecentProject {
        state::RecentProject {
            path: path.to_string(),
            name: "N".to_string(),
            last_opened_utc: "1".to_string(),
            engine_abi: 8,
            engine_id: pin.map(|s| s.to_string()),
            args: String::new(),
            favorite: false,
            missing: false,
        }
    }

    fn ee(path: &str) -> state::EngineEntry {
        state::EngineEntry::new(path, 8, "Arcane".to_string())
    }

    #[test]
    fn shell_engine_prefers_a_live_pin() {
        let a = ee("C:/eng-a/ArcaneEditor.exe");
        let b = ee("C:/eng-b/ArcaneEditor.exe");
        let pin = b.id.clone();
        let s = hub(vec![rp("D:/G/My/My.arcproj", Some(&pin))], vec![a, b]);
        assert_eq!(shell_engine(&s, "D:/G/My/My.arcproj").unwrap().id, pin);
        // The folder-vs-manifest fold: a double-clicked FILE must find the
        // entry however it was recorded.
        assert_eq!(shell_engine(&s, "D:/G/My").unwrap().id, pin);
    }

    #[test]
    fn shell_engine_falls_back_to_the_first_engine() {
        let a = ee("C:/eng-a/ArcaneEditor.exe");
        let first = a.id.clone();
        // Unlisted project -> first engine (a fresh session's default).
        let s = hub(vec![], vec![a, ee("C:/eng-b/ArcaneEditor.exe")]);
        assert_eq!(shell_engine(&s, "D:/G/New/New.arcproj").unwrap().id, first);
        // A DANGLING pin (engine no longer registered) falls back the same
        // way rather than refusing the launch.
        let s = hub(vec![rp("D:/G/My/My.arcproj", Some("gone"))],
                    vec![ee("C:/eng-a/ArcaneEditor.exe")]);
        assert_eq!(shell_engine(&s, "D:/G/My/My.arcproj").unwrap().id, first);
    }

    #[test]
    fn shell_engine_with_nothing_registered_is_none() {
        let s = hub(vec![rp("D:/G/My/My.arcproj", None)], vec![]);
        assert!(shell_engine(&s, "D:/G/My/My.arcproj").is_none());
    }
}
