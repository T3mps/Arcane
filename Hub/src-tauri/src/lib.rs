// Arcane Hub -- Tauri entry point.
//
// Split, mirroring Tools/setup-wizard: all DECISION logic lives in pure modules
// that `cargo test` covers; the IO skin is spawn.rs (process spawning) and
// resolve.rs (recorded-path resolution), and THIS file is only the
// #[tauri::command] IPC surface plus the state read-modify-write each command
// performs. None of the skin is unit-tested; the pure halves it defers to are.
//
// The Hub is INSTALLED per-user (%LOCALAPPDATA%\Programs\Arcane Hub\), so it
// must never derive a repo root from current_exe() and must never assume an
// engine sits beside it. That is setup-wizard's repo-root-portable assumption
// and it is wrong here. Engines are REGISTERED BY PATH and validated with the
// engine's own --print-engine-info probe.

pub mod engine;
pub mod paths;
pub mod project;
pub mod resolve;
pub mod settings;
pub mod spawn;
pub mod state;
pub mod store;

use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

fn now_utc_iso() -> String {
    // Seconds since the epoch is enough to sort "last opened" and avoids
    // pulling a date crate in for a display string.
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    secs.to_string()
}

#[tauri::command]
fn load_state() -> state::HubState {
    state::load()
}

#[tauri::command]
fn register_engine(path: String) -> Result<state::EngineEntry, String> {
    let exe = engine::resolve_editor_exe(Path::new(&path));
    let info = spawn::probe_engine(&exe)?;

    let entry = state::EngineEntry::new(&exe.to_string_lossy(), info.engine_abi, info.build);
    let mut s = state::load();
    state::upsert_engine(&mut s.engines, entry.clone());
    state::save(&s)?;
    Ok(entry)
}

// Re-probe every registered engine and refresh its cached identity (abi/build).
//
// Registration caches the probe's answer, and in the dev loop the SAME exe
// path is rebuilt in place daily -- so a cached abi goes stale the moment the
// engine bumps, and the compatibility display then inverts: a project stamped
// by the CURRENT engine reads as incompatible against last week's number
// (caught live in the 2026-07-28 review: entry said abi 7, the exe answered 8).
// Called once per launch from the frontend, AFTER the first paint -- each probe
// is milliseconds, but it is still a process spawn per engine and does not
// belong on every refresh().
//
// An engine that fails its probe keeps its cached data: failure proves nothing
// new (the exe may be mid-rebuild), and open_project still reports the truth
// at launch time.
#[tauri::command]
fn refresh_engines() -> state::HubState {
    let mut s = state::load();
    let mut changed = false;
    for e in s.engines.iter_mut() {
        if let Ok(info) = spawn::probe_engine(Path::new(&e.path)) {
            if e.engine_abi != info.engine_abi || e.build != info.build {
                e.engine_abi = info.engine_abi;
                e.build = info.build;
                changed = true;
            }
        }
    }
    if changed {
        if let Err(err) = state::save(&s) {
            s.warnings.push(format!("Could not save refreshed engine info: {err}"));
        }
    }
    s
}

// Resolves a folder to the exe the same way register_engine does, so the two
// are symmetric: without it, forget_engine("C:/eng") matched nothing and
// reported success. Absence stays a silent no-op on purpose -- removing an
// engine twice is idempotent, and an error banner for "already gone" would be
// noise. Contrast set_project_engine, where a miss means state would be wrong.
#[tauri::command]
fn forget_engine(path: String) -> Result<(), String> {
    let mut s = state::load();
    let exe = engine::resolve_editor_exe(Path::new(&path));
    // The pin is an engine id, which IS the normalised exe path.
    let id = state::normalise_path(&exe.to_string_lossy());
    state::remove_engine(&mut s.engines, &exe.to_string_lossy());
    // Projects pinned to it fall back to the default rather than dangling.
    state::unpin_engine(&mut s.recents, &id);
    state::save(&s)
}

// Pin a project to a specific registered engine, or pass null to send it back
// to following the Hub default.
//
// Both arguments are VALIDATED. This is an IPC surface, and writing a pin to an
// unregistered engine would create exactly the dangling state unpin_engine
// exists to prevent -- silently, and persisted.
#[tauri::command]
fn set_project_engine(path: String, engine_id: Option<String>) -> Result<(), String> {
    let mut s = state::load();

    if let Some(id) = &engine_id {
        if !s.engines.iter().any(|e| &e.id == id) {
            return Err(format!("no engine is registered with id '{id}'"));
        }
    }
    if !state::set_project_engine(&mut s.recents, &path, engine_id) {
        return Err(format!("'{path}' is not in the project list"));
    }
    state::save(&s)
}

// Delete a project from disk, then drop it from the list.
//
// To the RECYCLE BIN, never remove_dir_all: this erases a whole project folder
// on one confirmation, and the difference between "recoverable" and "gone" is
// the difference between a bad afternoon and a lost one. `trash` goes through
// the Windows shell's IFileOperation, which is the same path Explorer's Delete
// takes, so the folder lands somewhere the user already knows how to restore
// from.
//
// Three things have to hold before anything is deleted:
//   1. the path resolves to a folder holding EXACTLY ONE .arcproj
//      (resolve_project, the engine's own rule) -- so this can never be aimed
//      at a folder that is not unambiguously a single project;
//   2. it is absolute and is not a drive root (project::delete_guard);
//   3. it still exists -- and if it does not, the entry is simply unlisted.
//      A project already gone from disk still has to be removable, or its row
//      would be stuck in the list with no control that can shift it.
#[tauri::command]
fn delete_project(path: String) -> Result<(), String> {
    let recorded = PathBuf::from(&path);

    if recorded.exists() {
        let (root, _) = resolve::resolve_project(&recorded)?;
        if let Some(why) = project::delete_guard(&root) {
            return Err(format!("refusing to delete {}: {why}", root.display()));
        }
        trash::delete(&root).map_err(|e| {
            format!(
                "could not delete {}: {e}. Is the project open in the editor?",
                root.display()
            )
        })?;
    }

    let mut s = state::load();
    state::remove_recent(&mut s.recents, &path);
    state::save(&s)
}

// Extra arguments appended after `--project <path>` when this project launches.
#[tauri::command]
fn set_project_args(path: String, args: String) -> Result<(), String> {
    let mut s = state::load();
    if !state::set_project_args(&mut s.recents, &path, args.trim()) {
        return Err(format!("'{path}' is not in the project list"));
    }
    state::save(&s)
}

// Open the project's folder in Explorer.
#[tauri::command]
fn reveal_project(path: String) -> Result<(), String> {
    let recorded = PathBuf::from(&path);
    let dir = resolve::project_dir(&recorded);
    if !dir.is_dir() {
        return Err(format!("{} is no longer on disk", dir.display()));
    }

    let mut cmd = Command::new("explorer");
    if recorded.is_file() {
        // `/select,<file>` opens the folder with the manifest highlighted.
        // raw_arg, not arg: Command would quote the whole `/select,C:\My
        // Games\x.arcproj` token as one string once it contains a space, and
        // explorer parses its own command line and does not accept that form.
        cmd.raw_arg(format!("/select,\"{}\"", recorded.display()));
    } else {
        cmd.arg(&dir);
    }
    // NOT status-checked: explorer.exe returns a non-zero exit code even when
    // it opens the window successfully.
    cmd.spawn()
        .map_err(|e| format!("could not open {}: {e}", dir.display()))?;
    Ok(())
}

// Rename a project: its folder, its .arcproj file, the `name` inside that
// manifest, and the Hub's own entry -- in that order, so the step most likely
// to fail comes first and every later failure rolls back cleanly.
//
// This is safe to do because NOTHING inside a project stores an absolute path.
// `Project::Open` derives the root from whatever path it is handed, mounts
// `game://` at `<root>/Content` and REBUILDS the Guid -> mount-path map from
// scratch on every open (AssetRegistry::ScanContent), with the GUIDs living in
// the asset files or their `.meta` sidecars -- so they travel with the files.
// `bootScene` is a `game://` URI and plugins resolve at `<root>/Plugins/<name>`.
// The manifest's `name` has one consumer, the editor window title
// (EditorApp.cpp:95).
//
// What this deliberately does NOT touch: `gameModule` (build output named by
// the project's own build scripts), anything under Source/, and generated
// solution files. Renaming a project is not renaming its game module, and
// quietly rewriting build inputs would be worse than leaving them alone.
#[tauri::command]
fn rename_project(path: String, new_name: String) -> Result<String, String> {
    let new_name = new_name.trim().to_string();
    if let Some(why) = project::name_error(&new_name) {
        return Err(why);
    }

    // Membership is checked BEFORE any disk work, so the state update at the
    // end cannot fail after the folder has already moved.
    let mut s = state::load();
    let key = state::normalise_path(&path);
    if !s.recents.iter().any(|e| state::normalise_path(&e.path) == key) {
        return Err(format!("'{path}' is not in the project list"));
    }

    let (root, manifest) = resolve::resolve_project(Path::new(&path))?;
    let parent = root
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .ok_or_else(|| format!("{} has no parent folder to rename within", root.display()))?;
    let new_root = parent.join(&new_name);

    // Read and edit the manifest in memory first: a malformed .arcproj is
    // discovered while nothing has moved.
    let text = std::fs::read_to_string(&manifest)
        .map_err(|e| format!("could not read {}: {e}", manifest.display()))?;
    let edited = project::rename_in_manifest(&text, &new_name)?;

    // A case-only rename ("mygame" -> "MyGame") normalises to the same key, so
    // the collision check must not fire on the project's own folder.
    let renaming_folder = state::normalise_path(&new_root.to_string_lossy())
        != state::normalise_path(&root.to_string_lossy());
    if renaming_folder && new_root.exists() {
        return Err(format!("{} already exists", new_root.display()));
    }

    if renaming_folder {
        std::fs::rename(&root, &new_root).map_err(|e| {
            format!(
                "could not rename {} to {}: {e}. Is the project open in the editor?",
                root.display(),
                new_root.display()
            )
        })?;
    }
    let undo_folder = || {
        if renaming_folder {
            let _ = std::fs::rename(&new_root, &root);
        }
    };

    // The manifest, now inside the renamed folder.
    let old_manifest = new_root.join(manifest.file_name().unwrap_or_default());
    let new_manifest = new_root.join(format!("{new_name}.{}", project::MANIFEST_EXT));
    if old_manifest != new_manifest {
        if let Err(e) = std::fs::rename(&old_manifest, &new_manifest) {
            undo_folder();
            return Err(format!("could not rename {}: {e}", old_manifest.display()));
        }
    }

    if let Err(e) = std::fs::write(&new_manifest, edited) {
        if old_manifest != new_manifest {
            let _ = std::fs::rename(&new_manifest, &old_manifest);
        }
        undo_folder();
        return Err(format!("could not write {}: {e}", new_manifest.display()));
    }

    let new_path = new_manifest.to_string_lossy().to_string();
    state::rename_recent(&mut s.recents, &path, &new_path, &new_name);
    state::save(&s)?;
    Ok(new_path)
}

// Remove ONE project from the Hub's own list. It does not touch the project on
// disk -- that is delete_project's job, and the menu labels the two apart.
// Restored 2026-07-28: the project-actions wave dropped it when Delete took its
// place on the card, which left "stop showing me this" impossible without
// erasing the folder. Unity Hub and Unreal both keep the two separate.
#[tauri::command]
fn forget_project(path: String) -> Result<(), String> {
    let mut s = state::load();
    state::remove_recent(&mut s.recents, &path);
    state::save(&s)
}

// Repoint a listed project whose folder moved -- the greyed row's Locate...
//
// `new_path` is a .arcproj the user just picked (the dialog only yields
// those), validated by the engine's own exactly-one-manifest rule via
// resolve_project. The entry keeps its engine pin and launch arguments --
// moving a folder changes neither -- but takes its name and ABI from the
// manifest just read, because the project may have changed while it was away.
// Locating is not opening: the list is not reordered and nothing launches.
#[tauri::command]
fn relocate_project(path: String, new_path: String) -> Result<(), String> {
    let (_root, manifest) = resolve::resolve_project(Path::new(&new_path))?;
    // 0 = unknown, same as open_project: a manifest without a readable abi is
    // "no conflict provable", never a guessed number.
    let abi = resolve::manifest_abi(&manifest).unwrap_or(0);
    let name = project::display_name(&new_path);

    let mut s = state::load();
    if !state::relocate_recent(&mut s.recents, &path, &new_path, &name, abi) {
        return Err(format!("'{path}' is not in the project list"));
    }
    state::save(&s)
}

// Same contract as forget_project, for the whole list: Hub state only.
#[tauri::command]
fn clear_recents() -> Result<(), String> {
    let mut s = state::load();
    s.recents.clear();
    state::save(&s)
}

#[tauri::command]
fn load_settings() -> settings::Settings {
    settings::load()
}

#[tauri::command]
fn save_settings(settings: settings::Settings) -> Result<(), String> {
    settings::save(&settings::Settings {
        default_project_dir: settings::clean_dir(&settings.default_project_dir),
        project_view: settings::clean_view(&settings.project_view),
        ..settings
    })
}

// The dialog's starting directory. A configured folder that has since been
// deleted degrades to "let the OS choose" rather than erroring -- the user
// wanted a file picker, not a lecture about their settings.
#[tauri::command]
fn default_dialog_dir() -> Option<String> {
    let dir = settings::clean_dir(&settings::load().default_project_dir);
    if dir.is_empty() || !Path::new(&dir).is_dir() {
        return None;
    }
    Some(dir)
}

#[tauri::command]
fn hub_data_dir() -> String {
    paths::hub_dir().to_string_lossy().to_string()
}

#[tauri::command]
fn reveal_hub_data_dir() -> Result<(), String> {
    let dir = paths::hub_dir();
    // Explorer fails on a path that does not exist yet, and the folder is only
    // created on first save -- so a fresh install would otherwise no-op.
    std::fs::create_dir_all(&dir).map_err(|e| format!("create {}: {e}", dir.display()))?;
    // NOT status-checked: explorer.exe returns a non-zero exit code even when
    // it opens the window successfully.
    Command::new("explorer")
        .arg(&dir)
        .spawn()
        .map_err(|e| format!("could not open {}: {e}", dir.display()))?;
    Ok(())
}

#[tauri::command]
fn hub_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}

// Launch the editor on a project. Deliberately does NOT wait: the Hub stays
// running, the editor is independent of it, and several editors may run at
// once. Closing the Hub must not close editors.
#[tauri::command]
fn open_project(app: tauri::AppHandle, project_path: String, engine_path: String) -> Result<(), String> {
    let proj = PathBuf::from(&project_path);
    let exe = engine::resolve_editor_exe(Path::new(&engine_path));
    if !proj.exists() {
        return Err(format!("project not found: {}", proj.display()));
    }
    if !exe.exists() {
        return Err(format!("engine not found: {}", exe.display()));
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

    // Watchdog: a spawn that SUCCEEDS can still be an editor that REFUSES a
    // moment later -- the project gate exits 2 before any window exists, so a
    // detached launch turns that refusal into click-then-nothing. Watch the
    // child briefly on a background thread and tell the frontend if it died at
    // boot; the launch itself stays instant (this command returns without
    // waiting). Two seconds covers the refuse-at-boot paths, which all exit
    // before device init; an editor alive past that is launched, and the
    // thread ends with the child disowned as before. stderr is deliberately
    // NOT piped: a pipe nobody drains after this thread ends would eventually
    // BLOCK the editor mid-log -- the exit code has to carry the story.
    {
        let app = app.clone();
        let shown = project::display_name(&project_path);
        std::thread::spawn(move || {
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
            while std::time::Instant::now() < deadline {
                match child.try_wait() {
                    Ok(Some(status)) => {
                        let why = match status.code() {
                            // Exit 2 is the editor's own project gate
                            // (ArcaneEditor main.cpp): wrong abi, unreadable
                            // manifest, refused boot.
                            Some(2) => "the editor refused the project (engine/abi gate)".to_string(),
                            Some(c) => format!("the editor exited immediately with code {c}"),
                            None => "the editor was killed before it opened".to_string(),
                        };
                        use tauri::Emitter;
                        let _ = app.emit("launch-failed", format!("{shown}: {why}."));
                        return;
                    }
                    Ok(None) => std::thread::sleep(std::time::Duration::from_millis(100)),
                    Err(_) => return,
                }
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
            // The project was just found on disk to launch; load() re-stamps
            // this on every read regardless.
            missing: false,
        },
    );
    state::save(&s)
}

#[tauri::command]
fn create_project(dir: String, name: String, engine_path: String) -> Result<String, String> {
    let exe = engine::resolve_editor_exe(Path::new(&engine_path));
    // Probe FIRST and abort on failure -- never fall back to a guessed ABI.
    let info = spawn::probe_engine(&exe)?;

    let root = PathBuf::from(&dir).join(&name);
    if root.exists() && std::fs::read_dir(&root).map(|mut d| d.next().is_some()).unwrap_or(false) {
        return Err(format!("{} already exists and is not empty", root.display()));
    }
    std::fs::create_dir_all(root.join("Content"))
        .map_err(|e| format!("could not create {}: {e}", root.display()))?;

    let manifest = project::manifest_json(&name, info.engine_abi)?;
    let file = root.join(format!("{name}.arcproj"));
    std::fs::write(&file, manifest).map_err(|e| format!("could not write {}: {e}", file.display()))?;

    // Returns the .arcproj FILE, not the folder: the caller feeds this straight
    // into open_project, and recents should record a newly created project the
    // same way it records one the user picked through the Open dialog.
    Ok(file.to_string_lossy().to_string())
}

// Adjacency is a SUGGESTION for the dev loop, never an assumption: the staged
// dev build lands in Arcane/bin/<Config>-windows-x86_64-md/Hub/ beside
// .../ArcaneEditor/. Returns a path only if it actually probes clean.
#[tauri::command]
fn suggest_engine() -> Option<state::EngineEntry> {
    let exe_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
    let candidates = [
        exe_dir.join(engine::EDITOR_EXE),
        exe_dir.join("..").join("ArcaneEditor").join(engine::EDITOR_EXE),
    ];
    for c in candidates {
        if let Ok(info) = spawn::probe_engine(&c) {
            return Some(state::EngineEntry {
                id: state::normalise_path(&c.to_string_lossy()),
                path: c.to_string_lossy().to_string(),
                engine_abi: info.engine_abi,
                build: info.build,
                // It answered the probe one line up; it is on disk.
                missing: false,
            });
        }
    }
    None
}

pub fn run() {
    tauri::Builder::default()
        // FIRST plugin registered, per its own docs: it has to win the race
        // before anything else initialises. A second launch lands in this
        // callback inside the FIRST process; the new process exits on its own.
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            // "Show me the Hub": surface the window the user already has.
            use tauri::Manager;
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }))
        .plugin(tauri_plugin_dialog::init())
        .setup(|_app| {
            // State moved from roaming %APPDATA% to %LOCALAPPDATA% on
            // 2026-07-28 (machine-specific paths do not roam); copy old files
            // in once, before the webview's first load_state can run.
            paths::migrate_legacy_state();
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            load_state,
            register_engine,
            refresh_engines,
            forget_engine,
            delete_project,
            forget_project,
            relocate_project,
            clear_recents,
            set_project_engine,
            set_project_args,
            reveal_project,
            rename_project,
            open_project,
            create_project,
            suggest_engine,
            load_settings,
            save_settings,
            default_dialog_dir,
            hub_data_dir,
            reveal_hub_data_dir,
            hub_version
        ])
        .run(tauri::generate_context!())
        .expect("error while running the Arcane Hub");
}
