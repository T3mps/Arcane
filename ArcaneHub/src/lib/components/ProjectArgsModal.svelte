<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import type { RecentProject } from "$lib/api";

  let { project, busy, error, onSave, onCancel }: {
    project: RecentProject;
    busy: boolean;
    /** The last guard() failure, shown here because the banner is under the scrim. */
    error: string;
    onSave: (args: string) => Promise<boolean>;
    onCancel: () => void;
  } = $props();

  // Seeded once: mounted fresh per project, then the field is the user's.
  // svelte-ignore state_referenced_locally
  let args = $state(project.args);

  const submit = () => { if (!busy) onSave(args.trim()); };
</script>

<Modal title="Command-line arguments" onClose={onCancel}>
  <p class="lead">
    Passed to the editor after <code>--project</code> when <strong>{project.name}</strong>
    launches. Saved with this project only, in the Hub &mdash; not in the
    <code>.arcproj</code>, so they do not follow the project to anyone else.
  </p>

  <label class="f">
    <span class="label">Arguments</span>
    <input bind:value={args} spellcheck="false"
           placeholder="--backend vulkan"
           onkeydown={(e) => e.key === "Enter" && submit()} />
  </label>

  <!-- Says how the string is read, because the rule is not guessable and the
       failure is silent: a path with a space, split in two, just produces an
       argument the editor ignores. -->
  <p class="note">
    Split on spaces. Wrap a value containing a space in double quotes, as in
    <code>--scene "My Level"</code>. Each token is handed to the editor exactly
    as typed &mdash; there is no shell in between, so nothing here is expanded.
  </p>

  {#if error}
    <p class="banner" role="alert">{error}</p>
  {/if}

  {#snippet footer()}
    <Button onclick={onCancel} disabled={busy}>Cancel</Button>
    <Button variant="primary" onclick={submit} disabled={busy}>Save</Button>
  {/snippet}
</Modal>

<style>
  .lead { font-size: 13px; color: var(--text-dim); margin: 0; line-height: 1.55; }
  strong { color: var(--text-muted); font-weight: 600; }

  .f { display: flex; flex-direction: column; gap: 5px; }
  input { background: var(--well); border: 1px solid var(--border);
          border-radius: var(--r-btn); color: var(--text);
          font-family: var(--font-mono); font-size: 12.5px; padding: 9px 12px;
          user-select: text; cursor: text; width: 100%; }
  input::placeholder { color: var(--text-dim); }
  input:focus { border-color: var(--accent-ring); outline: none; }

  .note { font-size: 12px; color: var(--text-dim); margin: 0; line-height: 1.55; }
  code { font-family: var(--font-mono); font-size: 11px; color: var(--text-muted); }

  /* Matches the main-area banner in +page.svelte -- same failure, same look. */
  .banner { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
            border: 1px solid color-mix(in srgb, var(--fail-accent) 40%, transparent);
            border-radius: var(--r-btn); color: var(--text); font-size: 13px;
            padding: 10px 13px; margin: 0; user-select: text; }
</style>
