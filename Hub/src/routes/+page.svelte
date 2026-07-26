<script lang="ts">
  import "$lib/theme.css";
  import { onMount } from "svelte";
  import { open } from "@tauri-apps/plugin-dialog";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import WindowChrome from "$lib/components/WindowChrome.svelte";
  import Sidebar, { type View } from "$lib/components/Sidebar.svelte";
  import ProjectsView from "$lib/views/ProjectsView.svelte";
  import EnginesView from "$lib/views/EnginesView.svelte";
  import PackagesView from "$lib/views/PackagesView.svelte";
  import SettingsView from "$lib/views/SettingsView.svelte";
  import NewProjectModal from "$lib/components/NewProjectModal.svelte";
  import ProjectEngineModal from "$lib/components/ProjectEngineModal.svelte";
  import { resolveEngine } from "$lib/format";
  import {
    loadState, registerEngine, forgetEngine, forgetProject, clearRecents,
    openProject, createProject, suggestEngine, setProjectEngine,
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
  let busy = $state(false);
  let view = $state<View>("projects");
  let settings = $state<Settings>({
    defaultProjectDir: "", closeAfterLaunch: false, projectView: "grid",
  });
  let hubDir = $state("");
  let version = $state("");
  // Owned here so the dialog can render above the error banner and so a failed
  // create keeps the dialog up with its message inside it, not behind the scrim.
  let creating = $state(false);
  // The project whose engine is being chosen, or null when the picker is shut.
  let choosingFor = $state<RecentProject | null>(null);
  // Load-time notices, ACCUMULATED rather than read straight off `hub`.
  // Recovery renames the bad file aside, so the very next load reports nothing
  // -- and since refresh() replaces `hub` after every action, rendering
  // hub.warnings directly made the notice vanish on the user's next click.
  let notices = $state<string[]>([]);

  async function refresh() {
    hub = await loadState();
    // `?? []` because this is an IPC boundary: the TS type is a promise about
    // what Rust sends, not something the compiler checks. It was briefly wrong
    // (a serde skip_serializing dropped the field), and a bare `for...of` over
    // undefined threw inside refresh -- taking the whole reload down with it,
    // including the engine selection, on every action that calls guard().
    for (const w of hub.warnings ?? []) {
      if (!notices.includes(w)) notices.push(w);
    }
    if (!selectedEngine || !hub.engines.some((e) => e.id === selectedEngine!.id)) {
      selectedEngine = hub.engines[0] ?? null;
    }
    // Adjacency is a suggestion for the dev loop, never an assumption.
    suggestion = hub.engines.length === 0 ? await suggestEngine() : null;
  }

  onMount(async () => {
    await refresh();
    settings = await loadSettings();
    hubDir = await hubDataDir();
    version = await hubVersion();
    // Land on Engines when there is nothing to launch with -- the one thing the
    // user must do first. Replaces the old force-showing engines section.
    if (hub.engines.length === 0) view = "engines";
  });

  async function guard(fn: () => Promise<unknown>) {
    error = "";
    busy = true;
    try { await fn(); await refresh(); }
    catch (e) { error = String(e); }
    finally { busy = false; }
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
    await openProject(p.path, engine.path);
    // Read at launch time, not cached at mount, so toggling the setting takes
    // effect without restarting the Hub.
    if (settings.closeAfterLaunch) await getCurrentWindow().close();
  });

  const chooseEngine = (engineId: string | null) => {
    const p = choosingFor;
    if (!p) return;
    choosingFor = null;
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
      if (settings.closeAfterLaunch) await getCurrentWindow().close();
    });
    // Close only on SUCCESS: a failed create keeps the typed name and folder so
    // the message (rendered inside the dialog) can be acted on.
    if (ok) creating = false;
    return ok;
  };

  const applySettings = (next: Settings) => guard(async () => {
    await saveSettings(next);
    // Re-read rather than trusting the local copy: Rust normalises the folder
    // string on save, so what is stored may differ from what was typed.
    settings = await loadSettings();
  });
</script>

<div class="app">
  <WindowChrome />
  <div class="body">
    <Sidebar {view} engine={selectedEngine} onNavigate={(v) => (view = v)} />
    <main>
      <!-- Suppressed while the dialog is up: the banner sits under the scrim,
           so the dialog renders the same message itself. -->
      {#if error && !creating}
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
                      defaultEngine={selectedEngine} {busy}
                      layout={settings.projectView}
                      onLaunch={launch} onForget={(p) => guard(() => forgetProject(p.path))}
                      onOpen={addProject} onNew={() => (creating = true)}
                      onChangeEngine={(p) => (choosingFor = p)}
                      onLayout={(v) => applySettings({ ...settings, projectView: v })} />
      {:else if view === "engines"}
        <EnginesView engines={hub.engines} selected={selectedEngine} {suggestion} {busy}
                     onRegister={addEngine}
                     onRegisterPath={(path) => guard(() => registerEngine(path))}
                     onSelect={(e) => (selectedEngine = e)}
                     onForget={(e) => guard(() => forgetEngine(e.path))} />
      {:else if view === "packages"}
        <PackagesView />
      {:else}
        <SettingsView {settings} {busy} {hubDir} {version}
                      recentCount={hub.recents.length}
                      onSave={applySettings}
                      onBrowseDir={() => pickFolder("Default location for projects")}
                      onReveal={() => guard(revealHubDataDir)}
                      onClearRecents={() => guard(clearRecents)} />
      {/if}
    </main>
  </div>

  {#if creating}
    <NewProjectModal {busy} {error} engine={selectedEngine}
                     defaultDir={settings.defaultProjectDir}
                     onBrowse={() => pickFolder("Where should the project live?")}
                     onCreate={makeProject} onCancel={() => (creating = false)} />
  {/if}

  {#if choosingFor}
    <ProjectEngineModal project={choosingFor} engines={hub.engines}
                        defaultEngine={selectedEngine} {busy}
                        onChoose={chooseEngine} onCancel={() => (choosingFor = null)} />
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
