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
    <h2 class="display">Engines</h2>
    <p class="sub">
      {engines.length} registered{#if selected} &middot; using {selected.build}{/if}
    </p>
  </div>
  <Button variant="gold" disabled={busy} onclick={onRegister}>Register engine</Button>
</header>

{#if engines.length === 0}
  <EmptyState title="No engine registered"
              body="The Hub launches projects with an engine you register. Point it at a folder containing ArcaneEditor.exe.">
    {#if suggestion}
      <!-- {@const} binds the narrowed value so TypeScript does not have to
           re-narrow a reactive prop inside the callback closure below. -->
      {@const s = suggestion}
      <Button variant="gold" disabled={busy} onclick={() => onRegisterPath(s.path)}>
        Found one nearby &mdash; register {s.build}
      </Button>
    {/if}
  </EmptyState>
{:else}
  {#each engines as e (e.id)}
    <EngineRow engine={e} selected={selected?.id === e.id} {busy}
               onSelect={() => onSelect(e)} onForget={() => onForget(e)} />
  {/each}
  <p class="hint">The selected engine launches every project and decides which are compatible.</p>
{/if}

<style>
  .top { display: flex; align-items: flex-end; justify-content: space-between;
         gap: 16px; margin-bottom: 15px; }
  h2 { font-size: 22px; margin: 0; color: #f3f6fc;
       text-shadow: 0 0 20px color-mix(in srgb, var(--gold) 20%, transparent); }
  .sub { font-size: 11.5px; color: var(--text-muted); margin: 2px 0 0; }
  .hint { font-size: 11.5px; color: var(--text-dim); margin: 12px 0 0; }
</style>
