<script module lang="ts">
  // One popover vocabulary for every dropdown-shaped control (the Add button,
  // the grid sorter) -- extracted from ProjectsView when the sorter's native
  // <select> was replaced: the OS popup could only be color-scheme'd into
  // cooperation, never styled to match, and two inline copies of the same
  // mechanics in one file is how they drift apart.
  export type DropdownItem = {
    label: string;
    /** The current choice in a pick-one menu -- checked and announced. When
     * any item carries this, every item reserves the check slot so labels
     * stay aligned, and the whole menu speaks menuitemradio. */
    active?: boolean;
    disabled?: boolean;
    title?: string;
    onselect: () => void;
  };
</script>

<script lang="ts">
  import type { Snippet } from "svelte";
  import Icon from "$lib/components/Icon.svelte";

  // The trigger is a snippet, not a baked-in button: Add wants a full ghost
  // Button, the sorter wants a bare pill-resident control, and both must
  // carry the menu-pattern ARIA -- so the snippet receives {open, toggle,
  // aria} and spreads them onto whatever it renders.
  let { items, ariaLabel, width = 200, trigger }: {
    items: DropdownItem[];
    ariaLabel: string;
    /** Menu width in px; the default matches ProjectMenu's proportions. */
    width?: number;
    trigger: Snippet<[{ open: boolean; toggle: () => void; aria: Record<string, unknown> }]>;
  } = $props();

  let open = $state(false);
  let wrap = $state<HTMLElement | null>(null);

  const radio = $derived(items.some((i) => i.active !== undefined));

  const buttons = () =>
    wrap ? [...wrap.querySelectorAll<HTMLButtonElement>(".menu button")] : [];

  // First ENABLED item, not first item: focusing a disabled button no-ops.
  $effect(() => {
    if (open) buttons().find((b) => !b.disabled)?.focus();
  });

  function close(restoreFocus = true) {
    if (!open) return;
    open = false;
    if (restoreFocus) wrap?.querySelector("button")?.focus();
  }

  function choose(it: DropdownItem) {
    // Close FIRST, same rule as ProjectMenu: an action may open a dialog, and
    // a popover left behind the scrim catches clicks the user cannot see.
    open = false;
    it.onselect();
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
      // Menu, not dialog: Tab dismisses. Focus returns to the trigger because
      // the items unmount on this keystroke (ProjectMenu's rule).
      case "Tab":
        e.preventDefault();
        close();
        break;
    }
  }

  function onWindowKey(e: KeyboardEvent) {
    if (open && e.key === "Escape") {
      e.preventDefault();
      close();
    }
  }

  function onWindowPointerDown(e: PointerEvent) {
    if (open && !wrap?.contains(e.target as Node)) close(false);
  }
</script>

<svelte:window onkeydown={onWindowKey} onpointerdown={onWindowPointerDown} />

<div class="wrap" bind:this={wrap}>
  {@render trigger({
    open,
    toggle: () => (open ? close() : (open = true)),
    aria: { "aria-haspopup": "menu", "aria-expanded": open },
  })}
  {#if open}
    <div class="menu" role="menu" tabindex="-1" aria-label={ariaLabel}
         onkeydown={onMenuKeys} style="width:{width}px">
      {#each items as it (it.label)}
        <button class="item" type="button" tabindex="-1"
                role={radio ? "menuitemradio" : "menuitem"}
                aria-checked={radio ? !!it.active : undefined}
                disabled={it.disabled} title={it.title}
                onclick={() => choose(it)}>
          {#if radio}<span class="slot">{#if it.active}<Icon name="check" size={13} />{/if}</span>{/if}
          {it.label}
        </button>
      {/each}
    </div>
  {/if}
</div>

<style>
  /* inline-flex, not block: the wrapper sits in toolbar rows and pills, and
     must shrink-wrap its trigger rather than take the row's width. */
  .wrap { position: relative; display: inline-flex; }

  /* Absolute under the trigger, not ProjectMenu's fixed layer: every call
     site lives in chrome that neither clips nor scrolls, so the anchor
     cannot go stale. Left edges aligned, dropdown-button style (user call
     2026-07-29). Surface tokens match ProjectMenu's .menu so the popovers
     read as one species. */
  .menu { position: absolute; z-index: 30; top: calc(100% + 4px); left: 0;
          padding: 5px;
          background: var(--surface); border: 1px solid var(--border);
          border-radius: var(--r-panel);
          box-shadow: 0 16px 40px rgba(0, 0, 0, .55);
          animation: rise var(--dur) var(--ease); }

  .item { display: flex; align-items: center; gap: 7px; width: 100%;
          text-align: left; font: inherit; font-size: 13px; padding: 8px 10px;
          border: 0; border-radius: 5px; background: none; color: var(--text);
          cursor: default; transition: background var(--dur) var(--ease); }
  .item:hover:not(:disabled) { background: rgba(255, 255, 255, .06); }
  /* Present but honest -- the title says what to do about it. */
  .item:disabled { color: var(--text-dim); opacity: .6; }

  /* Reserved on every item of a pick-one menu, so the labels align whether
     or not the check is there. */
  .slot { width: 14px; display: grid; place-items: center; flex: none; }

  @keyframes rise { from { opacity: 0; transform: translateY(-4px) } }
</style>
