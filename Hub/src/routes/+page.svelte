<script lang="ts">
  import "$lib/theme.css";
  import { onMount } from "svelte";
  import { listen } from "@tauri-apps/api/event";
  import { open } from "@tauri-apps/plugin-dialog";
  import WindowChrome from "$lib/components/WindowChrome.svelte";
  import Sidebar, { type View } from "$lib/components/Sidebar.svelte";
  import type { ProjectActions } from "$lib/components/ProjectMenu.svelte";
  import type { EngineActions } from "$lib/views/EnginesView.svelte";
  import ProjectsView from "$lib/views/ProjectsView.svelte";
  import EnginesView from "$lib/views/EnginesView.svelte";
  import PackagesView from "$lib/views/PackagesView.svelte";
  import SettingsModal from "$lib/components/SettingsModal.svelte";
  import NewProjectModal from "$lib/components/NewProjectModal.svelte";
  import ProjectEngineModal from "$lib/components/ProjectEngineModal.svelte";
  import RenameProjectModal from "$lib/components/RenameProjectModal.svelte";
  import DeleteProjectModal from "$lib/components/DeleteProjectModal.svelte";
  import ProjectArgsModal from "$lib/components/ProjectArgsModal.svelte";
  import { nextSort, resolveEngine } from "$lib/format";
  import {
    loadState, refreshEngines, registerEngine, forgetEngine, deleteProject,
    forgetProject, clearRecents, runningProjects,
    openProject, createProject, suggestEngine, setProjectEngine,
    setProjectFavorite, setProjectArgs, revealProject, renameProject, relocateProject,
    loadSettings, saveSettings, defaultDialogDir,
    hubDataDir, revealHubDataDir, hubVersion,
    type HubState, type EngineEntry, type RecentProject, type Settings,
  } from "$lib/api";

  // NOT `state` -- see Global Constraints. A variable of that name makes
  // svelte2tsx read the `$state` rune as a legacy store-subscription, and
  // svelte-check reports 6 phantom errors while the app still runs fine.
  let hub = $state<HubState>({ recents: [], engines: [], warnings: [] });
  let selectedEngine = $state<EngineEntry | null>(null);
  let suggestion = $state<EngineEntry | null>(null);
  let error = $state("");
  // The re-entry gate: set the instant an action starts, checked at guard()'s
  // door, so a double-click cannot run two actions. NOT what the controls see.
  let busy = $state(false);
  // What the controls see. Most actions are a few milliseconds of IPC, and
  // flipping every disabled={busy} style for two frames dimmed the whole page
  // to 50% opacity and back -- a full-page flicker on every update. The
  // disabled visuals only engage when an action is actually SLOW (>150ms,
  // the spinner-delay rule); fast ones change nothing on screen.
  let busyUi = $state(false);
  let view = $state<View>("projects");
  let settings = $state<Settings>({
    defaultProjectDir: "", launchBehavior: "tray", projectView: "grid",
    projectSort: "opened", projectSortDesc: true, confirmDelete: true,
  });
  let hubDir = $state("");
  let version = $state("");
  // Normalised keys (format.normalisePath) of projects with a live editor.
  // Replaced wholesale on every running-changed event -- the payload is the
  // full set, so a missed event self-heals on the next transition.
  let running = $state<Set<string>>(new Set());
  // ONE modal at a time, as a discriminated union rather than six independent
  // flags. Owned here so a dialog renders above the error banner and a failed
  // action keeps its dialog up with the message inside it, not behind the
  // scrim. The union replaced the per-modal booleans/nullables: their modalUp
  // derived had to enumerate every one, and its own comment admitted "the
  // list has grown once already and a missed entry shows up as a message the
  // user can see but cannot read" -- with one value, a forgotten entry and
  // two-modals-at-once are unrepresentable rather than guarded against.
  type Modal =
    | { kind: "none" }
    | { kind: "new" }
    | { kind: "settings" }
    | { kind: "engine"; p: RecentProject }
    | { kind: "rename"; p: RecentProject }
    | { kind: "args"; p: RecentProject }
    | { kind: "delete"; p: RecentProject };
  let modal = $state<Modal>({ kind: "none" });
  const modalUp = $derived(modal.kind !== "none");
  const closeModal = () => (modal = { kind: "none" });
  // Load-time notices, ACCUMULATED rather than read straight off `hub`.
  // Recovery renames the bad file aside, so the very next load reports nothing
  // -- and since refresh() replaces `hub` after every action, rendering
  // hub.warnings directly made the notice vanish on the user's next click.
  let notices = $state<string[]>([]);
  // Deduplicated: the same fact arriving twice (a watchdog re-fire, a repeated
  // click) must not stack identical banners.
  const pushNotice = (m: string) => {
    if (!notices.includes(m)) notices.push(m);
  };

  // One adoption path for every HubState that arrives, whatever produced it.
  function adoptState(next: HubState) {
    hub = next;
    // `?? []` because this is an IPC boundary: the TS type is a promise about
    // what Rust sends, not something the compiler checks. It was briefly wrong
    // (a serde skip_serializing dropped the field), and a bare `for...of` over
    // undefined threw inside refresh -- taking the whole reload down with it,
    // including the engine selection, on every action that calls guard().
    for (const w of next.warnings ?? []) pushNotice(w);
    // Re-resolve the selection FROM the new list rather than keeping the old
    // object when its id still exists: entries are fresh copies, and a
    // re-probe or re-register may have changed abi/build -- holding the stale
    // copy would keep last week's facts alive in the sidebar and every
    // compatibility row derived from it.
    selectedEngine = hub.engines.find((e) => e.id === selectedEngine?.id)
      ?? hub.engines[0] ?? null;
  }

  async function refresh() {
    adoptState(await loadState());
    // Adjacency is a suggestion for the dev loop, never an assumption.
    suggestion = hub.engines.length === 0 ? await suggestEngine() : null;
  }

  onMount(async () => {
    await refresh();
    settings = await loadSettings();
    hubDir = await hubDataDir();
    version = await hubVersion();
    running = new Set(await runningProjects());
    // Land on Engines when there is nothing to launch with -- the one thing the
    // user must do first. Replaces the old force-showing engines section.
    if (hub.engines.length === 0) view = "engines";
    // AFTER the first paint, not before it: each probe is a process spawn, and
    // the cached data is right far more often than not -- but a dev-loop
    // engine rebuilt in place goes stale silently, and the compat display then
    // inverts (a project stamped by the CURRENT engine reads incompatible
    // against last week's cached abi). Re-probe and adopt whatever changed.
    adoptState(await refreshEngines());
  });

  // A SECOND, synchronous onMount, because the async one above cannot return a
  // cleanup. The launch watchdog reports an editor that died at boot (the
  // refuse-gate exits before any window, so a detached spawn is otherwise
  // click-then-nothing); it arrives as an event because the launch command
  // returns before the verdict exists. Rendered as a notice, not `error`:
  // it is an async fact about a process, not a failed Hub action.
  onMount(() => {
    const unFailed = listen<string>("launch-failed", (e) => pushNotice(e.payload));
    // The last tracked editor exited (the Hub may just have restored itself):
    // opened-times and missing flags deserve fresh eyes.
    const unIdle = listen("editors-idle", () => { refresh(); });
    // The Rust disk watcher noticed a listed path appear or vanish while the
    // Hub sat open. Adopt in place -- the rows are keyed by path, so the only
    // visible change is the row that actually changed.
    const unDisk = listen<HubState>("state-changed", (e) => adoptState(e.payload));
    // A quick-launch pick from the tray menu. Routed through launch() so the
    // tray goes through the SAME path a card click takes -- engine
    // resolution, probe, focus-existing, outcomes -- rather than a second
    // launch brain living Rust-side.
    const unTray = listen<string>("tray-launch", (e) => {
      const p = hub.recents.find((r) => r.path === e.payload);
      if (p) launch(p);
    });
    // Which projects have a live editor -- drives the running badge and the
    // launch tooltip's "focuses its window" phrasing.
    const unRunning = listen<string[]>("running-changed", (e) => {
      running = new Set(e.payload);
    });
    return () => {
      unFailed.then((f) => f());
      unIdle.then((f) => f());
      unDisk.then((f) => f());
      unTray.then((f) => f());
      unRunning.then((f) => f());
    };
  });

  async function guard(fn: () => Promise<unknown>) {
    // Dropped, not queued: the second click of an accidental double-click
    // lands here during the invisible first 150ms and must do nothing.
    if (busy) return;
    error = "";
    busy = true;
    const slow = setTimeout(() => (busyUi = true), 150);
    try { await fn(); await refresh(); }
    catch (e) { error = String(e); }
    finally {
      clearTimeout(slow);
      busy = false;
      busyUi = false;
    }
  }

  const addEngine = () => guard(async () => {
    const dir = await open({ directory: true, title: "Select an Arcane engine folder" });
    if (typeof dir === "string") await registerEngine(dir);
  });

  // Pick the .arcproj FILE, not the containing folder. Project::Open accepts
  // both, but a folder holding two manifests is ambiguous and it bails --
  // Project.cpp:37 returns nullopt, asserted by ProjectTest.cpp:52. Picking the
  // file names the project unambiguously, and it is what the editor's own Open
  // dialog already asks for (EditorApp.cpp:1309 filters "Arcane Project"/arcproj).
  const addProject = () => guard(async () => {
    const file = await open({
      title: "Select a project (.arcproj)",
      defaultPath: (await defaultDialogDir()) ?? undefined,
      filters: [{ name: "Arcane Project", extensions: ["arcproj"] }],
    });
    if (typeof file === "string" && selectedEngine) {
      await openProject(file, selectedEngine.path);
    }
  });

  // The greyed row's Locate…: pick the moved project's .arcproj and repoint the
  // entry at it. Same file filter as Add, because it IS the same question --
  // "which manifest is this project" -- asked about a row we already hold.
  const locate = (p: RecentProject) => guard(async () => {
    const file = await open({
      title: `Locate ${p.name} (.arcproj)`,
      defaultPath: (await defaultDialogDir()) ?? undefined,
      filters: [{ name: "Arcane Project", extensions: ["arcproj"] }],
    });
    if (typeof file === "string") await relocateProject(p.path, file);
  });

  // Shared by the New Project dialog and the Settings default-location field.
  const pickFolder = async (title: string): Promise<string | null> => {
    const dir = await open({
      directory: true,
      title,
      defaultPath: (await defaultDialogDir()) ?? undefined,
    });
    return typeof dir === "string" ? dir : null;
  };

  const launch = (p: RecentProject) => guard(async () => {
    // The project's own engine if it has a live pin, else the sidebar default.
    // Same rule the card renders, so what launches is what the card said.
    const { engine } = resolveEngine(p.engineId, hub.engines, selectedEngine);
    if (!engine) return;
    // Hiding-while-running happens RUST-side (open_project), which also owns
    // the restore -- the frontend no longer touches the window on launch.
    const out = await openProject(p.path, engine.path);
    // A vanished PROJECT needs no words here: guard()'s refresh flips the row
    // to its missing treatment, which already carries the explanation. The
    // engine case gets a notice because the Projects view has no row for the
    // engine to grey out -- the story lives on another screen.
    if (out.kind === "engineMissing") {
      pushNotice(`${engine.build} is no longer at ${engine.path}. See Engines.`);
    }
  });

  const chooseEngine = (engineId: string | null) => {
    if (modal.kind !== "engine") return;
    const p = modal.p;
    closeModal();
    return guard(() => setProjectEngine(p.path, engineId));
  };

  const makeProject = async (name: string, dir: string): Promise<boolean> => {
    let ok = false;
    await guard(async () => {
      if (!selectedEngine) return;
      // create_project returns the .arcproj it wrote, so the new project is
      // recorded exactly the way an Open-dialog project is.
      const manifest = await createProject(dir, name, selectedEngine.path);
      await openProject(manifest, selectedEngine.path);
      ok = true;
    });
    // Close only on SUCCESS: a failed create keeps the typed name and folder so
    // the message (rendered inside the dialog) can be acted on.
    if (ok) closeModal();
    return ok;
  };

  // Both of these follow makeProject's shape: the dialog closes only on
  // SUCCESS, so a failure keeps the typed value with the message beside it
  // instead of dropping the user back to a banner they cannot read.
  const doRename = async (newName: string): Promise<boolean> => {
    if (modal.kind !== "rename") return false;
    const p = modal.p;
    let ok = false;
    await guard(async () => { await renameProject(p.path, newName); ok = true; });
    if (ok) closeModal();
    return ok;
  };

  const doArgs = async (args: string): Promise<boolean> => {
    if (modal.kind !== "args") return false;
    const p = modal.p;
    let ok = false;
    await guard(async () => { await setProjectArgs(p.path, args); ok = true; });
    if (ok) closeModal();
    return ok;
  };

  // The menu item's entry point. Reads the setting at CLICK time rather than
  // caching it, so turning the confirmation off takes effect immediately.
  const askDelete = (p: RecentProject) => {
    if (settings.confirmDelete) {
      modal = { kind: "delete", p };
      return;
    }
    // Straight through, because the user asked for that. The safety net is
    // still there and is why this is offerable at all: delete_project moves the
    // folder to the Recycle Bin, so an unconfirmed mis-click is recoverable.
    guard(() => deleteProject(p.path));
  };

  const doDelete = async (): Promise<boolean> => {
    if (modal.kind !== "delete") return false;
    const p = modal.p;
    let ok = false;
    await guard(async () => { await deleteProject(p.path); ok = true; });
    if (ok) closeModal();
    return ok;
  };

  // Chromium's own menu -- Back, Reload, Save as, Inspect -- is webview
  // furniture that means nothing in a launcher, and it appeared everywhere a
  // project card was not. Suppressed app-wide so right-clicking one pixel
  // beside a card does not produce a completely different kind of menu.
  //
  // Text fields are the exception: cut/copy/paste and select-all there are
  // genuinely useful, and we offer no replacement for them.
  function onContextMenu(e: MouseEvent) {
    if ((e.target as HTMLElement).closest("input, textarea")) return;
    e.preventDefault();
  }

  const applySettings = (next: Settings) => guard(async () => {
    await saveSettings(next);
    // Re-read rather than trusting the local copy: Rust normalises the folder
    // string on save, so what is stored may differ from what was typed.
    settings = await loadSettings();
  });

  // The whole per-project vocabulary as ONE object (type owned by ProjectMenu,
  // which renders it). Declared after the handlers it references -- a const
  // object literal evaluates immediately, so naming them earlier would hit the
  // temporal dead zone.
  const projectActions: ProjectActions = {
    launch,
    toggleFavorite: (p) => guard(() => setProjectFavorite(p.path, !p.favorite)),
    changeEngine: (p) => (modal = { kind: "engine", p }),
    reveal: (p) => guard(() => revealProject(p.path)),
    rename: (p) => (modal = { kind: "rename", p }),
    args: (p) => (modal = { kind: "args", p }),
    forget: (p) => guard(() => forgetProject(p.path)),
    locate,
    delete: askDelete,
  };

  // Same shape for the Engines view -- the vocabulary object mirrors
  // ProjectActions, typed where the controls that call it are rendered.
  const engineActions: EngineActions = {
    register: addEngine,
    registerPath: (path) => guard(() => registerEngine(path)),
    select: (e) => (selectedEngine = e),
    forget: (e) => guard(() => forgetEngine(e.path)),
  };
</script>

<svelte:window oncontextmenu={onContextMenu} />

<div class="app">
  <WindowChrome />
  <div class="body">
    <Sidebar {view} engine={selectedEngine} settingsOpen={modal.kind === "settings"}
             onNavigate={(v) => (view = v)} onSettings={() => (modal = { kind: "settings" })} />
    <main>
      <!-- Suppressed while a dialog is up: the banner sits under the scrim,
           so the dialog renders the same message itself. -->
      {#if error && !modalUp}
        <p class="error" role="alert">{error}</p>
      {/if}
      <!-- Distinct from `error`: these are not a failed action but state the Hub
           had to recover on load, and staying quiet about it is how a reset
           project list becomes indistinguishable from having no projects. -->
      <!-- Unkeyed on purpose: keying by the string itself would throw on two
           identical messages, and the list is short and never reordered. -->
      {#each notices as w}
        <p class="warn" role="status">{w}</p>
      {/each}
      {#if view === "projects"}
        <ProjectsView recents={hub.recents} engines={hub.engines}
                      defaultEngine={selectedEngine} busy={busyUi} {running}
                      layout={settings.projectView}
                      confirmDelete={settings.confirmDelete}
                      sort={settings.projectSort} sortDesc={settings.projectSortDesc}
                      actions={projectActions}
                      onOpen={addProject} onNew={() => (modal = { kind: "new" })}
                      onLayout={(v) => applySettings({ ...settings, projectView: v })}
                      onSort={(k) => {
                        const n = nextSort(settings.projectSort, settings.projectSortDesc, k);
                        applySettings({ ...settings, projectSort: n.key, projectSortDesc: n.desc });
                      }} />
      {:else if view === "engines"}
        <EnginesView engines={hub.engines} selected={selectedEngine} {suggestion}
                     busy={busyUi} actions={engineActions} />
      {:else}
        <PackagesView />
      {/if}
    </main>
  </div>

  {#if modal.kind === "new"}
    <NewProjectModal busy={busyUi} {error} engine={selectedEngine}
                     defaultDir={settings.defaultProjectDir}
                     onBrowse={() => pickFolder("Where should the project live?")}
                     onCreate={makeProject} onCancel={closeModal} />
  {:else if modal.kind === "engine"}
    <ProjectEngineModal project={modal.p} engines={hub.engines}
                        defaultEngine={selectedEngine} busy={busyUi}
                        onChoose={chooseEngine} onCancel={closeModal} />
  {:else if modal.kind === "rename"}
    <RenameProjectModal project={modal.p} busy={busyUi} {error}
                        onRename={doRename} onCancel={closeModal} />
  {:else if modal.kind === "args"}
    <ProjectArgsModal project={modal.p} busy={busyUi} {error}
                      onSave={doArgs} onCancel={closeModal} />
  {:else if modal.kind === "delete"}
    <DeleteProjectModal project={modal.p} busy={busyUi} {error}
                        onDelete={doDelete} onCancel={closeModal} />
  {:else if modal.kind === "settings"}
    <SettingsModal {settings} busy={busyUi} {hubDir} {version} {error}
                   recentCount={hub.recents.length}
                   onSave={applySettings}
                   onBrowseDir={() => pickFolder("Default location for projects")}
                   onReveal={() => guard(revealHubDataDir)}
                   onClearRecents={() => guard(clearRecents)}
                   onClose={closeModal} />
  {/if}
</div>

<style>
  /* The window's own gradient lives here rather than on body so the rounded
     corners from --r-win clip it. */
  /* Flat neutral. The two radial washes that used to sit here -- a gold glow
     top-right and an explicitly blue one bottom-left -- were the largest
     coloured surface in the app and the main reason it read as tinted. */
  .app { display: flex; flex-direction: column; height: 100vh; overflow: hidden;
         border-radius: var(--r-win);
         background: linear-gradient(var(--bg-top), var(--bg-bottom)); }
  .body { flex: 1; display: flex; min-height: 0; }
  main { flex: 1; min-width: 0; overflow-y: auto; padding: 22px 28px 26px; }

  .error { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
           border: 1px solid color-mix(in srgb, var(--fail-accent) 40%, transparent);
           border-radius: var(--r-btn); color: var(--text); font-size: 13px;
           padding: 10px 13px; margin: 0 0 14px; user-select: text; }

  /* Neutral on purpose. The accent means "act" and coral means "won't open";
     a recovery notice is neither, so it gets the calm treatment rather than
     borrowing a colour that already says something else. */
  .warn { background: var(--surface-2); border: 1px solid var(--border);
          border-radius: var(--r-btn); color: var(--text-muted); font-size: 13px;
          padding: 10px 13px; margin: 0 0 14px; user-select: text; }
</style>
