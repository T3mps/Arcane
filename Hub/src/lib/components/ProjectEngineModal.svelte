<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import type { EngineEntry, RecentProject } from "$lib/api";

  let { project, engines, defaultEngine, busy, onChoose, onCancel }: {
    project: RecentProject;
    engines: EngineEntry[];
    defaultEngine: EngineEntry | null;
    busy: boolean;
    /** null = follow the Hub default. */
    onChoose: (engineId: string | null) => void;
    onCancel: () => void;
  } = $props();

  // "" is the radio value for "follow the default" -- a radio group cannot
  // carry a null value, and an empty string is never a real engine id.
  const DEFAULT = "";
  // Seeded once, deliberately: the dialog is mounted fresh per project
  // (`{#if choosingFor}`) and from then on `choice` is the user's edit, which a
  // re-render must not overwrite.
  // svelte-ignore state_referenced_locally
  let choice = $state(project.engineId ?? DEFAULT);

  const apply = () => onChoose(choice === DEFAULT ? null : choice);
</script>

<Modal title="Engine for {project.name}" onClose={onCancel}>
  <p class="lead">
    Projects follow the Hub's default engine unless they choose their own.
  </p>

  <div class="opts" role="radiogroup" aria-label="Engine for {project.name}">
    <label class="opt" class:on={choice === DEFAULT}>
      <input type="radio" bind:group={choice} value={DEFAULT} disabled={busy} />
      <span class="txt">
        <span class="nm">Use the default</span>
        <span class="sub">
          {#if defaultEngine}
            Currently {defaultEngine.build} &middot; abi {defaultEngine.engineAbi}.
            Follows the sidebar, so changing it moves this project too.
          {:else}
            No engine is registered yet.
          {/if}
        </span>
      </span>
    </label>

    {#each engines as e (e.id)}
      <label class="opt" class:on={choice === e.id}>
        <input type="radio" bind:group={choice} value={e.id} disabled={busy} />
        <span class="txt">
          <span class="nm">{e.build}</span>
          <span class="sub">abi {e.engineAbi} &middot; <code>{e.path}</code></span>
        </span>
      </label>
    {/each}
  </div>

  {#if engines.length === 0}
    <p class="lead">Register an engine first, on the Engines tab.</p>
  {/if}

  {#snippet footer()}
    <Button onclick={onCancel} disabled={busy}>Cancel</Button>
    <Button variant="primary" onclick={apply} disabled={busy}>Use this engine</Button>
  {/snippet}
</Modal>

<style>
  .lead { font-size: 13px; color: var(--text-dim); margin: 0; line-height: 1.55; }
  .opts { display: flex; flex-direction: column; gap: 6px; }

  .opt { display: flex; align-items: flex-start; gap: 11px; cursor: default;
         padding: 11px 13px; border: 1px solid var(--border-soft);
         border-radius: var(--r-btn); background: var(--surface-2);
         transition: border-color var(--dur) var(--ease),
                     background var(--dur) var(--ease); }
  .opt:hover { border-color: var(--border-hover); }
  /* Neutral, like every other selected state. The radio's own accent-color
     stays gold, so the chosen option still carries the accent -- on the
     control itself rather than across the whole row. */
  .opt.on { border-color: var(--border-hover); background: var(--surface-sel); }

  /* A real radio, restyled: keeps native group semantics and arrow-key
     navigation instead of faking a group out of divs. */
  input[type="radio"] { flex: none; width: 14px; height: 14px; margin: 2px 0 0;
                        accent-color: var(--accent); cursor: default; }
  .txt { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .nm { font-size: 13.5px; font-weight: 600; color: var(--text); }
  .sub { font-size: 12px; color: var(--text-dim); line-height: 1.5;
         overflow-wrap: anywhere; }
  code { font-family: var(--font-mono); font-size: 11px; }
</style>
