<script lang="ts">
  import { coverFor, engineChipText, engineChipTitle, compatibilityNote } from "$lib/format";
  import { since, type RecentProject } from "$lib/api";

  // IDENTICAL prop contract to ProjectCard, so ProjectsView can swap the two
  // without a second set of wiring. Shape follows EngineRow, which already
  // establishes what a row looks like in this app: a wide pick button carrying
  // name + path, then fixed-width meta, then the destructive action last.
  let { project, compatible, engineAbi, engineLabel, pinned, dangling,
        disabled = false, onLaunch, onForget, onChangeEngine }:
    {
      project: RecentProject; compatible: boolean; engineAbi: number | null;
      engineLabel: string; pinned: boolean; dangling: boolean;
      disabled?: boolean; onLaunch: () => void; onForget: () => void;
      onChangeEngine: () => void;
    } = $props();

  const cover = $derived(coverFor(project.name, project.path));
  const engineText = $derived(engineChipText(engineLabel, pinned, dangling));
  const engineTitle = $derived(engineChipTitle(engineLabel, pinned, dangling));
  const why = $derived(
    compatibilityNote(compatible, project.path, project.engineAbi, engineLabel, engineAbi),
  );
</script>

<div class="row" class:incompat={!compatible}>
  <button class="hit" type="button" {disabled} onclick={onLaunch}
          title={why} aria-label={project.name}>
    <span class="cover" style="--a: {cover.angle}deg" aria-hidden="true">{cover.monogram}</span>
    <span class="txt">
      <span class="nm">{project.name}</span>
      <span class="path">{project.path}</span>
    </span>
  </button>

  {#if compatible}
    <code class="abi">abi {project.engineAbi ? project.engineAbi : "?"}</code>
  {:else}
    <code class="abi badge">abi {project.engineAbi}</code>
  {/if}

  <!-- Sibling of .hit, not nested: an interactive control inside a button is
       invalid HTML and its clicks are not reliably delivered. -->
  <button class="eng" type="button" {disabled} onclick={onChangeEngine}
          class:pin={pinned} class:missing={dangling} title={engineTitle}
          aria-label="Engine for {project.name}: {engineText}">
    <!-- A CSS dot, not a glyph: hollow = following the default, filled =
         an explicit choice. Drawn rather than typed so it cannot depend on
         a symbol font and stays crisp at this size. -->
    <span class="mark" class:filled={pinned || dangling} aria-hidden="true"></span>
    <span class="lbl">{engineText}</span>
  </button>

  <span class="when">{since(project.lastOpenedUtc)}</span>

  <button class="x" type="button" {disabled} onclick={onForget}
          aria-label="Remove {project.name} from the list. Does not delete it from disk."
          title="Remove from this list &mdash; does not delete the project from disk">&#10005;</button>
</div>

<style>
  .row { display: flex; align-items: center; gap: 14px; padding: 12px 14px;
         border: 1px solid var(--border-soft); border-radius: var(--r-panel);
         background: var(--surface); margin-bottom: 7px;
         transition: border-color var(--dur) var(--ease); }
  .row:hover { border-color: var(--border-hover); }

  .hit { flex: 1; min-width: 0; display: flex; align-items: center; gap: 11px;
         background: none; border: 0; padding: 0; text-align: left;
         font: inherit; color: inherit; cursor: default; }
  .hit:disabled { opacity: .5; }

  /* Same gradient and monogram as the tile, at row scale, so a project is
     recognisable across both layouts. */
  .cover { flex: none; width: 38px; height: 38px; display: grid; place-items: center;
           border-radius: 6px; font-family: var(--font-display); font-size: 16px;
           color: var(--cover-ink);
           background: linear-gradient(var(--a), var(--cover-from), var(--cover-to)); }
  .txt { display: flex; flex-direction: column; gap: 1px; min-width: 0; }
  .nm { font-size: 14px; font-weight: 600; color: var(--text);
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .path { font-family: var(--font-mono); font-size: 11.5px; color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .abi { flex: none; font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); }
  .when { flex: none; font-family: var(--font-mono); font-size: 11px;
          color: var(--text-dim); width: 66px; text-align: right; }

  .eng { flex: none; max-width: 210px; display: flex; align-items: center; gap: 7px;
         background: none; border: 0; padding: 6px 8px; border-radius: 5px;
         font: inherit; font-size: 11.5px; color: var(--text-dim); cursor: default;
         transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .eng:hover:not(:disabled) { color: var(--text-muted); background: rgba(255, 255, 255, .04); }
  .eng:disabled { opacity: .5; }
  .mark { flex: none; width: 7px; height: 7px; border-radius: 50%;
          border: 1.5px solid currentColor; }
  .mark.filled { background: currentColor; }
  .lbl { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .eng.pin { color: var(--text-muted); }
  .eng.missing { color: var(--fail); }

  /* Same three-state logic as the tile: inert surfaces plus a coral badge, so
     the meaning does not change with the layout. */
  .row.incompat { background: var(--surface-2); border-color: var(--border); }
  .row.incompat .cover { background: linear-gradient(var(--a), var(--border-soft), var(--bg-bottom));
                          color: var(--text-dim); }
  .row.incompat .nm { color: var(--text-muted); }
  .row.incompat :focus-visible { outline-color: var(--fail); }
  .row.incompat .badge { font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent);
           border-radius: 3px; padding: 0 4px; }

  /* Always visible here, unlike the tile's hover-reveal: a row has room, and a
     control that appears only on hover is undiscoverable in a dense list. */
  .x { flex: none; width: 26px; height: 26px; display: grid; place-items: center;
       border: 0; border-radius: 4px; background: none; color: var(--text-dim);
       font-size: 11px; cursor: default;
       transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .x:hover:not(:disabled) { color: var(--fail); background: rgba(255, 255, 255, .05); }
  .x:disabled { opacity: .5; }
</style>
