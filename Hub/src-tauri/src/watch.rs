// The disk watcher: a 2s poll that keeps `missing` and the running badges
// honest while the Hub sits open. Without it, a project moved mid-session
// looked healthy until some OTHER action happened to reload the list
// (caught live 2026-07-29). A poll, not a filesystem watcher, on purpose:
// noticing a folder's DISAPPEARANCE with notify means watching every parent
// chain on every involved drive and re-arming on each move, while the truth
// here is a handful of stat calls (`load` re-stamps `missing` from the disk
// on every read). Emits only on transitions, so an idle Hub sends nothing
// and the frontend never repaints for no reason.

use crate::{launch, state};

pub fn spawn_disk_watch(app: tauri::AppHandle) {
    std::thread::spawn(move || {
        use tauri::Emitter;
        let mut last = state::disk_fingerprint(&state::load());
        // The lock-derived half of the running set changes without any map
        // event -- an editor from a previous Hub session exits, or one is
        // launched by hand -- so the watcher owns those transitions too.
        let mut last_running = launch::running_keys(&app);
        loop {
            std::thread::sleep(std::time::Duration::from_secs(2));
            let s = state::load();
            let now = state::disk_fingerprint(&s);
            if now != last {
                last = now;
                let _ = app.emit("state-changed", &s);
            }
            let now_running = launch::running_keys(&app);
            if now_running != last_running {
                last_running = now_running.clone();
                let _ = app.emit("running-changed", now_running);
            }
        }
    });
}
