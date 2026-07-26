<script module lang="ts">
  // MODULE script, not the instance script: a type has to be exported at module
  // scope to be importable as `import Sidebar, { type View } from "..."`.
  // Declaring it in the instance <script> below would not export it.
  export type View = "projects" | "engines";
</script>

<script lang="ts">
  import type { EngineEntry } from "$lib/api";
  // Exactly two items on purpose. Nav pointing at features that do not exist
  // makes a launcher feel like a mockup of itself; adding a third later is one
  // entry in this array, not a re-layout.
  let { view, engine, onNavigate }:
    { view: View; engine: EngineEntry | null; onNavigate: (v: View) => void } = $props();

  const items: { id: View; label: string; glyph: string }[] = [
    { id: "projects", label: "Projects", glyph: "▣" },
    { id: "engines", label: "Engines", glyph: "⚙" },
  ];
</script>

<aside class="side">
  <nav>
    {#each items as it (it.id)}
      <!-- aria-current="true", not "page": these are buttons switching an
           in-app view, not links to documents, so the generic boolean form is
           the accurate one. EngineRow uses the same value for the same reason. -->
      <button class="nav" type="button" class:on={view === it.id}
              onclick={() => onNavigate(it.id)}
              aria-current={view === it.id ? "true" : undefined}>
        <span class="g" aria-hidden="true">{it.glyph}</span>{it.label}
      </button>
    {/each}
  </nav>

  <div class="eng">
    <div class="label">Active engine</div>
    {#if engine}
      <div class="v"><span class="dot" aria-hidden="true"></span>{engine.build}</div>
      <code>abi {engine.engineAbi}</code>
    {:else}
      <div class="v none">None registered</div>
    {/if}
  </div>
</aside>

<style>
  .side { width: 198px; flex: none; background: var(--surface-2);
          border-right: 1px solid var(--border-soft);
          padding: 10px 11px 14px; display: flex; flex-direction: column; }
  nav { display: flex; flex-direction: column; gap: 2px; }
  .nav { display: flex; align-items: center; gap: 10px; padding: 8px 10px;
         border-radius: 6px; font: inherit; font-size: 12.5px; text-align: left;
         background: none; border: 0; color: var(--text-muted); cursor: default;
         transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .nav:hover:not(.on) { background: rgba(255, 255, 255, .04); color: var(--text); }
  .nav.on { background: color-mix(in srgb, var(--gold) 12%, transparent); color: #f7dda0;
            box-shadow: inset 2px 0 0 var(--gold); }
  .g { width: 14px; text-align: center; opacity: .8; }

  .eng { margin-top: auto; background: var(--surface); border: 1px solid var(--border-soft);
         border-radius: var(--r-panel); padding: 10px 11px; }
  .v { font-size: 12px; margin-top: 5px; display: flex; align-items: center; gap: 6px; }
  /* --text-dim, NOT --warn: in this palette --warn and --gold are the same hex,
     and gold means "act" here. Rendering an absent engine in the action colour
     would say the opposite of what it means. Absent state reads as inert. */
  .v.none { color: var(--text-dim); }
  .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--ok); flex: none; }
  code { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); }
</style>
