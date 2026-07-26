<script lang="ts">
  import { coverFor } from "$lib/format";
  import { since, type RecentProject } from "$lib/api";

  let { project, compatible, engineAbi, disabled = false, onLaunch, onForget }:
    {
      project: RecentProject; compatible: boolean; engineAbi: number | null;
      disabled?: boolean; onLaunch: () => void; onForget: () => void;
    } = $props();

  const cover = $derived(coverFor(project.name, project.path));
  // The full sentence the flat list used to print as body text. Kept as the
  // accessible description so the explanation survives the move to cards.
  const why = $derived(
    compatible
      ? project.path
      : `Built against abi ${project.engineAbi}; the selected engine is abi ${engineAbi}. It will refuse to open.`,
  );
</script>

<div class="card" class:bad={!compatible}>
  <button class="hit" {disabled} onclick={onLaunch} title={why} aria-label={project.name}>
    <span class="cover" style="--a: {cover.angle}deg" aria-hidden="true">{cover.monogram}</span>
    <span class="cb">
      <span class="nm">{project.name}</span>
      <span class="mt">
        {#if compatible}
          <span>abi {project.engineAbi ? project.engineAbi : "?"}</span>
        {:else}
          <em class="badge">abi {project.engineAbi}</em>
        {/if}
        <span>{since(project.lastOpenedUtc)}</span>
      </span>
    </span>
  </button>
  <button class="x" onclick={onForget} aria-label="Remove {project.name} from the list"
          title="Remove from list">&#10005;</button>
</div>

<style>
  .card { position: relative; border: 1px solid var(--border-soft);
          border-radius: var(--r-panel); overflow: hidden; background: var(--surface);
          transition: border-color var(--dur) var(--ease); }
  .card:hover { border-color: #2d3750; }
  .hit { display: block; width: 100%; text-align: left; background: none;
         border: 0; padding: 0; font: inherit; color: inherit; cursor: default; }
  .hit:disabled { opacity: .5; }

  .cover { display: grid; place-items: center; height: 64px;
           font-family: var(--font-display); font-size: 21px; color: var(--gold);
           background: linear-gradient(var(--a), #463714, #191b26); }
  .cb { display: block; padding: 9px 11px; }
  .nm { display: block; font-size: 12.5px; font-weight: 600; color: var(--text);
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .mt { display: flex; justify-content: space-between; align-items: center;
        margin-top: 5px; font-family: var(--font-mono); font-size: 9.5px;
        color: var(--text-dim); }

  /* INERT: no gold anywhere, surfaces drop back, name recedes to muted (5.9:1). */
  .bad { background: var(--surface-2); border-color: var(--border); }
  .bad .cover { background: linear-gradient(var(--a), #20242e, #14171e); color: #4d566a; }
  .bad .nm { color: var(--text-muted); }
  /* Second, non-chromatic signal: a bordered badge (coral is 8.2:1 here). */
  .badge { font-style: normal; font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent); border-radius: 3px; padding: 0 4px; }

  .x { position: absolute; top: 6px; right: 6px; width: 20px; height: 20px;
       display: grid; place-items: center; border: 0; border-radius: 4px;
       background: rgba(8, 11, 18, .6); color: var(--text-dim); font-size: 10px;
       cursor: default; opacity: 0;
       transition: opacity var(--dur) var(--ease), color var(--dur) var(--ease); }
  .card:hover .x, .x:focus-visible { opacity: 1; }
  .x:hover { color: var(--fail); }
</style>
