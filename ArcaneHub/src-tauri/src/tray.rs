// The tray half of the launch lifecycle. In the default launch behaviour the
// Hub does not vanish while editors run -- it parks in the system tray, where
// a left-click brings the window back and a right-click offers the quick
// actions a launcher earns a tray for: open, launch a recent project, quit.
//
// The icon exists ONLY while the window is parked. Creating it at startup
// would make the Hub a resident background app, which is the exact
// tray-lingering habit users complain about in Unity Hub and the Epic
// launcher; creating it on park and removing it on every show path means the
// tray is a state, not a residence.

use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Emitter, Manager};

use crate::state;

/// One well-known id: created on park, removed by id on every show path, so
/// "is the icon up" never needs its own bookkeeping.
const TRAY_ID: &str = "hub-tray";

/// How many recent projects the tray menu offers to launch. The recents Vec
/// is newest-first (touch_recent inserts at the front), so "the first N that
/// are on disk" is exactly "the N the user opened last".
const QUICK_LAUNCH: usize = 5;

/// Surface the Hub window and take the tray icon down. The ONE show path:
/// the single-instance callback, the wait thread's restore and the tray's
/// own Open all route here, so no path can forget the other half and leave
/// a dead icon beside a visible window.
pub fn show_hub(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.show();
        let _ = w.unminimize();
        let _ = w.set_focus();
    }
    remove(app);
}

pub fn remove(app: &AppHandle) {
    let _ = app.remove_tray_by_id(TRAY_ID);
}

/// Hide the window into the tray. The menu is built fresh on every park, so
/// the quick-launch entries are as current as the moment the user launched --
/// a menu is not a view, and rebuilding beats keeping one synchronised.
pub fn park(app: &AppHandle) -> tauri::Result<()> {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.hide();
    }
    // A second launch while already parked would otherwise stack two icons.
    remove(app);

    let open = MenuItemBuilder::with_id("open", "Open Arcane Hub").build(app)?;
    let mut menu = MenuBuilder::new(app).item(&open).separator();

    let recents = state::load().recents;
    let launchable: Vec<_> = recents.iter().filter(|p| !p.missing).take(QUICK_LAUNCH).collect();
    for p in &launchable {
        // The id carries the project path; the frontend resolves which engine
        // launches it, through the SAME launch path a card click takes --
        // probe, focus-existing, outcomes and all. Duplicating that engine
        // resolution Rust-side would be a second brain that drifts.
        let item =
            MenuItemBuilder::with_id(format!("launch:{}", p.path), &p.name).build(app)?;
        menu = menu.item(&item);
    }
    if !launchable.is_empty() {
        menu = menu.separator();
    }
    let quit = MenuItemBuilder::with_id("quit", "Quit Hub").build(app)?;
    let menu = menu.item(&quit).build()?;

    let mut tray = TrayIconBuilder::with_id(TRAY_ID)
        .tooltip("Arcane Hub")
        .menu(&menu)
        // Left-click is "show me the Hub", not "show me a menu": the menu is
        // the right-click's job, matching every other tray citizen.
        .show_menu_on_left_click(false)
        .on_menu_event(|app, ev| match ev.id().as_ref() {
            "open" => show_hub(app),
            // Quit quits the HUB. Editors are independent processes and
            // survive, same as closing the window would leave them.
            // remove() BEFORE exit: app.exit ends the process without
            // reliably running the tray icon's Drop, and Windows then shows
            // the dead icon until the tray next repaints. Quit is only
            // reachable while parked (the menu lives on the icon), so
            // without this every tray-quit left a ghost icon -- the
            // 2026-08-11 tray-full-of-corpses incident.
            "quit" => {
                remove(app);
                app.exit(0);
            }
            id => {
                if let Some(path) = id.strip_prefix("launch:") {
                    let _ = app.emit("tray-launch", path.to_string());
                }
            }
        })
        .on_tray_icon_event(|tray, ev| {
            if let TrayIconEvent::Click {
                button: MouseButton::Left,
                button_state: MouseButtonState::Up,
                ..
            } = ev
            {
                show_hub(tray.app_handle());
            }
        });
    if let Some(icon) = app.default_window_icon() {
        tray = tray.icon(icon.clone());
    }
    tray.build(app)?;
    Ok(())
}
