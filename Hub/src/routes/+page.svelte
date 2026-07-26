<script lang="ts">
  import "$lib/theme.css";
  import { onMount } from "svelte";
  import { open } from "@tauri-apps/plugin-dialog";
  import WindowChrome from "$lib/components/WindowChrome.svelte";
  import Sidebar, { type View } from "$lib/components/Sidebar.svelte";
  import ProjectsView from "$lib/views/ProjectsView.svelte";
  import EnginesView from "$lib/views/EnginesView.svelte";
  import {
    loadState, registerEngine, forgetEngine, forgetProject,
    openProject, createProject, suggestEngine,
    type HubState, type EngineEntry, type RecentProject,
  } from "$lib/api";

  // NOT `state` -- see Global Constraints. A variable of that name makes
  // svelte2tsx read the `$state` rune as a legacy store-subscription, and
  // svelte-check reports 6 phantom errors while the app still runs fine.
  let hub = $state<HubState>({ recents: [], engines: [] });
  let selectedEngine = $state<EngineEntry | null>(null);
  let suggestion = $state<EngineEntry | null>(null);
  let error = $state("");
  let busy = $state(false);
  let view = $state<View>("projects");

  async function refresh() {
    hub = await loadState();
    if (!selectedEngine || !hub.engines.some((e) => e.id === selectedEngine!.id)) {
      selectedEngine = hub.engines[0] ?? null;
    }
    // Adjacency is a suggestion for the dev loop, never an assumption.
    suggestion = hub.engines.length === 0 ? await suggestEngine() : null;
  }

  onMount(async () => {
    await refresh();
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

  const addProject = () => guard(async () => {
    const dir = await open({ directory: true, title: "Select a project folder" });
    if (typeof dir === "string" && selectedEngine) {
      await openProject(dir, selectedEngine.path);
    }
  });

  const launch = (p: RecentProject) => guard(async () => {
    if (selectedEngine) await openProject(p.path, selectedEngine.path);
  });

  const makeProject = async (name: string): Promise<boolean> => {
    let ok = false;
    await guard(async () => {
      if (!selectedEngine) return;
      const dir = await open({ directory: true, title: "Where should the project live?" });
      if (typeof dir !== "string") return;   // cancelled: leave the panel open
      const root = await createProject(dir, name, selectedEngine.path);
      await openProject(root, selectedEngine.path);
      ok = true;
    });
    return ok;
  };
</script>

<div class="app">
  <WindowChrome />
  <div class="body">
    <Sidebar {view} engine={selectedEngine} onNavigate={(v) => (view = v)} />
    <main>
      {#if error}
        <p class="error" role="alert">{error}</p>
      {/if}
      {#if view === "projects"}
        <ProjectsView recents={hub.recents} engine={selectedEngine} {busy}
                      onLaunch={launch} onForget={(p) => guard(() => forgetProject(p.path))}
                      onOpen={addProject} onCreate={makeProject} />
      {:else}
        <EnginesView engines={hub.engines} selected={selectedEngine} {suggestion} {busy}
                     onRegister={addEngine}
                     onRegisterPath={(path) => guard(() => registerEngine(path))}
                     onSelect={(e) => (selectedEngine = e)}
                     onForget={(e) => guard(() => forgetEngine(e.path))} />
      {/if}
    </main>
  </div>
</div>

<style>
  /* The window's own gradient lives here rather than on body so the rounded
     corners from --r-win clip it. */
  .app { display: flex; flex-direction: column; height: 100vh; overflow: hidden;
         border-radius: var(--r-win);
         background:
           radial-gradient(760px 320px at 82% -14%, color-mix(in srgb, var(--gold) 7.5%, transparent), transparent 62%),
           radial-gradient(560px 260px at 4% 106%, rgba(80, 150, 200, .06), transparent 60%),
           linear-gradient(var(--bg-top), var(--bg-bottom)); }
  .body { flex: 1; display: flex; min-height: 0; }
  main { flex: 1; min-width: 0; overflow-y: auto; padding: 18px 24px 22px; }

  .error { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
           border: 1px solid color-mix(in srgb, var(--fail-accent) 40%, transparent);
           border-radius: var(--r-btn); color: var(--text); font-size: 12.5px;
           padding: 9px 12px; margin: 0 0 14px; user-select: text; }
</style>
