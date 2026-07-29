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
pub mod tray;

use std::collections::HashMap;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;

/// Live editors, keyed by normalised PROJECT path -> pid. One editor per
/// project: the dangerous case is the same project open twice (two editors
/// saving one project's files clobber each other); two different projects on
/// one engine build stay legal. The pid, not the Child, lives here -- the
/// wait thread owns the Child for its whole lifetime.
pub struct RunningEditors(pub Mutex<HashMap<String, u32>>);

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

/// Tell the frontend which projects have a live editor right now. Emitted on
/// every spawn and every exit; the payload is the full key set rather than a
/// delta, so a missed event costs one stale badge until the next, not a
/// permanently wrong count. Keys are `state::normalise_path` of the recorded
/// project path -- format.ts `normalisePath` mirrors that fold exactly.
fn emit_running(app: &tauri::AppHandle) {
    use tauri::{Emitter, Manager};
    let keys: Vec<String> = app
        .state::<RunningEditors>()
        .0
        .lock()
        .unwrap()
        .keys()
        .cloned()
        .collect();
    let _ = app.emit("running-changed", keys);
}

/// The same key set, pulled: the frontend's initial paint cannot wait for an
/// event that only fires on the next transition.
#[tauri::command]
fn running_projects(editors: tauri::State<RunningEditors>) -> Vec<String> {
    editors.0.lock().unwrap().keys().cloned().collect()
}

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

// Star or unstar a project. The frontend pins favourites above the rest.
#[tauri::command]
fn set_project_favorite(path: String, favorite: bool) -> Result<(), String> {
    let mut s = state::load();
    if !state::set_favorite(&mut s.recents, &path, favorite) {
        return Err(format!("'{path}' is not in the project list"));
    }
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

// Duplicate a project on disk beside the original and list the copy.
//
// `async fn` ON PURPOSE: this is the one command whose duration scales with
// the project (Content can be gigabytes), and a sync command runs on the main
// thread with the whole window frozen for the duration. On the async runtime
// the frontend's busy state stays honest instead.
//
// What a duplicate IS: the content, source, config and plugins -- everything
// that cannot be rebuilt -- under a fresh name. Build output (Binaries/,
// Intermediate/, at any depth) and .git are deliberately not copied
// (project::DUPLICATE_SKIP has the reasoning per name). Asset GUIDs copy
// as-is: they live in asset files/.meta sidecars and are scoped to the
// project's own registry, which rescans on open, so two projects holding the
// same GUIDs never meet.
#[tauri::command]
async fn duplicate_project(path: String) -> Result<String, String> {
    let (root, manifest) = resolve::resolve_project(Path::new(&path))?;
    let parent = root
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .ok_or_else(|| format!("{} has no parent folder to copy within", root.display()))?;

    // First free "X Copy" / "X Copy N" slot beside the original.
    let base = project::display_name(&path);
    let mut pick = None;
    for n in 1..=99 {
        let name = project::copy_name(&base, n);
        let dir = parent.join(&name);
        if !dir.exists() {
            pick = Some((name, dir));
            break;
        }
    }
    let (new_name, new_root) =
        pick.ok_or_else(|| format!("99 copies of {base} already exist here"))?;
    // The suffix can push a legal name over the 64-char cap; gate it like a
    // typed name rather than minting a folder rename_project would refuse.
    if let Some(why) = project::name_error(&new_name) {
        return Err(why);
    }

    // Read and edit the manifest in memory BEFORE any disk work (the
    // rename_project rule: discover a malformed .arcproj while nothing has
    // been created yet).
    let text = std::fs::read_to_string(&manifest)
        .map_err(|e| format!("could not read {}: {e}", manifest.display()))?;
    let edited = project::rename_in_manifest(&text, &new_name)?;

    // Any failure past this point removes new_root -- it did not exist before
    // this command (checked above), so the remove can only take the partial
    // copy this command just made.
    let fail = |e: String| {
        let _ = std::fs::remove_dir_all(&new_root);
        Err(e)
    };
    if let Err(e) = resolve::copy_tree(&root, &new_root) {
        return fail(e);
    }
    let copied = new_root.join(manifest.file_name().unwrap_or_default());
    let new_manifest = new_root.join(format!("{new_name}.{}", project::MANIFEST_EXT));
    if copied != new_manifest {
        if let Err(e) = std::fs::rename(&copied, &new_manifest) {
            return fail(format!("could not rename {}: {e}", copied.display()));
        }
    }
    if let Err(e) = std::fs::write(&new_manifest, &edited) {
        return fail(format!("could not write {}: {e}", new_manifest.display()));
    }

    // List the copy. The engine pin and arguments carry from the source (the
    // same engine opens it, the same switches apply); the star does not --
    // favouriting the original is not favouriting its copies. The stamp is
    // NOW even though the copy has never been opened: the Opened column's
    // real job is recency-of-interaction, creating IS the interaction, and a
    // "never"-stamped copy would sort to the bottom and read as a failure.
    let mut s = state::load();
    let key = state::normalise_path(&path);
    let (engine_id, args) = s
        .recents
        .iter()
        .find(|e| state::normalise_path(&e.path) == key)
        .map(|e| (e.engine_id.clone(), e.args.clone()))
        .unwrap_or_default();
    let new_path = new_manifest.to_string_lossy().to_string();
    state::touch_recent(
        &mut s.recents,
        state::RecentProject {
            path: new_path.clone(),
            name: new_name,
            last_opened_utc: now_utc_iso(),
            engine_abi: project::parse_manifest_abi(&edited).unwrap_or(0),
            engine_id,
            args,
            favorite: false,
            missing: false,
        },
    );
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
        launch_behavior: settings::clean_behavior(&settings.launch_behavior),
        project_sort: settings::clean_sort(&settings.project_sort),
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

// Launch the editor on a project, as a TRACKED child: the command still
// returns immediately (a wait thread owns the child), several editors may run
// at once, and closing the Hub still cannot close editors -- Windows children
// survive their parent absent a job object. What tracking buys: one editor
// per project (a second launch focuses the running one), hide-the-Hub while
// editors run (restored when the last exits), and the boot-refusal watchdog
// riding the same wait instead of a separate 2s timer.
#[tauri::command]
fn open_project(
    app: tauri::AppHandle,
    editors: tauri::State<RunningEditors>,
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

    // Probe RIGHT BEFORE spawning, not only at registration or Hub launch: a
    // long-lived Hub spans in-place rebuilds, and this is the last moment the
    // compatibility story can be made true. A probe failure refuses the launch
    // with a reason -- an engine that cannot answer its own identity probe
    // (mid-rebuild, missing DLLs) is not about to open a project, and spawning
    // it anyway would produce a silent nothing.
    let info = spawn::probe_engine(&exe)?;

    // One editor per project: a second launch means "take me there", so the
    // running editor's window is focused instead of spawning a rival that
    // would race it on every save. A pid whose window cannot be found is a
    // stale entry (the wait thread races this check) and launches fresh.
    let project_key = state::normalise_path(&project_path);
    {
        let mut live = editors.0.lock().unwrap();
        if let Some(&pid) = live.get(&project_key) {
            if spawn::focus_process_window(pid) {
                return Ok(OpenOutcome::Focused);
            }
            live.remove(&project_key);
        }
    }

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

    editors.0.lock().unwrap().insert(project_key.clone(), child.id());
    emit_running(&app);

    // Hand the screen to the editor, per the configured behaviour. Checked
    // AFTER a successful spawn so a refused launch never hides the window the
    // error banner lives in. `stay` does nothing by definition.
    match settings::load().launch_behavior.as_str() {
        settings::BEHAVIOR_TRAY => {
            let _ = tray::park(&app);
        }
        settings::BEHAVIOR_HIDE => {
            use tauri::Manager;
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.hide();
            }
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

            use tauri::{Emitter, Manager};
            let none_left = {
                let editors = app.state::<RunningEditors>();
                let mut live = editors.0.lock().unwrap();
                live.remove(&key);
                live.is_empty()
            };
            emit_running(&app);

            if started.elapsed() < std::time::Duration::from_secs(2) {
                let why = match status.and_then(|s| s.code()) {
                    // Exit 2 is the editor's own project gate (ArcaneEditor
                    // main.cpp): wrong abi, unreadable manifest, refused boot.
                    Some(2) => "the editor refused the project (engine/abi gate)".to_string(),
                    Some(c) => format!("the editor exited immediately with code {c}"),
                    None => "the editor was killed before it opened".to_string(),
                };
                let _ = app.emit("launch-failed", format!("{shown}: {why}."));
            }

            // How the window comes back is decided by the setting NOW, not
            // the one at launch time, so toggling mid-run behaves. Tray mode
            // deliberately does NOT restore: the icon is the handle, and a
            // launcher window popping itself up when the work closes is the
            // Epic habit the tray exists to avoid. Every other mode restores
            // -- but only a window that is actually hidden, so `stay` never
            // steals focus from whatever the user moved on to.
            if none_left {
                let hidden = app
                    .get_webview_window("main")
                    .map(|w| !w.is_visible().unwrap_or(true))
                    .unwrap_or(false);
                if hidden && settings::load().launch_behavior != settings::BEHAVIOR_TRAY {
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

/// What a Scan did, for the frontend's one-line report. Every count is
/// surfaced -- a scan that silently dropped ambiguous folders or gave up on
/// a budget would read as "found everything" when it did not.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanReport {
    pub added: u32,
    pub already_listed: u32,
    pub ambiguous: u32,
    pub truncated: bool,
}

// Godot's Scan: walk a folder tree and list every project found under it.
//
// `async` like duplicate_project: the walk's duration scales with the tree
// the user picks. Found projects APPEND with a never-opened stamp -- a bulk
// import must not bury the real recents, and "never" is the truth about a
// project the Hub has not launched. Engine pins are not invented; each entry
// follows the default until the user says otherwise.
#[tauri::command]
async fn scan_for_projects(dir: String) -> Result<ScanReport, String> {
    let outcome = resolve::scan_tree(Path::new(&dir))?;

    let found: Vec<state::RecentProject> = outcome
        .manifests
        .iter()
        .map(|m| {
            let path = m.to_string_lossy().to_string();
            state::RecentProject {
                name: project::display_name(&path),
                engine_abi: resolve::manifest_abi(m).unwrap_or(0),
                path,
                last_opened_utc: "0".to_string(),
                engine_id: None,
                args: String::new(),
                favorite: false,
                missing: false,
            }
        })
        .collect();

    let mut s = state::load();
    let (added, already_listed) = state::add_scanned(&mut s.recents, found);
    if added > 0 {
        state::save(&s)?;
    }
    Ok(ScanReport { added, already_listed, ambiguous: outcome.ambiguous, truncated: outcome.truncated })
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

// The disk watcher: a 2s poll that keeps `missing` honest while the Hub sits
// open. Without it, a project moved mid-session looked healthy until some
// OTHER action happened to reload the list (caught live 2026-07-29). A poll,
// not a filesystem watcher, on purpose: noticing a folder's DISAPPEARANCE
// with notify means watching every parent chain on every involved drive and
// re-arming on each move, while the truth here is a handful of stat calls
// (`load` re-stamps `missing` from the disk on every read). Emits only on a
// transition, so an idle Hub sends nothing and the frontend never repaints
// for no reason.
fn spawn_disk_watch(app: tauri::AppHandle) {
    std::thread::spawn(move || {
        use tauri::Emitter;
        let mut last = state::disk_fingerprint(&state::load());
        loop {
            std::thread::sleep(std::time::Duration::from_secs(2));
            let s = state::load();
            let now = state::disk_fingerprint(&s);
            if now != last {
                last = now;
                let _ = app.emit("state-changed", &s);
            }
        }
    });
}

pub fn run() {
    // Before ANY window exists: claim the taskbar family id ArcaneEditor also
    // sets, so the Hub's and every editor's buttons stack as one group.
    spawn::claim_app_user_model_id();
    tauri::Builder::default()
        // FIRST plugin registered, per its own docs: it has to win the race
        // before anything else initialises. A second launch lands in this
        // callback inside the FIRST process; the new process exits on its own.
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            // "Show me the Hub": surface the window the user already has.
            // While editors run the window may be hidden or parked in the
            // tray, and relaunching the Hub is the designed way to get it
            // back before they exit; show_hub also clears the tray icon.
            tray::show_hub(app);
        }))
        .plugin(tauri_plugin_dialog::init())
        .manage(RunningEditors(Mutex::new(HashMap::new())))
        .setup(|app| {
            use tauri::Manager;
            spawn_disk_watch(app.app_handle().clone());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            load_state,
            register_engine,
            refresh_engines,
            forget_engine,
            delete_project,
            duplicate_project,
            forget_project,
            relocate_project,
            clear_recents,
            set_project_engine,
            set_project_favorite,
            set_project_args,
            reveal_project,
            rename_project,
            open_project,
            running_projects,
            scan_for_projects,
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
