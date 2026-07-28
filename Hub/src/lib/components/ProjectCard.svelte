<script lang="ts">
  import ProjectMenu from "$lib/components/ProjectMenu.svelte";
  import { coverFor, engineChipText, engineChipTitle, compatibilityNote,
           missingNote, projectDir } from "$lib/format";
  import { since, type RecentProject } from "$lib/api";

  let { project, compatible, engineAbi, engineLabel, pinned, dangling,
        disabled = false, confirmDelete, onLaunch, onDelete, onChangeEngine,
        onReveal, onRename, onArgs, onForget, onLocate }:
    {
      project: RecentProject; compatible: boolean; engineAbi: number | null;
      /** Build name of the engine that will actually launch this project. */
      engineLabel: string;
      /** An explicit per-project choice is in effect. */
      pinned: boolean;
      /** Pinned to an engine that is no longer registered. */
      dangling: boolean;
      disabled?: boolean; confirmDelete: boolean;
      onLaunch: () => void; onDelete: () => void;
      onChangeEngine: () => void;
      onReveal: () => void; onRename: () => void; onArgs: () => void;
      onForget: () => void; onLocate: () => void;
    } = $props();

  const cover = $derived(coverFor(project.name, project.path));
  // The recorded path stopped resolving. Everything that acts on the folder
  // (launch, the engine band) disables; the menu shrinks to Locate/Remove.
  const gone = $derived(project.missing);
  // The same folder line the list row shows. A tile that named the project and
  // nothing else was the one place in the Hub where you could not tell two
  // same-named projects apart.
  const dir = $derived(projectDir(project.path));

  // Shared with ProjectRow via format.ts: the two layouts must say the same
  // thing about the same project, and this copy states facts about the system,
  // so it is tested rather than retyped per layout.
  const engineText = $derived(engineChipText(engineLabel, pinned, dangling));
  const engineTitle = $derived(engineChipTitle(engineLabel, pinned, dangling));
  const why = $derived(
    compatibilityNote(compatible, project.path, project.engineAbi, engineLabel, engineAbi),
  );

  // Right-click opens the SAME menu the ellipsis does, at the cursor.
  let menu: ProjectMenu;
</script>

<!-- The tile carries EXACTLY what the list row carries -- name, folder, opened,
     engine, abi -- stacked instead of in columns. They are two views of one
     record, so a fact that only appears in one of them is a reason to pick a
     layout for the wrong reason.
     Three distinct state classes, not one `bad`: `incompat` (card, wrong ABI),
     `gone` (card, path no longer on disk) and `missing` (engine chip, dangling
     pin) are independent failure states, and one class name meaning two things
     in one file is a trap for the next edit. -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- Right-click is a redundant MOUSE affordance for a menu already sitting on
     a focusable button on this card, so a keyboard user loses nothing and there
     is no role that means "card you may right-click". Same reasoning as the
     modal scrim and WindowChrome's double-click-to-maximize. -->
<div class="card" class:incompat={!compatible && !gone} class:gone
     oncontextmenu={(e) => { e.preventDefault(); menu.openAt(e.clientX, e.clientY); }}>
  <div class="top">
    <button class="hit" type="button" disabled={disabled || gone} onclick={onLaunch}
            title={gone ? missingNote(project.path) : why} aria-label={project.name}>
      <!-- The monogram, at BADGE scale. It was a 76px gradient band across the
           head of every card -- the last of the decorative-slab styling, and by
           some distance the least like anything else left in the Hub. Shrunk
           rather than dropped: it is the only per-project identity a grid has,
           and at 40px the gradient reads as a tint instead of as a picture. -->
      <span class="mono" style="--a: {cover.angle}deg" aria-hidden="true">{cover.monogram}</span>
      <span class="txt">
        <span class="nm">{project.name}</span>
        <span class="path">{dir}</span>
      </span>
    </button>

    <!-- The SAME action menu the list row uses, so both layouts offer the same
         things. In flow at the end of the header rather than floating over the
         card: absolutely positioned, it sat on top of whatever the name line
         needed, and the name is the one thing a tile must always show. -->
    <ProjectMenu bind:this={menu} {project} {disabled} {confirmDelete}
                 {onReveal} {onRename} {onArgs} {onForget} {onLocate} {onDelete} />
  </div>

  <div class="meta">
    {#if gone}
      <!-- The badge slot: being gone supersedes any ABI statement, because the
           number came from a manifest that is not there to disagree with. -->
      <span class="badge">missing</span>
    {:else if compatible}
      <span>abi {project.engineAbi ? project.engineAbi : "?"}</span>
    {:else}
      <span class="badge">abi {project.engineAbi}</span>
    {/if}
    <span>{since(project.lastOpenedUtc)}</span>
  </div>

  <!-- A SIBLING of .hit, not a child: nesting an interactive control inside a
       button is invalid HTML and browsers do not deliver its clicks reliably.
       Behind a hairline because it is a different action from the rest of the
       card -- everything above launches, this changes what launches it. -->
  <button class="eng" type="button" disabled={disabled || gone} onclick={onChangeEngine}
          class:pin={pinned} class:missing={dangling} title={engineTitle}
          aria-label="Engine for {project.name}: {engineText}">
    <!-- A CSS dot, not a glyph: hollow = following the default, filled =
         an explicit choice. Drawn rather than typed so it cannot depend on
         a symbol font and stays crisp at this size. -->
    <span class="mark" class:filled={pinned || dangling} aria-hidden="true"></span>
    <span class="lbl">{engineText}</span>
  </button>
</div>

<style>
  .card { position: relative; border: 1px solid var(--border-soft);
          border-radius: var(--r-panel); overflow: hidden; background: var(--surface);
          transition: border-color var(--dur) var(--ease),
                      background var(--dur) var(--ease); }
  /* A surface wash as well as the border, matching the list row and the nav.
     Border-only was the app's hover idiom before the monochrome pass; every
     other hoverable surface has since moved to lightening. */
  .card:hover { border-color: var(--border-hover);
                background: color-mix(in srgb, #ffffff 4%, var(--surface)); }

  .top { display: flex; align-items: flex-start; gap: 10px; padding: 11px 11px 0 13px; }
  .hit { flex: 1; min-width: 0; display: flex; align-items: center; gap: 10px;
         background: none; border: 0; padding: 0; text-align: left;
         font: inherit; color: inherit; cursor: default; }
  .hit:disabled { opacity: .5; }

  .mono { flex: none; width: 40px; height: 40px; display: grid; place-items: center;
          border-radius: 6px; font-family: var(--font-display); font-size: 17px;
          color: var(--cover-ink);
          background: linear-gradient(var(--a), var(--cover-from), var(--cover-to)); }

  /* Same type as the list row's name cell, deliberately: one record, two
     layouts, and a project should not change weight or colour between them. */
  .txt { min-width: 0; display: flex; flex-direction: column; gap: 1px; }
  .nm { max-width: 100%; font-size: 14px; font-weight: 600; color: var(--text);
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .path { max-width: 100%; font-family: var(--font-mono); font-size: 11.5px;
          color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .meta { display: flex; justify-content: space-between; align-items: center;
          gap: 8px; padding: 9px 13px 10px; font-family: var(--font-mono);
          font-size: 11px; color: var(--text-dim); }

  /* INERT: surfaces drop back, name recedes to muted (5.9:1). The tile CAN dim
     its surface where the flat list row cannot -- there the surface carries
     hover and a permanent tint would swallow it.
     `.card.incompat` rather than a bare class on purpose: the base `.card` rule
     sets the same two properties at equal specificity, so a bare one would win
     only by declaration order. This rule is absolute in the spec, so it should
     not depend on where it sits in the file.
     `.gone` shares every dim rule: a project that is not on disk is inert for a
     different reason but to the same degree, and two dim treatments would read
     as a third state that does not exist. */
  .card.incompat, .card.gone { background: var(--surface-2); border-color: var(--border); }
  .card.incompat:hover, .card.gone:hover {
    background: color-mix(in srgb, #ffffff 4%, var(--surface-2)); }
  .card.incompat .mono, .card.gone .mono {
    background: linear-gradient(var(--a), var(--border-soft), var(--bg-bottom));
    color: var(--text-dim); }
  .card.incompat .nm, .card.gone .nm { color: var(--text-muted); }
  /* The app-wide focus ring is the ACCENT (theme.css `:focus-visible`), which
     would paint the act colour onto an incompatible card the instant it is
     tabbed to -- a case no automated gate catches, because it only exists in
     the focus state. Coral is the card's own state colour and measures 8.2:1
     against this surface, so the ring stays plainly visible; only its hue
     changes. Since the re-hue that shift is subtler (both are reds) but it
     still keeps the act colour off a card that cannot be acted on. */
  .card.incompat :focus-visible, .card.gone :focus-visible { outline-color: var(--fail); }
  /* Second, non-chromatic signal: a bordered badge (coral is 8.2:1 here). */
  .card.incompat .badge, .card.gone .badge { font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent);
           border-radius: 3px; padding: 0 4px; }

  /* Its own band behind a hairline, the separator idiom the list and the
     settings sheet both use for "a different kind of thing starts here". */
  .eng { display: flex; align-items: center; gap: 7px; width: 100%;
         padding: 9px 13px; background: none; border: 0;
         border-top: 1px solid var(--border-soft); text-align: left;
         font: inherit; font-size: 11.5px; color: var(--text-dim);
         cursor: default;
         transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .eng:hover:not(:disabled) { color: var(--text-muted);
                              background: rgba(255, 255, 255, .04); }
  .eng:disabled { opacity: .5; }
  .mark { flex: none; width: 7px; height: 7px; border-radius: 50%;
          border: 1.5px solid currentColor; }
  .mark.filled { background: currentColor; }
  .lbl { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  /* Pinned reads as deliberate, not as a warning -- no accent, which means act. */
  .eng.pin { color: var(--text-muted); }
  .eng.missing { color: var(--fail); }
</style>
