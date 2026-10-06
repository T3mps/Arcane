# What Arcane's tooling can take from RenderDoc's tool design (architecture, workflows, polish)

**Date:** 2026-10-06

**Companion to:** `2026-10-06-renderdoc-lessons.md`. That doc covers GPU-debug features and the arc CLI wire protocol, and none of that is repeated here.

**Method.** Three read-only Opus passes over `D:\dev\_reference\renderdoc-1.46\qrenderdoc` and its docs, each compared with Arcane:
1. application architecture;
2. end-to-end user workflows;
3. polish and consistency.

Arcane was read at `D:\dev\starworks\Arcane-settings` (feat/settings) and, for S4's theme and keyboard work, `Arcane-settings-r` (feat/settings-s4 at ed413f38).

**Status.** Research only, nothing scheduled. Two findings were already fed into running settings-arc tasks; see "Already actioned" at the end.

## The contrast in one paragraph

RenderDoc is a read-mostly viewer of one immutable capture. It has no undo (`QRDInterface.h`: "there is no actual undo system"; it keeps only a `CaptureModifications` dirty bitmask), and its settings are a flat X-macro list with hand-wired dialog slots.

Arcane is already ahead on:
- undo (`CommandStack`);
- settings (the cvar-registry-driven window, with scopes, rungs, debounced save, a Restart bar and window-local undo);
- explained refusals (`RefusedFieldTooltip`, Keyboard-page conflict tooltips);
- theme contrast checks (RenderDoc has none);
- Hub file-association repair.

What RenderDoc does better comes from being a long-lived general tool:
- layouts that survive code changes;
- visible background work;
- every reference clickable everywhere;
- every table behaving the same;
- typed launch and failure verdicts;
- fix-it buttons;
- run presets;
- a live connection window;
- one number formatter;
- a user manual.

## Ranked adoption menu

Effort is S (under 2 days), M (under 1 week) or L (more).

### Tier 1: cheap, high value

| # | Idea | RenderDoc source | Arcane home | Effort |
|---|---|---|---|---|
| 1 | **Version-stamp the layout ini.** If the stored schema is older, rebuild the default dock layout and keep the non-dock sections (play mode, viewport, panel visibility). A deliberate default-layout change then becomes a version bump instead of a one-off upgrader like `UpgradeLegacyInspectorLayout`. This fixes the "imgui.ini vetoes authored UI changes" class at its root. | `Windows/MainWindow.cpp` `JSON_ID "rdocLayoutData"` / `JSON_VER`; `Code/QRDUtils.cpp` ~2375 rejects a wrong version; `LoadInitialLayout` falls back to the coded default | A custom `ImGuiSettingsHandler` `[ArcaneLayout][Schema] Version=N`, written like the `PanelVisibilitySettings*` handlers in `App/EditorApp.cpp`; `kLayoutSchemaVersion` in `Panels/DefaultLayout.hpp`; `EndDockSpace` resetLayout | S |
| 2 | **A home dock slot per panel,** so a new panel docks into an old layout instead of floating | `DockReference` (`LastUsedArea`, `MainToolArea`…) in `Code/Interface/QRDInterface.h`; `AddDockWindow(widget, ref, refWindow)` | A home field on `Panels/PanelRegistry.hpp` `PanelInfo` (band / right column / center), resolved against `BuildDefaultLayout`'s node ids | S-M |
| 3 | **Tooltips take the chord from the action, never a literal.** Add `TooltipWithChord(actionId, text)` and a description field on `EditorActionDesc`, so the Keyboard page, tooltips and a future command palette share one sentence. | `MainWindow.cpp` RegisterShortcut (one central map) | `Input/EditorActionTable.cpp`. Today `Settings/SettingsWindow.cpp:124,129` hard-codes "(Ctrl+Z)" / "(Ctrl+Y)", which goes stale after a rebind. **Already actioned** in S4-GATE. | S |
| 4 | **Typed launch verdict.** A host that exits before boot writes `{phase, reason, detail}` (to `Saved/`, or the arc descriptor's state), and the Hub reads it instead of decoding exit codes. | `renderdoc/core/target_control.cpp:125-140` (handshake); `LiveCapture.cpp:1517-1530, 739-777` (`RegisterAPI` + `supportMessage`) | `ArcaneHub/src-tauri/src/launch.rs:60-131, 340-385`. Exit codes 2 and 3 are each overloaded, the Hub's messages hedge, and its own comments record two misreports. | S |
| 5 | **Fix-it warnings.** Hidden until a check fails. Clicking one explains, lists the exact changes, asks consent, fixes, then re-checks. There is an "unfixable" branch too. | `qrenderdoc/Windows/Dialogs/CaptureDialog.cpp:197-201, 336-480` (Vulkan layer prompt) | `App/PluginLoadFailure.hpp` "built against ABI N" gets a [Rebuild Game Module] button. A Play-dropdown preflight on arcbuild `probe` exit 3. An optional fix-action id on `DiagLocator`, mapped to `MenuRequests`, so a Problems row gets one-click fixes. | S-M |
| 6 | **A global status line** with problem and unread counts, an error badge that flashes while unread, and double-click to open Problems. Errors stay visible even with Problems hidden. | `MainWindow::messageCheck` (`m_messageAlternate`), `statusDoubleClicked`; `MainWindow.cpp:1866-1888, 2281-2284` | Today there is no editor-wide status bar, only the Outliner count bar (`Panels/EditorPanels.cpp:2461`). Uses `DiagnosticStore::Count`. | S |
| 7 | **Named layout slots 1..N** plus "Save as default" | `MainWindow::GetLayoutPath` (`DefaultLayout.config`, `Layout%1.config`) | The Window menu (`Panels/EditorPanels.cpp:428`) has only Reset Layout | S |
| 8 | **One empty-state helper:** "No X. Do Y." | (Arcane's dim, centred, next-step text is already better than RenderDoc's blank panes; the gap is consistency) | 16 hand-written `TextDisabled` strings with mixed wording; `Widgets/EditorWidgets.hpp:126` `CenteredTextDisabled` is used only 3 times | S |
| 9 | **A Help menu:** docs, shortcuts, tips, About (ABI and commit), Report a problem. A "?" on each Problems row opens `docs/diag/<code>.md`. | `MainWindow.cpp:2990-2998`; `LiveCapture.cpp:1185-1188` | `Diagnostic.code` already exists "for a docs link to hang off" (`ArcaneCore/src/Arcane/Base/Diagnostics.hpp:672-675`) but is unused | S |
| 10 | **Recents:** numbered accelerators, "Clear history", and a "File not found, remove?" prompt | `MainWindow.cpp:1811-1828` | `Project/RecentProjects.hpp` (`RecentSelection` silently leaves out missing entries) | S |

### Tier 2: workflow features

| # | Idea | RenderDoc source | Arcane home | Effort |
|---|---|---|---|---|
| 11 | **Background work you can see.** A `BackgroundTasks` registry: `{label, progress or indeterminate, cancelFn?, start}` from cook, module build, shader compile and mesh import. A non-modal footer strip appears after a grace period, with Cancel where possible. It also feeds the witness idle/busy report. Add a shared progress/cancel helper and a tag-coalesced async queue, which generalises CookQueue's hand-rolled coalescing. | `MainWindow::messageCheck` (status progress after 1.5 s); `ShowProgressDialog(…, finished, update, cancel)` `QRDUtils.cpp:3353`; `ReplayManager::AsyncInvoke(cb, tag)` `ReplayManager.cpp:255`; `GUIInvoke` | `Project/CookQueue.hpp` (no progress or cancel), `Project/ModuleBuild.hpp` (Console "Build:" lines only), `Documents/PreviewStatus.cpp`, `EditorAppFrame.cpp:4321` | M |
| 12 | **Links in every text:** entity, asset and locator references are clickable in Console lines, Inspector fields and tooltips, all through one navigation setter. Use typed links (`AssetReferenceField`, `LocatorRoute`), not RenderDoc's parsing of IDs out of display strings. | `RichResourceTextInitialise` / `RichResourceTextMouseEvent` (`QRDUtils.cpp` ~605, ~1179-1240) | `Panels/LocatorRoute.hpp` serves Problems only; `InspectorSource::RestoreSelection` | M |
| 13 | **Saved run configurations (`.arclaunch`),** project- or user-local: topology, backend, scene, per-process args, env, `--set`. Plus an auto-saved "Last", a few recents in the Play dropdown, an optional auto start, and `arc launch --preset` reading the same file. The natural unit for client+server play. ServerLaunch's `--frames 0` rule stays enforced in code. | `CaptureDialog.cpp:95-140, 860-960`; `MainWindow.cpp:1396-1420, 1830-1850`; `docs/window/capture_attach.rst` (`.cap` files, `most_recent.cap`) | `App/PlayMode.hpp` (one int in the ini), `Project/RuntimeLaunch.hpp`, `ServerLaunch.hpp` (fixed argv); the Hub-only `ProjectArgsModal.svelte` args string | M |
| 14 | **Sessions panel** (Arcane's live connection window): one row per running host (kind, pid, state booted/ready/hung, backend, lease holder), with focus, screenshot, open log and quit. Children appear as they spawn. When a host exits it says why. Optional HUD line "arc: connected by <client>". **Slice now (S):** keep the `RuntimeLaunch` process handle; an early exit inside a boot watchdog publishes a Problems row with the exit code and log tail. | `qrenderdoc/Windows/Dialogs/LiveCapture.cpp` (`connectionClosed` 1317-1392); `renderdoc/core/core.cpp:1402-1432` (overlay line) | `RuntimeLaunch::SpawnDetached` is fire-and-forget; `ServerLaunch::ServerProcess` is held only for Stop. The full panel reads arc descriptors (D27). | S / M |
| 15 | **Base table conventions** through a `BeginEditorTable` / `EditorTableRow` pair: Ctrl+C copies aligned text; a shared right-click menu (Copy / Expand All / Collapse All plus a hook); a tooltip on cut-off text; saved expansion state | `Widgets/Extended/RDTreeView.{h,cpp}` (copySelection 457-516; contextMenuEvent 225-262), `RDTableView.h` | 8 `BeginTable` calls in 7 files, each with its own copy items (`ProblemsPanel.cpp:92`, `CrashReportDocument.cpp:298`); `EllipsisToWidth` is opt-in | M |
| 16 | **One number formatter** `Editor::Fmt::{Float, Length, Angle, Scale, Bytes, Count}` plus `editor.format.*` cvars (trim zeros, decimals, scientific cutoffs). Units come from reflection metadata, so drag fields and labels match. | `QRDUtils.h:277-322`, `.cpp:2843-2970` (`Formatter::Format`); `PersistentConfig.h:507-571` | `InspectorView.cpp:426` "%.3f", `SettingsEdit.cpp:80` "%g", `EditorPanels.cpp:1505/1509` "%.0f deg"/"%.2fx", `MeshDocument.cpp:794` "m"; only angles carry units (`InspectorMeta.hpp:55-70`) | M |
| 17 | **Theme coverage plus a colour-literal test:** every custom-drawn surface reads the palette | `Formatter::setPalette`, `IsDarkTheme`; `Styles/RDStyle/RDStyle.cpp` | Graph nodes, grid, pins and categories are constexpr dark (`ShaderEditorDocument.cpp:350-379`, `GraphCanvasStyle.hpp:74-88`, `AssetGraphPanel.cpp:1169`). **Already actioned** in S6-27. | M |
| 18 | **Theme tokens derived by rule:** a few seeds give hover, active, disabled and inactive-selection; presets may override. User `.arctheme` files get coherent variants for free. Add an unfocused-panel selection tone. | `RDStyle.cpp:167-268` (5 seeds; disabled = desaturated at half value) | `Widgets/EditorTheme.hpp:22-69`; each `data/EditorThemes/*.arctheme` spells out 33 tokens | M |
| 19 | **Crash reporting completes the loop:** an editable "what were you doing" note; Package (zip of envelope, dump, symbolized text, log, note, ABI, git SHA); File issue (a pre-filled `gh issue`, no upload server); Help > Report a problem; Help > Recent reports | `Dialogs/CrashDialog.cpp:140-250, 319-345`; `MainWindow.cpp:1423-1484, 3040-3065`; `CaptureContext.cpp:155-185` | `ArcaneCrashReporter/src/ReporterWindow.cpp:227, 636-642` (read-only details); `Documents/CrashReportDocument.hpp` | S-M |
| 20 | **OS drag-and-drop**, routed by type and run deferred so the source app is released: `.arcproj` switches project; `.arcscene` opens; `.arcdiag` opens the crash document; images and meshes import into the folder under the cursor. All of it sits behind the dirty-document guard. | `MainWindow.cpp:134, 3221-3251`; `PromptCloseCapture` 1127-1219 | None exists (no `WM_DROPFILES`/`IDropTarget` in the editor, Core or Client) | S-M |

### Tier 3: larger and later

| # | Idea | RenderDoc source | Arcane home | Effort |
|---|---|---|---|---|
| 21 | **Runtime panel and menu registration:** `RegisterPanel` and `RegisterMenuItem(menuPath, ctxMenuId, fn)` on the editor host, used by built-ins and the game module alike. Expose the SAME command surface to the arc CLI (D27). RenderDoc's real lesson is that the API windows use is the API scripts use. | `Code/Interface/Extensions.h` (`IExtensionManager` RegisterWindowMenu/PanelMenu/ContextMenu) | `PanelRegistry.hpp` is a closed constexpr array; `DocumentHost::RegisterFactory` and `RegisterSettingsPage` already exist | M-L |
| 22 | **A shelf of produced artifacts:** screenshots, golden diffs and `.arcdiag` files with thumbnails, rename (F2), a prompt for unsaved temporary ones, and "compare two", which opens the A/B TextureView (companion doc item 8) | `LiveCapture.cpp:778-930`; `docs/window/capture_connection.rst` | These live today only as folders or single documents | M |
| 23 | **A user manual:** `docs/manual/` with getting-started, how-to, one page per panel and settings page (embedding the real icons), and a "?" per panel keyed by `PanelRegistry` id. Cvar help states the default and range, and a settings reference page is generated from the registry. | `docs/` (getting_started, how (19 pages), window, behind_scenes); `docs/window/texture_viewer.rst`; `PersistentConfig.h` DOCUMENT "Defaults to N" | `docs/` has 122 specs and 159 plans and no manual; `Settings/SettingsRows.cpp:270-285` already shows help plus the cvar name | L (start the skeleton pre-1.0) |
| 24 | **Small items** | | | S each |

The small items in row 24:
- named icon aliases for meanings (Copy/Delete/Refresh) instead of 177 raw `ICON_LC_*` (`Code/Resources.h`; Arcane: `SeverityStyle.hpp` is the one meaning-to-icon map);
- numbered tips shown once on the start page (`Dialogs/TipsDialog.cpp:66-304`);
- an "Advanced" flat "modified only" settings view (`Dialogs/ConfigEditor.cpp`);
- open windows by name through a factory, and close windows a layout restore left orphaned (`setToolWindowCreateCallback`);
- restart-with-state through a temp session file. `Settings/EditorRestart.cpp` passes only `--project`; see `CaptureDialog.cpp:775-800`.

## Architecture notes

- **Selection.** RenderDoc pushes changes to `ICaptureViewer` observers and needs an `exclude` list to avoid re-entry. Arcane's epoch polling (`SelectionContext::Epoch`, `SelectionEdge::Observe`) is the right shape for immediate-mode UI. Borrow two things before multi-select (slice 3):
  - an explicit **selected vs effective** distinction: the clicked entity versus what the Inspector shows;
  - **one navigation setter** that every link goes through.
- **Responsiveness.** RenderDoc has three tiers: a short blocking wait (about 500 ms), modal progress for big operations, and a status-bar spinner after 1.5 s. Arcane must use only the non-modal tier, because a modal stall freezes the viewport and Play mode.
- **Settings and errors.** Arcane is already richer here. The gaps are items 6 and 12.

## Don't borrow (domain mismatch)

- **UI interaction:**
  - modal progress dialogs and blocking the UI thread;
  - the observer/exclude notification model;
  - one global "current state";
  - no undo plus a dirty bitmask.
- **Capture and replay concerns:**
  - resource-replacement overlays;
  - capture-time API options;
  - global process hooks and DLL injection;
  - the remote replay context (revisit only for the Linux port).
- **Infrastructure:**
  - the X-macro config;
  - unauthenticated fixed-port target control;
  - a vendor upload server, anonymous URLs and a remembered email (use zip plus `gh issue`);
  - the in-app updater and analytics (the Hub plus the ABI gate own versions).
- **Presentation and help:**
  - QStyle subclassing;
  - PNG icon sets with @2x variants;
  - CHM help;
  - automatic hex display;
  - parsing IDs out of display strings;
  - a random tip on every launch;
  - blank-pane empty states.
- **Automation:** the Python shell and extension manager (arc is the automation path).

## Already actioned (fed into running settings-arc tasks, 2026-10-06)

- **S6-27.** The graph node, grid, pin and category colours must go into the Dark, Light and HighContrast `.arctheme` presets, with Dark keeping today's values. Under Light and High Contrast, nodes are currently dark on a light canvas.
- **S4-GATE.** Settings-window tooltips must show the bound chord, not a "Ctrl+Z" literal, and the editor is grepped for other chord literals.
