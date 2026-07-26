<script lang="ts">
  import type { Snippet } from "svelte";
  // `primary` is the single primary action per view; `ghost` is everything
  // secondary; `danger` is destructive (Remove) and only ever coloured on hover
  // so a list of rows is not a wall of red.
  let {
    variant = "ghost",
    disabled = false,
    title = "",
    onclick,
    children,
  }: {
    variant?: "primary" | "ghost" | "danger";
    disabled?: boolean;
    title?: string;
    onclick?: () => void;
    children: Snippet;
  } = $props();
</script>

<button class="btn {variant}" type="button" {disabled} {title} {onclick}>{@render children()}</button>

<style>
  .btn {
    font: inherit; font-size: 13px; font-weight: 600; cursor: default;
    border-radius: var(--r-btn); padding: 10px 16px; border: 1px solid transparent;
    transition: background var(--dur) var(--ease), border-color var(--dur) var(--ease),
                color var(--dur) var(--ease);
  }
  .btn:disabled { opacity: .4; }

  /* White ink, not the near-black the gold fill used: this accent is dark, so
     the two are the other way round. */
  .primary { background: var(--accent); color: var(--accent-ink); }        /* 6.1:1 */
  .primary:hover:not(:disabled) { background: var(--accent-hover); }      /* 5.0:1 */

  .ghost { background: rgba(255, 255, 255, .05); color: var(--text);
           border-color: var(--border); }
  .ghost:hover:not(:disabled) { background: rgba(255, 255, 255, .09);
                                border-color: var(--border-hover); }

  .danger { background: none; color: var(--text-dim); }
  .danger:hover:not(:disabled) { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
                                 color: var(--fail); }
</style>
