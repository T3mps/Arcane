<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import EmptyState from "$lib/components/EmptyState.svelte";
  import ProjectCard from "$lib/components/ProjectCard.svelte";
  import { filterProjects, isCompatible } from "$lib/format";
  import type { EngineEntry, RecentProject } from "$lib/api";

  let { recents, engine, busy, onLaunch, onForget, onOpen, onCreate }:
    {
      recents: RecentProject[]; engine: EngineEntry | null; busy: boolean;
      onLaunch: (p: RecentProject) => void; onForget: (p: RecentProject) => void;
      onOpen: () => void; onCreate: (name: string) => Promise<boolean>;
    } = $props();

  let query = $state("");
  let creating = $state(false);
  let newName = $state("");

  const engineAbi = $derived(engine ? engine.engineAbi : null);
  const shown = $derived(filterProjects(recents, query));
  const incompatible = $derived(
    recents.filter((p) => !isCompatible(p.engineAbi, engineAbi)).length,
  );

  async function submit() {
    if (!newName.trim()) return;
    // Only clear on SUCCESS: cancelling the folder picker must leave the panel
    // open with the typed name intact, which is how this behaved pre-arc.
    const ok = await onCreate(newName.trim());
    if (ok) {
      creating = false;
      newName = "";
    }
  }
</script>

<header class="top">
  <div>
    <h2 class="display">Projects</h2>
    <p class="sub">
      {recents.length} {recents.length === 1 ? "project" : "projects"}
      {#if incompatible > 0}&middot; {incompatible} need a different engine{/if}
    </p>
  </div>
  <div class="acts">
    <Button disabled={busy || !engine} onclick={onOpen}>Open&hellip;</Button>
    <Button variant="gold" disabled={busy || !engine}
            onclick={() => (creating = !creating)}>New project</Button>
  </div>
</header>

{#if creating}
  <div class="new">
    <input bind:value={newName} placeholder="Project name" spellcheck="false"
           onkeydown={(e) => e.key === "Enter" && submit()} />
    <Button variant="gold" disabled={busy || !newName.trim()} onclick={submit}>
      Choose folder and create
    </Button>
  </div>
{/if}

{#if recents.length === 0}
  <EmptyState title="No projects yet"
              body="Open a folder containing a .arcproj, or create one. The Hub launches it with the engine selected in the sidebar." />
{:else}
  <input class="search" bind:value={query} placeholder="Search projects" spellcheck="false" />
  {#if shown.length === 0}
    <EmptyState title="No matches" body={`Nothing matches "${query}".`} />
  {:else}
    <div class="grid">
      {#each shown as p (p.path)}
        <ProjectCard project={p} engineAbi={engineAbi}
                     compatible={isCompatible(p.engineAbi, engineAbi)}
                     disabled={busy || !engine}
                     onLaunch={() => onLaunch(p)} onForget={() => onForget(p)} />
      {/each}
    </div>
  {/if}
{/if}

<style>
  .top { display: flex; align-items: flex-end; justify-content: space-between;
         gap: 16px; margin-bottom: 15px; }
  h2 { font-size: 22px; margin: 0; color: #f3f6fc;
       text-shadow: 0 0 20px color-mix(in srgb, var(--gold) 20%, transparent); }
  .sub { font-size: 11.5px; color: var(--text-muted); margin: 2px 0 0; }
  .acts { display: flex; gap: 8px; flex: none; }

  .new { display: flex; gap: 8px; margin-bottom: 13px; }
  .new input { flex: 1; }
  input { background: var(--well); border: 1px solid var(--border);
          border-radius: var(--r-btn); color: var(--text); font: inherit;
          font-size: 12px; padding: 7px 11px; user-select: text; cursor: text; }
  input::placeholder { color: var(--text-dim); }
  input:focus { border-color: var(--gold); outline: none; }
  .search { width: 100%; margin-bottom: 13px; }

  /* auto-fill, NOT repeat(3, 1fr): the window minimum is 800px wide. */
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr));
          gap: 11px; }
</style>
