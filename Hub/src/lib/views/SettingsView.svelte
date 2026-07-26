<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import type { Settings } from "$lib/api";

  let { settings, busy, recentCount, hubDir, version,
        onSave, onBrowseDir, onReveal, onClearRecents }: {
    settings: Settings;
    busy: boolean;
    recentCount: number;
    hubDir: string;
    version: string;
    onSave: (s: Settings) => void;
    onBrowseDir: () => Promise<string | null>;
    onReveal: () => void;
    onClearRecents: () => void;
  } = $props();

  // Local mirror of the text field. Committing on change (blur/Enter) rather
  // than on every keystroke keeps a half-typed path out of settings.json.
  //
  // Re-synced from the prop rather than seeded once: Rust normalises the folder
  // on save, so a typed "D:\Games\" comes back as "D:\Games" and the field must
  // show what was actually stored. This view stays mounted across saves, so a
  // one-shot seed would leave the two permanently out of step.
  let dirDraft = $state("");
  // Guarded on the STORED value changing, not on `settings` being reassigned.
  // Every save replaces the whole object, so an unguarded sync also fired when
  // an unrelated setting changed -- toggling the checkbox discarded whatever
  // the user had typed here but not yet committed. A plain variable, so reading
  // it does not make this effect depend on itself.
  let lastSynced: string | null = null;
  $effect(() => {
    const stored = settings.defaultProjectDir;
    if (stored !== lastSynced) {
      lastSynced = stored;
      dirDraft = stored;
    }
  });
  // Two-step confirm: clearing is reversible only by re-opening every project,
  // so it should not be one stray click.
  let confirmClear = $state(false);

  function commitDir() {
    if (dirDraft === settings.defaultProjectDir) return;
    onSave({ ...settings, defaultProjectDir: dirDraft });
  }

  async function browse() {
    const picked = await onBrowseDir();
    if (picked === null) return;
    dirDraft = picked;
    onSave({ ...settings, defaultProjectDir: picked });
  }

  function clearDir() {
    dirDraft = "";
    onSave({ ...settings, defaultProjectDir: "" });
  }

  function doClear() {
    confirmClear = false;
    onClearRecents();
  }
</script>

<header class="top">
  <div>
    <h2 class="display view-title">Settings</h2>
    <p class="view-sub">Saved to the Hub's own state; the engine never reads them.</p>
  </div>
</header>

<section>
  <h3 class="label">Projects</h3>
  <div class="row">
    <div class="txt">
      <p class="t">Default location</p>
      <p class="d">Where the New Project and Open dialogs start. Leave empty to let
        Windows pick the folder you used last.</p>
    </div>
  </div>
  <div class="ctl">
    <input bind:value={dirDraft} spellcheck="false" placeholder="Not set"
           onchange={commitDir}
           onkeydown={(e) => e.key === "Enter" && commitDir()} />
    <Button onclick={browse} disabled={busy}>Browse&hellip;</Button>
    <Button onclick={clearDir} disabled={busy || dirDraft === ""}>Clear</Button>
  </div>
</section>

<section>
  <h3 class="label">Launching</h3>
  <label class="row switch">
    <div class="txt">
      <p class="t">Close the Hub after launching a project</p>
      <p class="d">The editor runs independently either way -- closing the Hub never
        closes an editor.</p>
    </div>
    <input type="checkbox" checked={settings.closeAfterLaunch} disabled={busy}
           onchange={(e) => onSave({ ...settings, closeAfterLaunch: e.currentTarget.checked })} />
  </label>
</section>

<section>
  <h3 class="label">Data</h3>
  <div class="row">
    <div class="txt">
      <p class="t">Hub data folder</p>
      <p class="d"><code>{hubDir}</code></p>
    </div>
    <Button onclick={onReveal} disabled={busy}>Open folder</Button>
  </div>
  <div class="row">
    <div class="txt">
      <p class="t">Recent projects</p>
      <!-- States plainly what the Remove control on a card also promises. -->
      <p class="d">{recentCount} in the list. Clearing removes them from the Hub only;
        nothing is deleted from disk.</p>
    </div>
    {#if confirmClear}
      <div class="confirm">
        <Button onclick={() => (confirmClear = false)} disabled={busy}>Cancel</Button>
        <Button variant="danger" onclick={doClear} disabled={busy}>Clear the list</Button>
      </div>
    {:else}
      <Button variant="danger" disabled={busy || recentCount === 0}
              onclick={() => (confirmClear = true)}>Clear list</Button>
    {/if}
  </div>
</section>

<section>
  <h3 class="label">About</h3>
  <div class="row">
    <div class="txt">
      <p class="t">Arcane Hub {version}</p>
      <p class="d">Registers engines and launches projects. Engine installs, updates,
        and accounts are deliberately out of scope.</p>
    </div>
  </div>
</section>

<style>
  .top { margin-bottom: 15px; }

  section { border-top: 1px solid var(--border-soft); padding: 14px 0 4px; }
  section:first-of-type { border-top: 0; }
  h3 { margin: 0 0 10px; }

  .row { display: flex; align-items: center; justify-content: space-between;
         gap: 16px; padding: 6px 0; }
  .txt { min-width: 0; }
  .t { font-size: 13.5px; color: var(--text); margin: 0; }
  .d { font-size: 12.5px; color: var(--text-dim); margin: 4px 0 0; line-height: 1.55; }
  code { font-family: var(--font-mono); font-size: 11.5px; color: var(--text-muted);
         user-select: text; overflow-wrap: anywhere; }

  .ctl { display: flex; gap: 8px; padding: 4px 0 6px; }
  .ctl input { flex: 1; min-width: 0; }
  .ctl input {
    background: var(--well); border: 1px solid var(--border);
    border-radius: var(--r-btn); color: var(--text); font: inherit; font-size: 13px;
    padding: 9px 12px; user-select: text; cursor: text; }
  .ctl input::placeholder { color: var(--text-dim); }
  .ctl input:focus { border-color: var(--accent-ring); outline: none; }

  .switch { cursor: default; }
  /* A real checkbox, restyled: accent-color keeps the native control (and its
     keyboard and screen-reader behaviour) instead of faking one with a div. */
  input[type="checkbox"] { flex: none; width: 17px; height: 17px; margin: 0;
                           accent-color: var(--accent); cursor: default; }

  .confirm { display: flex; gap: 8px; flex: none; }
</style>
