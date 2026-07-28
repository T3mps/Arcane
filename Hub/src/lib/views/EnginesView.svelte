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

<!-- Same header shape as ProjectsView: title hard left, actions hard right on
     ONE row, with the count dropping to a slim second row. The two views are
     the same kind of surface and should not be laid out differently.
     Ghost-left / primary-right also matches the order there. -->
<header class="top">
  <h2 class="display view-title">Engines</h2>
  <div class="acts">
    <!-- "Locate", not "Register engine": it names what you DO (point the Hub at
         an engine already on disk), which is the distinction from the Install
         button beside it. The prop and the Rust command stay `register` -- that
         is still the effect. -->
    <Button disabled={busy} onclick={onRegister}
            title="Point the Hub at a folder containing ArcaneEditor.exe">Locate</Button>
    <!-- Wrapped in a titled span because a DISABLED button receives no mouse
         events, so its own title never renders a tooltip -- and a greyed
         control with no explanation is just a dead end. -->
    <span title="Downloading and installing engines is not built yet">
      <Button variant="primary" disabled>Install engine</Button>
    </span>
  </div>
</header>

<p class="view-sub meta">
  <!-- &nbsp; because Svelte trims the block's leading whitespace: a plain
       space here rendered as "registered· using". -->
  {engines.length} registered{#if selected}&nbsp;&middot; using {selected.build}{/if}
</p>

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
  /* Identical to ProjectsView's .top/.acts, less the search field: there is
     nothing between the title and the buttons here, so the group is pushed
     right by its own margin rather than by the field's. */
  .top { display: flex; align-items: center; gap: 12px; }
  .acts { display: flex; gap: 8px; flex: none; margin-left: auto; }

  /* Same rhythm as ProjectsView's second row. Beats the global .view-sub
     margin, which is written for a subtitle tucked under a title. */
  .meta { margin: 10px 0 14px; }

  .hint { font-size: 12.5px; color: var(--text-dim); margin: 14px 0 0; }
</style>
