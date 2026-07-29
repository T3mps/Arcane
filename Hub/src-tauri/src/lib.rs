// Arcane Hub -- Tauri entry point.
//
// Split, mirroring Tools/setup-wizard: all DECISION logic lives in pure
// modules that `cargo test` covers; the IO halves each own one story --
// spawn.rs (process spawning), resolve.rs (recorded-path resolution + disk
// choreography), launch.rs (the launch lifecycle: the launch core, its wait
// thread, the shell route, the running set), watch.rs (the background disk
// poll) -- and THIS file is only the #[tauri::command] IPC surface plus the
// state read-modify-write each command performs, and the run() builder.
// A command with a body longer than a screen belongs in one of those halves;
// this file earned that rule by quietly growing to 1,100 untested lines
// before the 2026-07-29 split restored it.
//
// The Hub is INSTALLED per-user (%LOCALAPPDATA%\Arcane Hub\), so it must
// never derive a repo root from current_exe() and must never assume an
// engine sits beside it. That is setup-wizard's repo-root-portable assumption
// and it is wrong here. Engines are REGISTERED BY PATH and validated with the
// engine's own --print-engine-info probe.

pub mod assoc;
pub mod editorlock;
pub mod engine;
pub mod launch;
pub mod paths;
pub mod project;
pub mod resolve;
pub mod settings;
pub mod spawn;
pub mod state;
pub mod store;
pub mod tray;
pub mod watch;

use std::collections::HashMap;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use launch::{OpenOutcome, RunningEditors};

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
// the frontend's busy state stays honest instead. The disk half lives in
// resolve::duplicate_project_files (tested there); this is the listing.
#[tauri::command]
async fn duplicate_project(path: String) -> Result<String, String> {
    let dup = resolve::duplicate_project_files(Path::new(&path))?;

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
    let new_path = dup.manifest.to_string_lossy().to_string();
    state::touch_recent(
        &mut s.recents,
        state::RecentProject {
            path: new_path.clone(),
            name: dup.name,
            last_opened_utc: launch::now_utc_iso(),
            engine_abi: dup.abi,
            engine_id,
            args,
            favorite: false,
            missing: false,
            // The freshly minted identity, never the original's.
            guid: Some(dup.guid),
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
    let guid = resolve::manifest_guid(&manifest);
    let name = project::display_name(&new_path);

    let mut s = state::load();
    if !state::relocate_recent(&mut s.recents, &path, &new_path, &name, abi, guid) {
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

// Launch the editor on a project, as a TRACKED child: the command returns
// immediately (launch.rs's wait thread owns the child), several editors may
// run at once, and closing the Hub still cannot close editors -- Windows
// children survive their parent absent a job object. The whole story --
// focus-existing, the probe, the behaviour, the watchdog -- lives in
// launch::do_open_project, shared with the .arcproj shell route.
#[tauri::command]
fn open_project(
    app: tauri::AppHandle,
    project_path: String,
    engine_path: String,
) -> Result<OpenOutcome, String> {
    launch::do_open_project(&app, project_path, engine_path)
}

/// The running key set, pulled: the frontend's initial paint cannot wait for
/// an event that only fires on the next transition.
#[tauri::command]
fn running_projects(app: tauri::AppHandle) -> Vec<String> {
    launch::running_keys(&app)
}

// Cover thumbnails for a batch of recorded project paths, keyed by the exact
// path handed in. One IPC round-trip for the whole grid; only projects WITH
// a cover appear in the map, so the frontend's fallback is a simple miss.
#[tauri::command]
fn project_covers(paths: Vec<String>) -> HashMap<String, String> {
    paths
        .into_iter()
        .filter_map(|p| resolve::project_cover(Path::new(&p)).map(|c| (p, c)))
        .collect()
}

/// What a Scan did, for the frontend's one-line report. Every count is
/// surfaced -- a scan that silently dropped ambiguous folders or gave up on
/// a budget would read as "found everything" when it did not. `relocated`
/// counts missing entries healed onto a found project by guid: those rows
/// un-greyed rather than appeared, which deserves its own word.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanReport {
    pub added: u32,
    pub already_listed: u32,
    pub relocated: u32,
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
                // Read here so add_scanned can heal a moved project's missing
                // entry onto this find instead of listing a stranger beside it.
                guid: resolve::manifest_guid(m),
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
    let (added, already_listed, relocated) = state::add_scanned(&mut s.recents, found);
    if added > 0 || relocated > 0 {
        state::save(&s)?;
    }
    Ok(ScanReport {
        added,
        already_listed,
        relocated,
        ambiguous: outcome.ambiguous,
        truncated: outcome.truncated,
    })
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

    // A fresh identity at birth, same as the engine's own Project::Create --
    // so a Hub-created project never needs the engine's open-time self-heal.
    let manifest = project::manifest_json(&name, info.engine_abi, &project::new_guid())?;
    let file = root.join(format!("{name}.arcproj"));
    std::fs::write(&file, manifest).map_err(|e| format!("could not write {}: {e}", file.display()))?;

    // Returns the .arcproj FILE, not the folder: the caller feeds this straight
    // into open_project, and recents should record a newly created project the
    // same way it records one the user picked through the Open dialog.
    Ok(file.to_string_lossy().to_string())
}

// Adjacency is a SUGGESTION for the dev loop, never an assumption. Returns a
// path only if it actually probes clean. (From the installed location this
// never fires -- engines are registered by path; this exists for a portable
// exe dropped beside a build.)
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
    // Before ANY window exists: claim the taskbar family id ArcaneEditor also
    // sets, so the Hub's and every editor's buttons stack as one group.
    spawn::claim_app_user_model_id();
    // And claim (or repair) the .arcproj association for this exe -- the
    // installer's own write shipped unquoted once, and a self-heal at every
    // start outlives any installer fix. See assoc.rs for the incident.
    assoc::claim_arcproj_association();
    tauri::Builder::default()
        // FIRST plugin registered, per its own docs: it has to win the race
        // before anything else initialises. A second launch lands in this
        // callback inside the FIRST process; the new process exits on its own.
        .plugin(tauri_plugin_single_instance::init(|app, args, _cwd| {
            // A second launch carrying a .arcproj is a FILE OPEN (the shell
            // association route), not "show me the Hub" -- route it and leave
            // the window wherever the launch lifecycle puts it.
            if launch::route_shell_args(app, args.iter().map(|s| s.as_str())) {
                return;
            }
            // Otherwise: surface the window the user already has. While
            // editors run it may be hidden or parked in the tray, and
            // relaunching the Hub is the designed way to get it back before
            // they exit; show_hub also clears the tray icon.
            tray::show_hub(app);
        }))
        .plugin(tauri_plugin_dialog::init())
        // SIZE | POSITION | MAXIMIZED only. Deliberately NOT the default
        // all-flags: VISIBLE would fight the hidden-start contract (the
        // window is created invisible so a shell-routed launch never
        // flashes), and DECORATIONS/FULLSCREEN are fixed by config.
        .plugin(
            tauri_plugin_window_state::Builder::default()
                .with_state_flags(
                    tauri_plugin_window_state::StateFlags::SIZE
                        | tauri_plugin_window_state::StateFlags::POSITION
                        | tauri_plugin_window_state::StateFlags::MAXIMIZED,
                )
                .build(),
        )
        .manage(RunningEditors::new())
        .setup(|app| {
            use tauri::Manager;
            watch::spawn_disk_watch(app.app_handle().clone());
            // The window is created HIDDEN (tauri.conf.json): a cold start
            // via the file association used to flash the Hub for a moment
            // before the launch decision hid it again (user report,
            // 2026-07-29). Now a shell-routed start stays invisible and the
            // launch outcome decides -- tray parks, close stays away, `stay`
            // and every failure path show the window -- while a NORMAL start
            // shows right here, the same moment it used to appear.
            let args: Vec<String> = std::env::args().collect();
            let routed = launch::route_shell_args(app.app_handle(), args.iter().map(|s| s.as_str()));
            if !routed {
                if let Some(w) = app.get_webview_window("main") {
                    let _ = w.show();
                }
            }
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
            project_covers,
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
