<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import EmptyState from "$lib/components/EmptyState.svelte";
  import ProjectCard from "$lib/components/ProjectCard.svelte";
  import ProjectRow from "$lib/components/ProjectRow.svelte";
  import Icon, { type IconName } from "$lib/components/Icon.svelte";
  import { filterProjects, isCompatible, resolveEngine, type ProjectView } from "$lib/format";
  import type { EngineEntry, RecentProject } from "$lib/api";

  // Both dialogs are owned by +page.svelte, not by this view: they are
  // app-level overlays that have to sit above the error banner, and +page is
  // the only stateful file by design.
  let { recents, engines, defaultEngine, busy, layout,
        onLaunch, onForget, onOpen, onNew, onChangeEngine, onLayout }:
    {
      recents: RecentProject[]; engines: EngineEntry[];
      defaultEngine: EngineEntry | null; busy: boolean;
      layout: ProjectView;
      onLaunch: (p: RecentProject) => void; onForget: (p: RecentProject) => void;
      onOpen: () => void; onNew: () => void;
      onChangeEngine: (p: RecentProject) => void;
      onLayout: (v: ProjectView) => void;
    } = $props();

  // Both layouts take the SAME props, so switching is a component swap rather
  // than two parallel call sites that can drift apart.
  const layouts: { id: ProjectView; icon: IconName; label: string }[] = [
    { id: "grid", icon: "grid", label: "Tiles" },
    { id: "list", icon: "list", label: "List" },
  ];

  let query = $state("");

  // Compatibility is now per card: each project is judged against the engine
  // that will actually open IT, not against one global selection.
  const resolvedFor = (p: RecentProject) =>
    resolveEngine(p.engineId, engines, defaultEngine);

  const shown = $derived(filterProjects(recents, query));
  const incompatible = $derived(
    recents.filter((p) => {
      const r = resolveEngine(p.engineId, engines, defaultEngine);
      return !isCompatible(p.engineAbi, r.engine?.engineAbi ?? null);
    }).length,
  );
</script>

<!-- Title, search and actions on ONE row. The count and the layout toggle drop
     to a slim second row: the count line runs long once projects are
     incompatible ("3 projects - 1 needs a different engine"), and at the 800px
     window minimum there is not room for it beside the search. -->
<header class="top">
  <h2 class="display view-title">Projects</h2>
  <input class="search" bind:value={query} placeholder="Search projects" spellcheck="false" />
  <div class="acts">
    <!-- "Add", not "Open...": this puts an EXISTING project into the list,
         which is what the word means in this kind of launcher. It also happens
         to launch it, but the list is the lasting effect. -->
    <Button disabled={busy || !defaultEngine} onclick={onOpen}
            title="Add an existing project to the list">Add</Button>
    <Button variant="primary" disabled={busy || !defaultEngine} onclick={onNew}>New project</Button>
  </div>
</header>

<div class="meta">
  <p class="view-sub">
    {recents.length} {recents.length === 1 ? "project" : "projects"}
    {#if incompatible > 0}&middot; {incompatible} need a different engine{/if}
  </p>
  <!-- aria-current, matching Sidebar and EngineRow: this is "which of a
       mutually exclusive set is active", not an independently pressable
       toggle. One vocabulary for one meaning across the app. -->
  <div class="layouts" role="group" aria-label="Project layout">
    {#each layouts as l (l.id)}
      <button type="button" class:on={layout === l.id} onclick={() => onLayout(l.id)}
              aria-current={layout === l.id ? "true" : undefined}
              aria-label={l.label} title={l.label}><Icon name={l.icon} size={15} /></button>
    {/each}
  </div>
</div>

{#if recents.length === 0}
  <EmptyState title="No projects yet"
              body="Add a project's .arcproj file, or create one. Projects launch with the Hub's default engine unless you give one its own." />
{:else if shown.length === 0}
  <EmptyState title="No matches" body={`Nothing matches "${query}".`} />
{:else}
  <div class:grid={layout === "grid"}>
    {#each shown as p (p.path)}
      {@const r = resolvedFor(p)}
      {@const shared = {
        project: p,
        engineAbi: r.engine?.engineAbi ?? null,
        compatible: isCompatible(p.engineAbi, r.engine?.engineAbi ?? null),
        engineLabel: r.engine ? r.engine.build : "none registered",
        pinned: r.pinned,
        dangling: r.dangling,
        disabled: busy || !r.engine,
        onLaunch: () => onLaunch(p),
        onForget: () => onForget(p),
        onChangeEngine: () => onChangeEngine(p),
      }}
      {#if layout === "grid"}
        <ProjectCard {...shared} />
      {:else}
        <ProjectRow {...shared} />
      {/if}
    {/each}
  </div>
{/if}

<style>
  .top { display: flex; align-items: center; gap: 12px; }
  .acts { display: flex; gap: 8px; flex: none; }

  /* margin-left:auto pushes the search and the buttons to the right as one
     group, leaving the title hard left. flex:1 with a cap lets the field take
     the slack at 1024px without stretching absurdly on a maximised window,
     and min-width:0 lets it give that slack back at the 800px minimum. */
  .search { flex: 1; min-width: 0; max-width: 320px; margin-left: auto; }

  .meta { display: flex; align-items: center; gap: 12px; margin: 10px 0 14px; }
  /* Beats the global .view-sub margin: inside this row the spacing is the
     row's job, not the paragraph's. */
  .meta .view-sub { margin: 0; }
  .layouts { margin-left: auto; }

  input { background: var(--well); border: 1px solid var(--border);
          border-radius: var(--r-btn); color: var(--text); font: inherit;
          font-size: 13px; padding: 9px 12px; user-select: text; cursor: text; }
  input::placeholder { color: var(--text-dim); }
  input:focus { border-color: var(--accent-ring); outline: none; }

  .layouts { flex: none; display: flex; gap: 2px; padding: 2px;
             background: var(--well); border: 1px solid var(--border);
             border-radius: var(--r-btn); }
  .layouts button { width: 34px; display: grid; place-items: center; border: 0;
                    border-radius: 5px; background: none; color: var(--text-dim);
                    font: inherit; cursor: default;
                    transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .layouts button:hover:not(.on) { color: var(--text-muted);
                                   background: rgba(255, 255, 255, .05); }
  /* Same neutral active treatment as the sidebar's current nav item. */
  .layouts button.on { background: var(--surface-sel); color: var(--text); }

  /* auto-fill, NOT repeat(3, 1fr): the window minimum is 800px wide. 210px
     tracks, up from 190px, so a card fits the larger type without clipping. */
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(210px, 1fr));
          gap: 12px; }
</style>
