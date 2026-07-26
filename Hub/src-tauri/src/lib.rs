// Arcane Hub -- Tauri entry point.
//
// Split, mirroring Tools/setup-wizard: all DECISION logic lives in pure modules
// that `cargo test` covers; this file only does process spawning, file IO and
// IPC, which are not unit-tested.
//
// The Hub is INSTALLED per-user (%LOCALAPPDATA%\Programs\Arcane Hub\), so it
// must never derive a repo root from current_exe() and must never assume an
// engine sits beside it. That is setup-wizard's repo-root-portable assumption
// and it is wrong here.

pub mod paths;
pub mod state;

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .run(tauri::generate_context!())
        .expect("error while running the Arcane Hub");
}
