<script lang="ts">
  import { coverFor, engineChipText, engineChipTitle, compatibilityNote } from "$lib/format";
  import { since, type RecentProject } from "$lib/api";

  let { project, compatible, engineAbi, engineLabel, pinned, dangling,
        disabled = false, onLaunch, onForget, onChangeEngine }:
    {
      project: RecentProject; compatible: boolean; engineAbi: number | null;
      /** Build name of the engine that will actually launch this project. */
      engineLabel: string;
      /** An explicit per-project choice is in effect. */
      pinned: boolean;
      /** Pinned to an engine that is no longer registered. */
      dangling: boolean;
      disabled?: boolean; onLaunch: () => void; onForget: () => void;
      onChangeEngine: () => void;
    } = $props();

  const cover = $derived(coverFor(project.name, project.path));

  // Shared with ProjectRow via format.ts: the two layouts must say the same
  // thing about the same project, and this copy states facts about the system,
  // so it is tested rather than retyped per layout.
  const engineText = $derived(engineChipText(engineLabel, pinned, dangling));
  const engineTitle = $derived(engineChipTitle(engineLabel, pinned, dangling));
  const why = $derived(
    compatibilityNote(compatible, project.path, project.engineAbi, engineLabel, engineAbi),
  );
</script>

<!-- `incompat` / `missing` rather than `bad` on both: the card and the engine
     row have independent failure states, and one class name meaning two things
     in one file is a trap for the next edit. -->
<div class="card" class:incompat={!compatible}>
  <button class="hit" type="button" {disabled} onclick={onLaunch} title={why} aria-label={project.name}>
    <span class="cover" style="--a: {cover.angle}deg" aria-hidden="true">{cover.monogram}</span>
    <span class="cb">
      <span class="nm">{project.name}</span>
      <span class="mt">
        {#if compatible}
          <span>abi {project.engineAbi ? project.engineAbi : "?"}</span>
        {:else}
          <span class="badge">abi {project.engineAbi}</span>
        {/if}
        <span>{since(project.lastOpenedUtc)}</span>
      </span>
    </span>
  </button>

  <!-- A SIBLING of .hit, not a child: nesting an interactive control inside a
       button is invalid HTML and browsers do not deliver its clicks reliably.
       Same reason the remove button below sits outside .hit. -->
  <button class="eng" type="button" {disabled} onclick={onChangeEngine}
          class:pin={pinned} class:missing={dangling} title={engineTitle}
          aria-label="Engine for {project.name}: {engineText}">
    <!-- A CSS dot, not a glyph: hollow = following the default, filled =
         an explicit choice. Drawn rather than typed so it cannot depend on
         a symbol font and stays crisp at this size. -->
    <span class="mark" class:filled={pinned || dangling} aria-hidden="true"></span>
    <span class="lbl">{engineText}</span>
  </button>

  <!-- Says "nothing is deleted" outright. A bare X on a card is exactly the
       control users assume is destructive, and it is not: forget_project only
       edits the Hub's own recents list. -->
  <button class="x" type="button" {disabled} onclick={onForget}
          aria-label="Remove {project.name} from the list. Does not delete it from disk."
          title="Remove from this list &mdash; does not delete the project from disk">&#10005;</button>
</div>

<style>
  .card { position: relative; border: 1px solid var(--border-soft);
          border-radius: var(--r-panel); overflow: hidden; background: var(--surface);
          transition: border-color var(--dur) var(--ease); }
  .card:hover { border-color: var(--border-hover); }
  .hit { display: block; width: 100%; text-align: left; background: none;
         border: 0; padding: 0; font: inherit; color: inherit; cursor: default; }
  .hit:disabled { opacity: .5; }

  .cover { display: grid; place-items: center; height: 76px;
           font-family: var(--font-display); font-size: 24px; color: var(--cover-ink);
           background: linear-gradient(var(--a), var(--cover-from), var(--cover-to)); }
  .cb { display: block; padding: 11px 13px; }
  .nm { display: block; font-size: 14px; font-weight: 600; color: var(--text);
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .mt { display: flex; justify-content: space-between; align-items: center;
        margin-top: 7px; font-family: var(--font-mono); font-size: 11px;
        color: var(--text-dim); }

  /* INERT: no gold anywhere, surfaces drop back, name recedes to muted (5.9:1).
     `.card.incompat` rather than bare `.bad` on purpose: the base `.card` rule sets
     the same two properties at equal specificity, so a bare `.bad` would win
     only by declaration order. This rule is absolute in the spec, so it should
     not depend on where it sits in the file. */
  .card.incompat { background: var(--surface-2); border-color: var(--border); }
  .card.incompat .cover { background: linear-gradient(var(--a), var(--border-soft), var(--bg-bottom));
                          color: var(--text-dim); }
  .card.incompat .nm { color: var(--text-muted); }
  /* The app-wide focus ring is the ACCENT (theme.css `:focus-visible`), which
     would paint the act colour onto an incompatible card the instant it is
     tabbed to -- a case no automated gate catches, because it only exists in
     the focus state. Coral is the card's own state colour and measures 8.2:1
     against this surface, so the ring stays plainly visible; only its hue
     changes. Since the re-hue that shift is subtler (both are reds) but it
     still keeps the act colour off a card that cannot be acted on. */
  .card.incompat :focus-visible { outline-color: var(--fail); }
  /* Second, non-chromatic signal: a bordered badge (coral is 8.2:1 here). */
  .card.incompat .badge { font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent); border-radius: 3px; padding: 0 4px; }

  /* Its own row under the meta line rather than squeezed into it: at the 190px
     grid minimum an engine name has nowhere to go beside "abi N" and "2m ago". */
  .eng { display: flex; align-items: center; gap: 7px; width: 100%;
         padding: 7px 13px 11px; background: none; border: 0; text-align: left;
         font: inherit; font-size: 11.5px; color: var(--text-dim);
         cursor: default; transition: color var(--dur) var(--ease); }
  .eng:hover:not(:disabled) { color: var(--text-muted); }
  .eng:disabled { opacity: .5; }
  .mark { flex: none; width: 7px; height: 7px; border-radius: 50%;
          border: 1.5px solid currentColor; }
  .mark.filled { background: currentColor; }
  .lbl { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  /* Pinned reads as deliberate, not as a warning -- no accent, which means act. */
  .eng.pin { color: var(--text-muted); }
  .eng.missing { color: var(--fail); }

  .x { position: absolute; top: 7px; right: 7px; width: 24px; height: 24px;
       display: grid; place-items: center; border: 0; border-radius: 4px;
       background: color-mix(in srgb, var(--bg-bottom) 60%, transparent);
       color: var(--text-dim); font-size: 10px;
       cursor: default; opacity: 0;
       transition: opacity var(--dur) var(--ease), color var(--dur) var(--ease); }
  .card:hover .x, .x:focus-visible { opacity: 1; }
  .x:hover { color: var(--fail); }
</style>
