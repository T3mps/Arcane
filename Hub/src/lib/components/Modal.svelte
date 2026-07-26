<script module lang="ts">
  // Unique per instance so two modals can never emit the same DOM id. A
  // hardcoded id was safe only as long as nothing opened two at once, which
  // nothing enforced.
  let seq = 0;
</script>

<script lang="ts">
  import { onMount, type Snippet } from "svelte";

  // Generic modal shell: scrim, panel, title, and the keyboard contract.
  // Content and footer are snippets so the shell owns none of the form logic.
  let { title, onClose, children, footer }: {
    title: string;
    onClose: () => void;
    children: Snippet;
    footer?: Snippet;
  } = $props();

  const titleId = `modal-title-${seq++}`;

  // A PLAIN variable, not $state: nothing needs to re-render when it is
  // assigned, and making it reactive is what previously dragged `panel` into
  // the focus effect's dependency set -- so the effect ran twice (once before
  // bind:this, once after) and its cleanup fired a focus round-trip in
  // between, re-capturing the restore target on the way through.
  let panel: HTMLElement | null = null;

  const FOCUSABLE =
    'button:not(:disabled), input:not(:disabled), select:not(:disabled), ' +
    'textarea:not(:disabled), [href], [tabindex]:not([tabindex="-1"])';

  const focusables = () =>
    panel ? [...panel.querySelectorAll<HTMLElement>(FOCUSABLE)] : [];

  // onMount, not $effect: this is a once-after-mount action with an
  // on-unmount undo, which is exactly onMount's contract. It runs after
  // bind:this has assigned `panel`, and it cannot re-enter.
  onMount(() => {
    // Whatever had focus when the modal opened. Without restoring it,
    // dismissing drops focus to <body> and a keyboard user loses their place.
    const opener = document.activeElement as HTMLElement | null;
    focusables()[0]?.focus();
    return () => opener?.focus();
  });

  function onkeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      e.preventDefault();
      onClose();
      return;
    }
    if (e.key !== "Tab") return;

    // Trap. A dialog you can Tab out of is modal in appearance only: focus
    // lands on the cards behind the scrim, which cannot be seen or clicked.
    const f = focusables();
    if (f.length === 0) return;
    const first = f[0];
    const last = f[f.length - 1];
    const active = document.activeElement;
    const outside = !panel?.contains(active);

    if (e.shiftKey && (active === first || outside)) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (active === last || outside)) {
      e.preventDefault();
      first.focus();
    }
  }
</script>

<svelte:window {onkeydown} />

<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- The scrim is a redundant MOUSE dismissal for an action already on two
     focusable controls (Escape and Cancel), so a keyboard user loses nothing
     and there is no sensible role for "the area outside the dialog". Same
     reasoning as WindowChrome's double-click-to-maximize. -->
<div class="scrim" onclick={onClose}></div>

<div class="wrap">
  <div class="panel" bind:this={panel} role="dialog" aria-modal="true" aria-labelledby={titleId}>
    <h2 class="display" id={titleId}>{title}</h2>
    <div class="body">{@render children()}</div>
    {#if footer}<div class="foot">{@render footer()}</div>{/if}
  </div>
</div>

<style>
  /* Starts BELOW the titlebar, not at inset:0. With decorations:false the Hub
     owns minimize/maximize/close and the drag region, so a full-cover scrim
     would leave the user unable to move or close the window while a dialog is
     open -- which no decorated app does. Tab is still trapped in the panel, so
     a keyboard user cannot reach the chrome; this is a mouse affordance. */
  .scrim { position: fixed; inset: var(--h-titlebar) 0 0 0; z-index: 20;
           background: rgba(0, 0, 0, .6); backdrop-filter: blur(2px);
           animation: fade var(--dur) var(--ease); }
  .wrap { position: fixed; inset: var(--h-titlebar) 0 0 0; z-index: 21; display: grid;
          place-items: center; padding: 24px; pointer-events: none; }
  .panel { pointer-events: auto; width: min(470px, 100%);
           max-height: calc(100vh - var(--h-titlebar) - 48px); overflow-y: auto;
           background: var(--surface); border: 1px solid var(--border);
           border-radius: var(--r-panel); padding: 18px 20px 16px;
           box-shadow: 0 24px 60px rgba(0, 0, 0, .55);
           animation: rise var(--dur) var(--ease); }

  h2 { font-size: 17px; margin: 0 0 16px; color: var(--text-bright); }
  .body { display: flex; flex-direction: column; gap: 13px; }
  .foot { display: flex; justify-content: flex-end; gap: 8px; margin-top: 20px; }

  @keyframes fade { from { opacity: 0 } }
  @keyframes rise { from { opacity: 0; transform: translateY(6px) } }
</style>
