<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import Dropdown, { type DropdownItem } from "$lib/components/Dropdown.svelte";
  import EmptyState from "$lib/components/EmptyState.svelte";
  import ProjectCard from "$lib/components/ProjectCard.svelte";
  import ProjectRow from "$lib/components/ProjectRow.svelte";
  import type { ProjectActions } from "$lib/components/ProjectMenu.svelte";
  import Icon, { type IconName } from "$lib/components/Icon.svelte";
  import { filterProjects, isCompatible, normalisePath, resolveEngine, sortProjects,
           type ProjectView, type SortKey } from "$lib/format";
  import type { EngineEntry, RecentProject } from "$lib/api";

  // Both dialogs are owned by +page.svelte, not by this view: they are
  // app-level overlays that have to sit above the error banner, and +page is
  // the only stateful file by design.
  let { recents, engines, defaultEngine, busy, running, covers, layout, confirmDelete,
        sort, sortDesc, actions, onOpen, onScan, onNew, onLayout, onSort }:
    {
      recents: RecentProject[]; engines: EngineEntry[];
      defaultEngine: EngineEntry | null; busy: boolean;
      /** Keys (format.normalisePath) of projects with a live editor. */
      running: Set<string>;
      /** Cover data URLs by recorded path; absent = the card's monogram. */
      covers: Record<string, string>;
      layout: ProjectView;
      /** Passed straight through to the action menu, which labels Delete by it. */
      confirmDelete: boolean;
      /** Active sort column + direction, persisted in settings like `layout`. */
      sort: SortKey; sortDesc: boolean;
      /** The whole per-project vocabulary, forwarded untouched to card/row/menu. */
      actions: ProjectActions;
      /** View-level actions stay individual props: they are not per-project. */
      onOpen: () => void; onScan: () => void; onNew: () => void;
      onLayout: (v: ProjectView) => void;
      /** A column was clicked/picked; +page owns the flip-or-switch rule. */
      onSort: (k: SortKey) => void;
    } = $props();

  // Both layouts take the SAME props, so switching is a component swap rather
  // than two parallel call sites that can drift apart.
  const layouts: { id: ProjectView; icon: IconName; label: string }[] = [
    { id: "grid", icon: "grid", label: "Tiles" },
    { id: "list", icon: "list", label: "List" },
  ];

  let query = $state("");

  // The Add dropdown (Unity Hub's grammar: "Add v" opens a menu, "+ New
  // project" acts). Disabled only while busy, because Scan needs no engine;
  // the engine requirement gates the one ITEM that launches.
  const addItems = $derived<DropdownItem[]>([
    {
      label: "Add project…",
      disabled: !defaultEngine,
      title: defaultEngine ? "Pick a project's .arcproj file" : "Register an engine first",
      onselect: onOpen,
    },
    {
      label: "Scan folder…",
      title: "Find every project under a folder and add them all",
      onselect: onScan,
    },
  ]);

  // Compatibility is now per card: each project is judged against the engine
  // that will actually open IT, not against one global selection.
  const resolvedFor = (p: RecentProject) =>
    resolveEngine(p.engineId, engines, defaultEngine);

  // The four sortable columns. The list layout renders them as clickable
  // headers; the grid, which has no columns, gets the same choices as a
  // compact picker in the meta row -- one sort, two controls.
  const COLS: { key: SortKey; label: string }[] = [
    { key: "name", label: "Name" },
    { key: "opened", label: "Opened" },
    { key: "engine", label: "Engine" },
    { key: "abi", label: "ABI" },
  ];

  // The grid's sort menu rides the same Dropdown as the Add button -- the
  // native <select> it replaces could only be color-scheme'd dark, never
  // styled to match. Picking the ACTIVE column flips the direction, the same
  // rule as clicking an active list header (onSort with the active key IS
  // the flip, per nextSort); the dirbtn beside it stays as the explicit
  // control.
  const sortItems = $derived<DropdownItem[]>(
    COLS.map((c) => ({ label: c.label, active: sort === c.key, onselect: () => onSort(c.key) })),
  );

  // Sorting by engine orders by what the card actually SAYS -- the resolved
  // build label -- not by some internal id the user never sees.
  const engineLabelOf = (p: RecentProject) => resolvedFor(p).engine?.build ?? "";
  const shown = $derived(
    sortProjects(filterProjects(recents, query), sort, sortDesc, engineLabelOf),
  );
  const incompatible = $derived(
    recents.filter((p) => {
      const r = resolveEngine(p.engineId, engines, defaultEngine);
      // A missing project is counted under `missing`, not here: its recorded
      // ABI came from a manifest that is no longer there to disagree with.
      return !p.missing && !isCompatible(p.engineAbi, r.engine?.engineAbi ?? null);
    }).length,
  );
  const missing = $derived(recents.filter((p) => p.missing).length);
</script>

<!-- Title, search and actions on ONE row. The count and the layout toggle drop
     to a slim second row: the count line runs long once projects are
     incompatible ("3 projects - 1 needs a different engine"), and at the 800px
     window minimum there is not room for it beside the search. -->
<header class="top">
  <h2 class="display view-title">Projects</h2>
  <!-- The magnifier keeps the field self-identifying once typing has
       replaced the placeholder; span-wrapped so the icon positions without
       reaching into the Icon component's scope. -->
  <div class="searchwrap">
    <span class="glass" aria-hidden="true"><Icon name="search" size={14} /></span>
    <input class="search" bind:value={query} placeholder="Search projects" spellcheck="false" />
  </div>
  <div class="acts">
    <!-- "Add", not "Open...": both routes put an EXISTING project into the
         list, which is what the word means in this kind of launcher. -->
    <Dropdown items={addItems} ariaLabel="Add projects">
      {#snippet trigger(t)}
        <Button disabled={busy} onclick={t.toggle} {...t.aria}
                title="Add existing projects to the list">
          <span class="lbl">Add<Icon name="chevron-down" size={14} /></span>
        </Button>
      {/snippet}
    </Dropdown>
    <Button variant="primary" disabled={busy || !defaultEngine} onclick={onNew}>
      <span class="lbl"><Icon name="plus" size={15} />New project</span>
    </Button>
  </div>
</header>

<div class="meta">
  <p class="view-sub">
    {recents.length} {recents.length === 1 ? "project" : "projects"}
    {#if incompatible > 0}&middot; {incompatible}
      {incompatible === 1 ? "needs" : "need"} a different engine{/if}
    {#if missing > 0}&middot; {missing} missing{/if}
  </p>
  <!-- The grid's sort control: the same four choices the list's headers
       offer, because the grid has no columns to click. The dropdown switches
       column (a new column starts at its natural direction); the button
       beside it states the current direction and flips it. -->
  {#if layout === "grid"}
    <div class="sorter">
      <Dropdown items={sortItems} ariaLabel="Sort projects by" width={150}>
        {#snippet trigger(t)}
          <button type="button" class="sorttrig" onclick={t.toggle} {...t.aria}
                  title="Sort projects by">
            {COLS.find((c) => c.key === sort)?.label}<Icon name="chevron-down" size={12} />
          </button>
        {/snippet}
      </Dropdown>
      <button type="button" class="dirbtn" onclick={() => onSort(sort)}
              title={sortDesc ? "Sorted descending — click for ascending"
                             : "Sorted ascending — click for descending"}
              aria-label={sortDesc ? "Sort ascending" : "Sort descending"}>
        <Icon name={sortDesc ? "arrow-down" : "arrow-up"} size={12} />
      </button>
    </div>
  {/if}
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
  <!-- Column headers for the list layout, on the same track list as the rows.
       Real buttons now that they sort -- each carries its own "Sort by" label,
       so the old aria-hidden (added when these were decorative text) is gone
       with the decoration. The leading and trailing spans hold the star and
       menu tracks. -->
  {#if layout === "list"}
    <div class="cols">
      <span></span>
      {#each COLS as c (c.key)}
        <button type="button" class="col" class:on={sort === c.key}
                onclick={() => onSort(c.key)}
                title="Sort by {c.label}" aria-label="Sort by {c.label}">
          {c.label}{#if sort === c.key}<span class="dir"
            aria-hidden="true"><Icon name={sortDesc ? "arrow-down" : "arrow-up"}
                                     size={10} /></span>{/if}
        </button>
      {/each}
      <span></span>
    </div>
  {/if}

  <div class:grid={layout === "grid"} class:list={layout === "list"}>
    {#each shown as p (p.path)}
      {@const r = resolvedFor(p)}
      {@const shared = {
        project: p,
        engineAbi: r.engine?.engineAbi ?? null,
        compatible: isCompatible(p.engineAbi, r.engine?.engineAbi ?? null),
        engineLabel: r.engine ? r.engine.build : "none registered",
        pinned: r.pinned,
        dangling: r.dangling,
        running: running.has(normalisePath(p.path)),
        disabled: busy || !r.engine,
        confirmDelete,
        actions,
      }}
      {#if layout === "grid"}
        <ProjectCard {...shared} cover={covers[p.path]} />
      {:else}
        <ProjectRow {...shared} />
      {/if}
    {/each}
  </div>
{/if}

<style>
  .top { display: flex; align-items: center; gap: 12px; }
  .acts { display: flex; gap: 8px; flex: none; }

  /* Icon + label inside a Button: the icon is display:block, so without a
     flex wrapper it would break the button's inline flow. */
  .lbl { display: flex; align-items: center; gap: 6px; }

  /* margin-left:auto pushes the search and the buttons to the right as one
     group, leaving the title hard left. flex:1 with a cap lets the field take
     the slack at 1024px without stretching absurdly on a maximised window,
     and min-width:0 lets it give that slack back at the 800px minimum. The
     wrapper carries the layout so the magnifier can anchor inside the field. */
  .searchwrap { flex: 1; min-width: 0; max-width: 320px; margin-left: auto;
                position: relative; }
  .glass { position: absolute; left: 11px; top: 50%; translate: 0 -50%;
           display: grid; place-items: center; color: var(--text-dim);
           pointer-events: none; }
  .search { width: 100%; box-sizing: border-box; padding-left: 32px; }

  .meta { display: flex; align-items: center; gap: 12px; margin: 10px 0 14px; }
  /* Beats the global .view-sub margin: inside this row the spacing is the
     row's job, not the paragraph's. */
  .meta .view-sub { margin: 0; }
  .layouts { margin-left: auto; }

  /* 7px vertical, matching Button's slimmed padding, so the search and the
     buttons beside it share one control height. */
  input { background: var(--well); border: 1px solid var(--border);
          border-radius: var(--r-btn); color: var(--text); font: inherit;
          font-size: 13px; padding: 7px 12px; user-select: text; cursor: text; }
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

  /* Same tracks, gap and horizontal padding as a ProjectRow, so a label sits
     over its column. Uses the shared tokens rather than repeating the values. */
  .cols { display: grid; grid-template-columns: var(--cols-project);
          gap: var(--gap-project); align-items: center; padding: 0 12px 8px;
          border-bottom: 1px solid var(--border-soft); }
  /* The header buttons keep the micro-caps the labels always had; being
     buttons adds only the hover brightening and the direction mark. Negative
     margin lets the hit area overhang the track without indenting the text,
     the same trick the row's engine chip uses. */
  .col { display: flex; align-items: center; gap: 5px; min-width: 0;
         margin: 0 -6px; padding: 2px 6px; background: none; border: 0;
         border-radius: 4px; font: inherit; font-size: 10.5px;
         letter-spacing: .13em; text-transform: uppercase;
         color: var(--text-dim); cursor: default; text-align: left;
         overflow: hidden; white-space: nowrap;
         transition: color var(--dur) var(--ease); }
  .col:hover { color: var(--text-muted); }
  .col.on { color: var(--text); }
  .dir { display: inline-flex; }

  /* ONE explicit height, shared with .layouts below: the sorter only exists
     in the grid view, and when its natural height exceeded the toggle's the
     whole meta row grew, so switching views nudged the page (user report
     2026-07-29). Pinning both pills to the same box means the row's tallest
     child never changes. */
  .sorter, .layouts { height: 30px; box-sizing: border-box; }

  .sorter { display: flex; align-items: center; gap: 2px; padding: 2px;
            background: var(--well); border: 1px solid var(--border);
            border-radius: var(--r-btn); }
  /* The old native <select> here needed a color-scheme hack just to keep its
     OS popup dark; the Dropdown popover is ours to style, so the trigger is
     a plain pill-resident button. */
  .sorttrig { display: flex; align-items: center; gap: 4px; background: none;
              border: 0; border-radius: 5px; color: var(--text-muted);
              font: inherit; font-size: 12px; padding: 0 6px; height: 24px;
              cursor: default;
              transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .sorttrig:hover, .sorttrig[aria-expanded="true"] {
    color: var(--text); background: rgba(255, 255, 255, .05); }
  .dirbtn { display: grid; place-items: center; width: 26px; height: 24px;
            background: none; border: 0; border-radius: 5px; font-size: 9px;
            color: var(--text-dim); cursor: default;
            transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .dirbtn:hover { color: var(--text); background: rgba(255, 255, 255, .05); }

  /* Separating adjacent rows is the list's job, not a row's: a row cannot see
     whether it has a neighbour, and `+` between two neighbours leaves the last
     one without a trailing edge for free. :global because the children are
     ProjectRow component roots, outside this file's scope. */
  .list > :global(* + *) { border-top: 1px solid var(--border-soft); }

  /* auto-fill, NOT repeat(3, 1fr): the window minimum is 800px wide. 230px
     tracks -- 190 originally, then 210 for the larger type, now 230 because a
     tile carries the folder path as well. Still three columns at the default
     window size, so this costs no density. */
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(230px, 1fr));
          gap: 12px; }
</style>
