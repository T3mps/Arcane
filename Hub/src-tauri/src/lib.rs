// Arcane Hub -- Tauri entry point.
//
// Split, mirroring Tools/setup-wizard: all DECISION logic lives in pure modules
// that `cargo test` covers; this file only does process spawning, file IO and
// IPC, which are not unit-tested.
//
// The Hub is INSTALLED per-user (%LOCALAPPDATA%\Programs\Arcane Hub\), so it
// must never derive a repo root from current_exe() and must never assume an
// engine sits beside it. That is setup-wizard's repo-root-portable assumption
// and it is wrong here. Engines are REGISTERED BY PATH and validated with the
// engine's own --print-engine-info probe.

pub mod engine;
pub mod paths;
pub mod project;
pub mod state;

use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

// ArcaneEditor is a ConsoleApp, so without this every spawn flashes a console.
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

fn now_utc_iso() -> String {
    // Seconds since the epoch is enough to sort "last opened" and avoids
    // pulling a date crate in for a display string.
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    secs.to_string()
}

// Run the engine's identity probe. Failure modes the UI must be able to show
// verbatim: not an Arcane engine, exe missing, probe printed nothing.
fn probe_engine(exe: &Path) -> Result<engine::EngineInfo, String> {
    if !exe.exists() {
        return Err(format!("no {} at {}", engine::EDITOR_EXE, exe.display()));
    }
    let out = Command::new(exe)
        .arg("--print-engine-info")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("could not run {}: {e}", exe.display()))?;

    if !out.status.success() {
        return Err(format!(
            "{} exited with {} -- not an Arcane engine?",
            exe.display(),
            out.status
        ));
    }
    engine::parse_probe_output(&String::from_utf8_lossy(&out.stdout))
}

#[tauri::command]
fn load_state() -> state::HubState {
    state::load()
}

#[tauri::command]
fn register_engine(path: String) -> Result<state::EngineEntry, String> {
    let exe = engine::resolve_editor_exe(Path::new(&path));
    let info = probe_engine(&exe)?;

    let entry = state::EngineEntry {
        id: state::normalise_path(&exe.to_string_lossy()),
        path: exe.to_string_lossy().to_string(),
        engine_abi: info.engine_abi,
        build: info.build,
    };
    let mut s = state::load();
    state::upsert_engine(&mut s.engines, entry.clone());
    state::save(&s)?;
    Ok(entry)
}

#[tauri::command]
fn forget_engine(path: String) -> Result<(), String> {
    let mut s = state::load();
    state::remove_engine(&mut s.engines, &path);
    state::save(&s)
}

#[tauri::command]
fn forget_project(path: String) -> Result<(), String> {
    let mut s = state::load();
    state::remove_recent(&mut s.recents, &path);
    state::save(&s)
}

// Launch the editor on a project. Deliberately does NOT wait: the Hub stays
// running, the editor is independent of it, and several editors may run at
// once. Closing the Hub must not close editors.
#[tauri::command]
fn open_project(project_path: String, engine_path: String) -> Result<(), String> {
    let proj = PathBuf::from(&project_path);
    let exe = engine::resolve_editor_exe(Path::new(&engine_path));
    if !proj.exists() {
        return Err(format!("project not found: {}", proj.display()));
    }
    if !exe.exists() {
        return Err(format!("engine not found: {}", exe.display()));
    }

    Command::new(&exe)
        .arg("--project")
        .arg(&proj)
        // The editor resolves shaders and plugin DLLs relative to its own
        // directory, so it must start there (same reason scripts/launch.ps1
        // sets the working directory).
        .current_dir(exe.parent().unwrap_or(Path::new(".")))
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("could not launch the editor: {e}"))?;

    // Record the open. A project the user just opened belongs at the top of
    // the list even if the editor later fails to load it.
    let mut s = state::load();
    let name = proj
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| project_path.clone());
    let abi = s
        .engines
        .iter()
        .find(|e| state::normalise_path(&e.path) == state::normalise_path(&exe.to_string_lossy()))
        .map(|e| e.engine_abi)
        .unwrap_or(0);
    state::touch_recent(
        &mut s.recents,
        state::RecentProject {
            path: proj.to_string_lossy().to_string(),
            name,
            last_opened_utc: now_utc_iso(),
            engine_abi: abi,
        },
    );
    state::save(&s)
}

#[tauri::command]
fn create_project(dir: String, name: String, engine_path: String) -> Result<String, String> {
    let exe = engine::resolve_editor_exe(Path::new(&engine_path));
    // Probe FIRST and abort on failure -- never fall back to a guessed ABI.
    let info = probe_engine(&exe)?;

    let root = PathBuf::from(&dir).join(&name);
    if root.exists() && std::fs::read_dir(&root).map(|mut d| d.next().is_some()).unwrap_or(false) {
        return Err(format!("{} already exists and is not empty", root.display()));
    }
    std::fs::create_dir_all(root.join("Content"))
        .map_err(|e| format!("could not create {}: {e}", root.display()))?;

    let manifest = project::manifest_json(&name, info.engine_abi)?;
    let file = root.join(format!("{name}.arcproj"));
    std::fs::write(&file, manifest).map_err(|e| format!("could not write {}: {e}", file.display()))?;

    Ok(root.to_string_lossy().to_string())
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
        if let Ok(info) = probe_engine(&c) {
            return Some(state::EngineEntry {
                id: state::normalise_path(&c.to_string_lossy()),
                path: c.to_string_lossy().to_string(),
                engine_abi: info.engine_abi,
                build: info.build,
            });
        }
    }
    None
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            load_state,
            register_engine,
            forget_engine,
            forget_project,
            open_project,
            create_project,
            suggest_engine
        ])
        .run(tauri::generate_context!())
        .expect("error while running the Arcane Hub");
}
