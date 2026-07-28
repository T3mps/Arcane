<script lang="ts">
  import Icon from "$lib/components/Icon.svelte";
  import type { RecentProject } from "$lib/api";

  // The per-project action menu, shared by the tile and the list row. It owns
  // the ITEM LIST as well as the popover, deliberately: "what can you do to a
  // project" is one vocabulary, and defining it per layout is how the two
  // layouts end up offering different things.
  let { project, disabled = false, confirmDelete = true,
        onReveal, onRename, onArgs, onForget, onLocate, onDelete }: {
    project: RecentProject;
    disabled?: boolean;
    /** Whether Delete opens a confirmation. Only affects this item's LABEL. */
    confirmDelete?: boolean;
    onReveal: () => void;
    onRename: () => void;
    onArgs: () => void;
    /** Remove from the LIST only -- delete is the one that touches disk. */
    onForget: () => void;
    /** Repoint a MISSING project at its moved .arcproj. */
    onLocate: () => void;
    onDelete: () => void;
  } = $props();

  type Item = { label: string; run: () => void; danger?: boolean };
  // A MISSING project gets exactly the two items that still mean something:
  // repair the row or retire it. Reveal, Rename, arguments and Delete all act
  // on disk state that is not there, and a menu of disabled items would make
  // the user hunt for the one that works. Locate is hidden on healthy rows for
  // the same reason Unity hides it: repointing a project that resolves is not
  // an action, it is a mistake waiting to be offered.
  const items = $derived<Item[]>(project.missing
    ? [
        { label: "Locate…", run: onLocate },
        { label: "Remove from list", run: onForget },
      ]
    : [
        { label: "Show in Explorer", run: onReveal },
        { label: "Rename project…", run: onRename },
        { label: "Command-line arguments…", run: onArgs },
        // No ellipsis and no confirmation: this only edits the list, and the
        // entry comes back through Add. It sits directly above Delete so the
        // two reads -- "stop showing me this" and "erase the folder" -- are
        // adjacent but visibly different kinds of item.
        { label: "Remove from list", run: onForget },
        // Last and set apart -- the only irreversible one. The ellipsis is what
        // every other item here uses to mean "opens something first", so it is
        // DROPPED when confirmation is off: the item then deletes on the click,
        // and a label promising a dialog that will not appear is the worst
        // place in this app to be inaccurate.
        {
          label: confirmDelete ? "Delete project…" : "Delete project",
          run: onDelete,
          danger: true,
        },
      ]);

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

  function choose(item: Item) {
    // Close FIRST: several of these open a dialog, and leaving a popover behind
    // the scrim is how a stray click lands on a control the user cannot see.
    open = false;
    item.run();
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
      <button class="item" class:danger={it.danger} type="button" role="menuitem"
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
  /* Coloured on hover only, so an open menu is not a wall of red for an action
     that only edits a list. Matches Button's `danger` variant. */
  .danger { color: var(--text-muted); margin-top: 4px;
            border-top: 1px solid var(--border-soft); border-radius: 0 0 5px 5px; }
  .danger:hover { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
                  color: var(--fail); }

  @keyframes rise { from { opacity: 0; transform: translateY(-4px) } }
</style>
