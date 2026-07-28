<script module lang="ts">
  import type { RecentProject } from "$lib/api";

  // The per-project action vocabulary, defined WHERE the menu is: this
  // component owns "what can you do to a project", so the callback contract
  // lives beside the item list it feeds. ONE object instead of eight props --
  // the chain (+page -> ProjectsView -> Card/Row -> here) was four layers of
  // hand-forwarded callbacks, and extending it meant touching every layer
  // (the Locate... addition missed two destructures on its first pass, which
  // is exactly the drift this collapses away).
  export type ProjectActions = {
    launch: (p: RecentProject) => void;
    /** Open the pin-an-engine picker for this project. */
    changeEngine: (p: RecentProject) => void;
    reveal: (p: RecentProject) => void;
    rename: (p: RecentProject) => void;
    args: (p: RecentProject) => void;
    /** Remove from the LIST only -- `delete` is the one that touches disk. */
    forget: (p: RecentProject) => void;
    /** Repoint a MISSING project at its moved .arcproj. */
    locate: (p: RecentProject) => void;
    delete: (p: RecentProject) => void;
  };
</script>

<script lang="ts">
  import Icon from "$lib/components/Icon.svelte";
  import { menuItemsFor, type MenuItem } from "$lib/format";

  // The per-project action menu, shared by the tile and the list row. It owns
  // the popover; the ITEM LIST itself lives in format.ts (menuItemsFor) where
  // it is unit-tested -- "what can you do to a project" is one vocabulary,
  // and it is copy that states facts, so it gets tested like the chip text.
  let { project, disabled = false, confirmDelete = true, actions }: {
    project: RecentProject;
    disabled?: boolean;
    /** Whether Delete opens a confirmation. Only affects this item's LABEL. */
    confirmDelete?: boolean;
    actions: ProjectActions;
  } = $props();

  const items = $derived(menuItemsFor(project.missing, confirmDelete));

  let open = $state(false);
  let trigger: HTMLButtonElement;
  let menu = $state<HTMLElement | null>(null);
  // Built when the menu opens, from the trigger's screen position.
  let placement = $state("");

  const MENU_W = 236;
  const EDGE = 8;
  // Enough room below the anchor to be worth opening downwards. Deliberately
  // an estimate: anchoring by `bottom` instead of measuring means the exact
  // menu height is never needed, and it does not exist until after it renders.
  const MIN_BELOW = 200;

  // `left` is where the menu's left edge wants to be; `top`/`bottom` bound the
  // thing being opened from -- a button's box, or a zero-height point at the
  // cursor. One routine for both, so the two ways in cannot drift apart in how
  // they handle the window edges.
  function place(left: number, top: number, bottom: number) {
    const l = Math.max(EDGE, Math.min(left, window.innerWidth - MENU_W - EDGE));
    placement =
      window.innerHeight - bottom >= MIN_BELOW
        ? `left:${l}px; top:${bottom + 4}px;`
        : `left:${l}px; bottom:${window.innerHeight - top + 4}px;`;
  }

  function show() {
    const r = trigger.getBoundingClientRect();
    // Right-aligned to the trigger: it sits at the right edge of a row, so a
    // left-aligned menu would hang off-screen.
    place(r.right - MENU_W, r.top, r.bottom);
    open = true;
  }

  /**
   * Open at a point instead of under the trigger -- the right-click path.
   *
   * A component export, reached through `bind:this`, rather than a second menu
   * built in the card: "mirrors the ellipsis exactly" is only true if it IS the
   * ellipsis menu, sharing one item list and one set of keyboard rules.
   *
   * Top-left at the cursor, which is what every desktop context menu does.
   */
  export function openAt(x: number, y: number) {
    place(x, y, y);
    open = true;
  }

  function hide(restoreFocus = true) {
    if (!open) return;
    open = false;
    if (restoreFocus) trigger.focus();
  }

  const buttons = () =>
    menu ? [...menu.querySelectorAll<HTMLButtonElement>("button")] : [];

  // Runs once with `menu` still null (the {#if} has not rendered yet) and again
  // once it is bound; the null pass is a no-op. Safe to re-run, unlike Modal's
  // focus effect, because there is no cleanup here to fire in between.
  $effect(() => {
    if (open && menu) buttons()[0]?.focus();
  });

  function choose(it: MenuItem) {
    // Close FIRST: several of these open a dialog, and leaving a popover behind
    // the scrim is how a stray click lands on a control the user cannot see.
    open = false;
    // The item's kind IS a ProjectActions key: dispatch by lookup, so there is
    // no hand-written mapping to fall out of step with the vocabulary.
    actions[it.kind](project);
  }

  function onMenuKeys(e: KeyboardEvent) {
    const f = buttons();
    if (f.length === 0) return;
    const i = f.indexOf(document.activeElement as HTMLButtonElement);

    switch (e.key) {
      case "ArrowDown":
        e.preventDefault();
        f[(i + 1) % f.length].focus();
        break;
      case "ArrowUp":
        e.preventDefault();
        f[(i - 1 + f.length) % f.length].focus();
        break;
      case "Home":
        e.preventDefault();
        f[0].focus();
        break;
      case "End":
        e.preventDefault();
        f[f.length - 1].focus();
        break;
      // Tab dismisses rather than trapping -- this is a menu, not a dialog.
      // Focus goes back to the TRIGGER instead of onward: the items are
      // tabindex="-1" and the menu unmounts on this very keystroke, so letting
      // the default run would move focus off an element that no longer exists
      // and drop the user at the top of the document.
      case "Tab":
        e.preventDefault();
        hide();
        break;
    }
  }

  function onWindowKey(e: KeyboardEvent) {
    if (open && e.key === "Escape") {
      e.preventDefault();
      hide();
    }
  }

  // pointerdown, not click: the menu must be gone before whatever was clicked
  // reacts. The trigger is excluded so its own click can toggle rather than
  // close-then-reopen.
  function onPointerDown(e: PointerEvent) {
    if (!open) return;
    const t = e.target as Node;
    if (trigger.contains(t) || menu?.contains(t)) return;
    hide(false);
  }
</script>

<svelte:window onkeydown={onWindowKey} onpointerdown={onPointerDown}
               onresize={() => hide(false)} onscrollcapture={() => hide(false)} />

<button class="dots" type="button" {disabled} bind:this={trigger}
        aria-haspopup="menu" aria-expanded={open}
        aria-label="Actions for {project.name}"
        title="Project actions"
        onclick={() => (open ? hide() : show())}>
  <Icon name="ellipsis" size={16} />
</button>

{#if open}
  <!-- position:fixed in a top-level layer, NOT absolute inside the card: the
       tile clips its own content (overflow:hidden, for the cover's rounded
       corners), so an absolutely positioned menu would be cut off at the card
       edge. Fixed also means the menu cannot widen a grid track. -->
  <!-- tabindex="-1" on both levels is the ARIA menu pattern: the container is
       programmatically focusable but out of the tab order, and the items are
       reached with the arrow keys (roving focus) rather than by tabbing
       through every one of them. -->
  <!-- The popover renders INSIDE the card/row that owns it, so a right-click on
       a menu item would otherwise bubble to that card's own contextmenu handler
       and yank the open menu over to the cursor. Swallowed here: preventDefault
       as well as stopPropagation, because stopping the bubble also stops the
       window-level handler that suppresses Chromium's menu. -->
  <div class="menu" role="menu" tabindex="-1" bind:this={menu} style={placement}
       aria-label="Actions for {project.name}" onkeydown={onMenuKeys}
       oncontextmenu={(e) => { e.preventDefault(); e.stopPropagation(); }}>
    {#each items as it (it.label)}
      <button class="item" class:danger={it.danger} class:sep={it.sep}
              type="button" role="menuitem"
              tabindex="-1" onclick={() => choose(it)}>{it.label}</button>
    {/each}
  </div>
{/if}

<style>
  .dots { width: 26px; height: 26px; display: grid; place-items: center;
          border: 0; border-radius: 4px; background: none; color: var(--text-dim);
          cursor: default;
          transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .dots:hover:not(:disabled), .dots[aria-expanded="true"] {
    color: var(--text); background: rgba(255, 255, 255, .07); }
  .dots:disabled { opacity: .5; }

  .menu { position: fixed; z-index: 30; width: 236px; padding: 5px;
          background: var(--surface); border: 1px solid var(--border);
          border-radius: var(--r-panel);
          box-shadow: 0 16px 40px rgba(0, 0, 0, .55);
          animation: rise var(--dur) var(--ease); }

  .item { display: block; width: 100%; text-align: left; font: inherit;
          font-size: 13px; padding: 8px 10px; border: 0; border-radius: 5px;
          background: none; color: var(--text); cursor: default;
          transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .item:hover { background: rgba(255, 255, 255, .06); }
  /* The divider lives on the item that OPENS the group, not on `danger`: the
     top corners square off so the hover wash does not float rounded against
     the line it sits under. */
  .sep { margin-top: 4px; border-top: 1px solid var(--border-soft);
         border-radius: 0 0 5px 5px; }
  /* Coloured on hover only, so an open menu is not a wall of red for an action
     that only edits a list. Matches Button's `danger` variant. */
  .danger { color: var(--text-muted); }
  .danger:hover { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
                  color: var(--fail); }

  @keyframes rise { from { opacity: 0; transform: translateY(-4px) } }
</style>
