<script lang="ts">
  import { onMount } from "svelte";
  import { open } from "@tauri-apps/plugin-dialog";
  import {
    loadState, registerEngine, forgetEngine, forgetProject,
    openProject, createProject, suggestEngine, since,
    type HubState, type EngineEntry, type RecentProject,
  } from "$lib/api";

  // NOT `state`: a variable of that name makes svelte2tsx parse the `$state`
  // rune as a legacy store-subscription and svelte-check reports 6 phantom
  // errors. The runtime compiler is unaffected, so this only shows up on
  // typecheck.
  let hub = $state<HubState>({ recents: [], engines: [] });
  let selectedEngine = $state<EngineEntry | null>(null);
  let suggestion = $state<EngineEntry | null>(null);
  let error = $state("");
  let busy = $state(false);
  let showEngines = $state(false);
  let creating = $state(false);
  let newName = $state("");

  async function refresh() {
    hub = await loadState();
    if (!selectedEngine || !hub.engines.some((e) => e.id === selectedEngine!.id)) {
      selectedEngine = hub.engines[0] ?? null;
    }
    // Adjacency is a suggestion for the dev loop, never an assumption.
    suggestion = hub.engines.length === 0 ? await suggestEngine() : null;
  }

  onMount(refresh);

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

  const makeProject = () => guard(async () => {
    if (!selectedEngine || !newName.trim()) return;
    const dir = await open({ directory: true, title: "Where should the project live?" });
    if (typeof dir !== "string") return;
    const root = await createProject(dir, newName.trim(), selectedEngine.path);
    await openProject(root, selectedEngine.path);
    creating = false;
    newName = "";
  });
</script>

<div class="shell">
  <header>
    <h1>Arcane<span>Hub</span></h1>
    <button class="engine" class:none={!selectedEngine} onclick={() => (showEngines = !showEngines)}>
      {#if selectedEngine}
        <em>{selectedEngine.build}</em><code>abi {selectedEngine.engineAbi}</code>
      {:else}
        No engine registered
      {/if}
    </button>
  </header>

  {#if error}
    <p class="error" role="alert">{error}</p>
  {/if}

  {#if showEngines || hub.engines.length === 0}
    <section>
      <div class="bar">
        <h2>Engines</h2>
        <button onclick={addEngine} disabled={busy}>Register engine</button>
      </div>
      {#if hub.engines.length === 0}
        <p class="empty">
          The Hub launches projects with an engine you register. Point it at a folder
          containing <code>ArcaneEditor.exe</code>.
        </p>
        {#if suggestion}
          <button class="suggest" disabled={busy}
                  onclick={() => guard(() => registerEngine(suggestion!.path))}>
            Found one nearby &mdash; register {suggestion.build}
          </button>
        {/if}
      {:else}
        {#each hub.engines as e (e.id)}
          <div class="row engine-row" class:sel={selectedEngine?.id === e.id}>
            <button class="pick" onclick={() => (selectedEngine = e)}>
              <strong>{e.build}</strong>
              <code class="path">{e.path}</code>
            </button>
            <code class="abi">abi {e.engineAbi}</code>
            <button class="ghost" disabled={busy}
                    onclick={() => guard(() => forgetEngine(e.path))}>Remove</button>
          </div>
        {/each}
      {/if}
    </section>
  {/if}

  <section>
    <div class="bar">
      <h2>Projects</h2>
      <div class="actions">
        <button onclick={addProject} disabled={busy || !selectedEngine}>Open&hellip;</button>
        <button class="primary" onclick={() => (creating = !creating)}
                disabled={busy || !selectedEngine}>New project</button>
      </div>
    </div>

    {#if creating}
      <div class="new">
        <input bind:value={newName} placeholder="Project name" spellcheck="false"
               onkeydown={(e) => e.key === "Enter" && makeProject()} />
        <button class="primary" onclick={makeProject} disabled={busy || !newName.trim()}>
          Choose folder and create
        </button>
      </div>
    {/if}

    {#if hub.recents.length === 0}
      <p class="empty">No projects yet. Open a folder that contains a
        <code>.arcproj</code>, or create one.</p>
    {:else}
      {#each hub.recents as p (p.path)}
        {@const match = !selectedEngine || p.engineAbi === 0 || p.engineAbi === selectedEngine.engineAbi}
        <div class="row proj" class:mismatch={!match}>
          <span class="abibar" aria-hidden="true"></span>
          <button class="pick" onclick={() => launch(p)} disabled={busy || !selectedEngine}>
            <strong>{p.name}</strong>
            <code class="path">{p.path}</code>
          </button>
          <span class="meta">
            <code class="abi">{p.engineAbi ? `abi ${p.engineAbi}` : "abi ?"}</code>
            <em>{since(p.lastOpenedUtc)}</em>
          </span>
          <button class="ghost" disabled={busy}
                  onclick={() => guard(() => forgetProject(p.path))}>Remove</button>
        </div>
        {#if !match}
          <p class="warn">Built against abi {p.engineAbi}; the selected engine is
            abi {selectedEngine?.engineAbi}. It will refuse to open.</p>
        {/if}
      {/each}
    {/if}
  </section>
</div>

<style>
  /* Palette derived from the engine's own linear-HDR -> ACES tonemap ramp:
     blue-black shadow, violet mid, amber highlight. */
  :global(:root) {
    --void: #0b0d14;
    --panel: #141824;
    --line: #232a3d;
    --text: #c8cede;
    --dim: #6c7691;
    --arc: #7c6bf5;
    --ember: #e8a33d;
    --bad: #d9645f;
    --display: "Bahnschrift", "Segoe UI Variable Display", "Segoe UI", sans-serif;
    --body: "Segoe UI Variable Text", "Segoe UI", system-ui, sans-serif;
    --mono: "Cascadia Mono", Consolas, ui-monospace, monospace;
  }
  :global(body) { margin: 0; background: var(--void); color: var(--text); font-family: var(--body); }
  :global(*:focus-visible) { outline: 2px solid var(--arc); outline-offset: 2px; }

  .shell { max-width: 880px; margin: 0 auto; padding: 28px 24px 48px; }

  header { display: flex; align-items: center; justify-content: space-between;
           padding-bottom: 16px; border-bottom: 1px solid var(--line); }
  h1 { font-family: var(--display); font-weight: 600; font-size: 22px; letter-spacing: .14em;
       text-transform: uppercase; margin: 0; }
  h1 span { color: var(--dim); margin-left: .5ch; }

  .engine { background: none; border: 1px solid var(--line); border-radius: 3px; color: var(--text);
            font: inherit; padding: 6px 10px; cursor: pointer; display: flex; gap: 10px; align-items: baseline; }
  .engine:hover { border-color: var(--arc); }
  .engine.none { color: var(--ember); border-color: color-mix(in srgb, var(--ember) 45%, transparent); }
  .engine em { font-style: normal; }
  .engine code, .abi { font-family: var(--mono); font-size: 11px; color: var(--dim); }

  h2 { font-family: var(--display); font-size: 12px; letter-spacing: .18em; text-transform: uppercase;
       color: var(--dim); margin: 0; font-weight: 600; }
  .bar { display: flex; align-items: center; justify-content: space-between; margin: 28px 0 10px; }
  .actions { display: flex; gap: 8px; }

  button { font: inherit; font-size: 13px; cursor: pointer; border-radius: 3px;
           border: 1px solid var(--line); background: var(--panel); color: var(--text); padding: 6px 12px; }
  button:hover:not(:disabled) { border-color: var(--arc); }
  button:disabled { opacity: .4; cursor: default; }
  button.primary { background: var(--arc); border-color: var(--arc); color: #0b0d14; font-weight: 600; }
  button.ghost { background: none; border-color: transparent; color: var(--dim); }
  button.ghost:hover:not(:disabled) { color: var(--bad); border-color: transparent; }

  .row { display: flex; align-items: center; gap: 12px; padding: 10px 12px;
         border-bottom: 1px solid var(--line); position: relative; }
  .row:hover { background: var(--panel); }
  .pick { flex: 1; text-align: left; background: none; border: none; padding: 0;
          display: flex; flex-direction: column; gap: 3px; min-width: 0; }
  .pick strong { font-weight: 600; font-size: 14px; }
  .path { font-family: var(--mono); font-size: 11px; color: var(--dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .meta { display: flex; flex-direction: column; align-items: flex-end; gap: 3px; }
  .meta em { font-style: normal; font-size: 11px; color: var(--dim); }

  /* Signature: the ABI bar. Violet when this project will actually open with
     the selected engine, amber when it will not. Encodes the one fact the
     engine probe exists to establish. */
  .abibar { position: absolute; left: 0; top: 6px; bottom: 6px; width: 2px; background: var(--arc); }
  .proj.mismatch .abibar { background: var(--ember); }
  .engine-row.sel { background: var(--panel); }

  .warn { margin: -1px 0 0; padding: 6px 12px 10px; font-size: 12px; color: var(--ember);
          border-bottom: 1px solid var(--line); }
  .empty { color: var(--dim); font-size: 13px; line-height: 1.6; margin: 12px 0; }
  .empty code { font-family: var(--mono); font-size: 12px; }
  .suggest { margin-top: 4px; }

  .error { background: color-mix(in srgb, var(--bad) 14%, transparent);
           border: 1px solid color-mix(in srgb, var(--bad) 40%, transparent);
           color: var(--text); border-radius: 3px; padding: 9px 12px; font-size: 13px; margin: 16px 0 0; }

  .new { display: flex; gap: 8px; padding: 10px 0 4px; }
  .new input { flex: 1; background: var(--panel); border: 1px solid var(--line); border-radius: 3px;
               color: var(--text); font: inherit; font-size: 13px; padding: 6px 10px; }
  .new input:focus { border-color: var(--arc); outline: none; }

  @media (prefers-reduced-motion: no-preference) {
    .row { transition: background 120ms ease; }
    button { transition: border-color 120ms ease; }
  }
</style>
