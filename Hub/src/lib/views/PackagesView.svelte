<script lang="ts">
  import EmptyState from "$lib/components/EmptyState.svelte";

  // PLACEHOLDER, deliberately marked as one.
  //
  // The design rule for this sidebar was "every entry points at a surface that
  // exists" -- nav aimed at nothing makes a launcher feel like a mockup of
  // itself. This entry was added ahead of its feature by explicit request, so
  // the mitigation is that the surface says plainly that it is not built yet
  // rather than presenting dead controls. There are no fake install buttons
  // here on purpose: a disabled "Install" is a worse lie than a sentence.
  //
  // When this is built, the work already exists to borrow from: scripts/setup.ps1
  // is the headless doctor that sequences dependency checks and installs
  // (vcpkg deps, Docker, the Postgres bring-up in Server/scripts/db-setup), and
  // Setup.exe is a Tauri GUI over exactly that. The Hub should drive the same
  // orchestrator rather than growing a second one.
  const planned = [
    {
      name: "Multiplayer",
      needs: "PostgreSQL via Docker, the Account and Combat services",
      why: "Persistent player state and matchmaking for a project that wants them.",
    },
  ];
</script>

<header>
  <h2 class="display view-title">Packages</h2>
  <p class="view-sub">Optional capability, and the dependencies each one needs</p>
</header>

<div class="wrap">
  <EmptyState
    title="Not built yet"
    body="Packages will add optional capability to a project and check that this
          machine has what it needs — a doctor that reports what is missing and
          installs it, rather than leaving you to follow a setup document." />

  <section>
    <h3 class="label">Planned</h3>
    {#each planned as p (p.name)}
      <div class="row">
        <div class="txt">
          <p class="t">{p.name}</p>
          <p class="d">{p.why}</p>
          <p class="needs">Needs: {p.needs}</p>
        </div>
        <span class="tag">Planned</span>
      </div>
    {/each}
    <p class="hint">
      This will drive the same doctor the setup wizard already uses, rather than
      growing a second one beside it.
    </p>
  </section>
</div>

<style>
  .wrap { display: flex; flex-direction: column; gap: 22px; }
  section { border-top: 1px solid var(--border-soft); padding-top: 14px; }
  h3 { margin: 0 0 10px; }

  .row { display: flex; align-items: flex-start; justify-content: space-between;
         gap: 16px; padding: 13px 14px; background: var(--surface);
         border: 1px solid var(--border-soft); border-radius: var(--r-panel); }
  .txt { min-width: 0; }
  .t { font-size: 13.5px; font-weight: 600; color: var(--text); margin: 0; }
  .d { font-size: 12.5px; color: var(--text-dim); margin: 4px 0 0; line-height: 1.55; }
  .needs { font-size: 11.5px; color: var(--text-dim); margin: 6px 0 0;
           font-family: var(--font-mono); }

  /* Neutral, not the accent: nothing here can be acted on, and the accent is
     reserved for things that can. */
  .tag { flex: none; font-size: 10.5px; letter-spacing: .12em;
         text-transform: uppercase; color: var(--text-dim);
         border: 1px solid var(--border); border-radius: 999px; padding: 3px 9px; }

  .hint { font-size: 12.5px; color: var(--text-dim); margin: 14px 0 0; }
</style>
