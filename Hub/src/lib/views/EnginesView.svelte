<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import EmptyState from "$lib/components/EmptyState.svelte";
  import EngineRow from "$lib/components/EngineRow.svelte";
  import type { EngineEntry } from "$lib/api";

  let { engines, selected, suggestion, busy, onRegister, onRegisterPath, onSelect, onForget }:
    {
      engines: EngineEntry[]; selected: EngineEntry | null; suggestion: EngineEntry | null;
      busy: boolean; onRegister: () => void; onRegisterPath: (path: string) => void;
      onSelect: (e: EngineEntry) => void; onForget: (e: EngineEntry) => void;
    } = $props();
</script>

<header class="top">
  <div>
    <h2 class="display view-title">Engines</h2>
    <p class="view-sub">
      {engines.length} registered{#if selected} &middot; using {selected.build}{/if}
    </p>
  </div>
  <Button variant="primary" disabled={busy} onclick={onRegister}>Register engine</Button>
</header>

{#if engines.length === 0}
  <EmptyState title="No engine registered"
              body="The Hub launches projects with an engine you register. Point it at a folder containing ArcaneEditor.exe.">
    {#if suggestion}
      <!-- {@const} binds the narrowed value so TypeScript does not have to
           re-narrow a reactive prop inside the callback closure below. -->
      {@const s = suggestion}
      <Button variant="primary" disabled={busy} onclick={() => onRegisterPath(s.path)}>
        Found one nearby &mdash; register {s.build}
      </Button>
    {/if}
  </EmptyState>
{:else}
  {#each engines as e (e.id)}
    <EngineRow engine={e} selected={selected?.id === e.id} {busy}
               onSelect={() => onSelect(e)} onForget={() => onForget(e)} />
  {/each}
  <p class="hint">The selected engine is the default: it launches any project that
    has not been given its own. Change a project's engine from its card.</p>
{/if}

<style>
  .top { display: flex; align-items: flex-end; justify-content: space-between;
         gap: 16px; margin-bottom: 15px; }
  .hint { font-size: 12.5px; color: var(--text-dim); margin: 14px 0 0; }
</style>
