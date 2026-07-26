<script lang="ts">
  import Button from "./Button.svelte";
  import type { EngineEntry } from "$lib/api";

  let { engine, selected, busy = false, onSelect, onForget }:
    {
      engine: EngineEntry; selected: boolean; busy?: boolean;
      onSelect: () => void; onForget: () => void;
    } = $props();
</script>

<div class="row" class:sel={selected}>
  <!-- aria-current, not aria-pressed: picking an engine is "this one is now
       current" in a mutually exclusive set, not a two-state toggle you can
       un-press. Same vocabulary as Sidebar's active nav item. -->
  <button class="pick" type="button" onclick={onSelect}
          aria-current={selected ? "true" : undefined}>
    <span class="nm">{engine.build}</span>
    <code class="path">{engine.path}</code>
  </button>
  <code class="abi">abi {engine.engineAbi}</code>
  <Button variant="danger" disabled={busy} onclick={onForget}>Remove</Button>
</div>

<style>
  .row { display: flex; align-items: center; gap: 12px; padding: 9px 11px;
         border: 1px solid var(--border-soft); border-radius: var(--r-panel);
         background: var(--surface); margin-bottom: 8px;
         transition: border-color var(--dur) var(--ease); }
  .row:hover { border-color: #2d3750; }
  /* Selection is the gold rail, same vocabulary as the sidebar's active item. */
  .sel { box-shadow: inset 2px 0 0 var(--gold); }
  .pick { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px;
          background: none; border: 0; padding: 0; text-align: left;
          font: inherit; color: inherit; cursor: default; }
  .nm { font-size: 13px; font-weight: 600; }
  .path { font-family: var(--font-mono); font-size: 10.5px; color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .abi { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); flex: none; }
</style>
