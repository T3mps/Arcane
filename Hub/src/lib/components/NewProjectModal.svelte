<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import { projectNameError, projectPathPreview } from "$lib/format";
  import type { EngineEntry } from "$lib/api";

  let { busy, error, engine, defaultDir, onBrowse, onCreate, onCancel }: {
    busy: boolean;
    /** The last guard() failure, shown here because the banner is under the scrim. */
    error: string;
    engine: EngineEntry | null;
    defaultDir: string;
    onBrowse: () => Promise<string | null>;
    onCreate: (name: string, dir: string) => Promise<boolean>;
    onCancel: () => void;
  } = $props();

  let name = $state("");
  // Seeded once, deliberately: the dialog is mounted fresh each time it opens
  // (`{#if creating}`), and once open the field is the user's to edit -- a later
  // settings change must not yank the folder out from under them. Settings is
  // unreachable while the modal is up, so "later" only means the next open.
  // svelte-ignore state_referenced_locally
  let dir = $state(defaultDir);
  // Errors stay hidden until the field has been used or Create was pressed, so
  // the dialog does not open already complaining about an empty name.
  let touched = $state(false);

  const nameError = $derived(projectNameError(name));
  const preview = $derived(projectPathPreview(dir, name));
  const ready = $derived(!nameError && dir.trim() !== "");

  async function browse() {
    const picked = await onBrowse();
    if (picked !== null) dir = picked;
  }

  async function submit() {
    touched = true;
    if (!ready || busy) return;
    // The parent closes the modal on success; on failure it stays open with
    // the typed values intact so the error banner can be acted on.
    await onCreate(name.trim(), dir.trim());
  }
</script>

<Modal title="New project" onClose={onCancel}>
  <label class="f">
    <span class="label">Name</span>
    <input bind:value={name} spellcheck="false" placeholder="MyGame"
           oninput={() => (touched = true)}
           onkeydown={(e) => e.key === "Enter" && submit()} />
  </label>
  {#if touched && nameError}
    <p class="err" role="alert">{nameError}</p>
  {/if}

  <div class="f">
    <span class="label">Location</span>
    <div class="row">
      <input bind:value={dir} spellcheck="false" placeholder="Choose a folder"
             onkeydown={(e) => e.key === "Enter" && submit()} />
      <Button onclick={browse} disabled={busy}>Browse&hellip;</Button>
    </div>
  </div>

  <!-- Always rendered so the dialog does not change height as you type. -->
  <p class="preview" class:dim={!preview}>
    {preview || "The project folder and its .arcproj appear here."}
  </p>

  {#if error}
    <p class="banner" role="alert">{error}</p>
  {/if}

  {#if engine}
    <p class="note">
      Stamped with <strong>{engine.build}</strong> (abi {engine.engineAbi}), the engine
      selected in the sidebar.
    </p>
  {/if}

  {#snippet footer()}
    <Button onclick={onCancel} disabled={busy}>Cancel</Button>
    <Button variant="primary" onclick={submit} disabled={busy || !ready}>Create</Button>
  {/snippet}
</Modal>

<style>
  .f { display: flex; flex-direction: column; gap: 5px; }
  .row { display: flex; gap: 8px; }
  .row input { flex: 1; min-width: 0; }
  input { background: var(--well); border: 1px solid var(--border);
          border-radius: var(--r-btn); color: var(--text); font: inherit;
          font-size: 13px; padding: 9px 12px; user-select: text; cursor: text;
          width: 100%; }
  input::placeholder { color: var(--text-dim); }
  input:focus { border-color: var(--accent-ring); outline: none; }

  /* --fail, not --fail-accent: this is an inline field note, not the banner. */
  .err { color: var(--fail); font-size: 12.5px; margin: -6px 0 0; }
  .preview { font-family: var(--font-mono); font-size: 11.5px; color: var(--text-muted);
             background: var(--well); border: 1px solid var(--border-soft);
             border-radius: var(--r-btn); padding: 7px 9px; margin: 2px 0 0;
             overflow-wrap: anywhere; user-select: text; }
  .preview.dim { color: var(--text-dim); font-family: var(--font-ui); }
  /* Matches the main-area banner in +page.svelte -- same failure, same look. */
  .banner { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
            border: 1px solid color-mix(in srgb, var(--fail-accent) 40%, transparent);
            border-radius: var(--r-btn); color: var(--text); font-size: 13px;
            padding: 10px 13px; margin: 0; user-select: text; }
  .note { font-size: 12.5px; color: var(--text-dim); margin: 0; }
  strong { color: var(--text-muted); font-weight: 600; }
</style>
