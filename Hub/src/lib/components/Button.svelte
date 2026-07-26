<script lang="ts">
  import type { Snippet } from "svelte";
  // `gold` is the single primary action per view; `ghost` is everything
  // secondary; `danger` is destructive (Remove) and only ever coloured on hover
  // so a list of rows is not a wall of red.
  let {
    variant = "ghost",
    disabled = false,
    title = "",
    onclick,
    children,
  }: {
    variant?: "gold" | "ghost" | "danger";
    disabled?: boolean;
    title?: string;
    onclick?: () => void;
    children: Snippet;
  } = $props();
</script>

<button class="btn {variant}" type="button" {disabled} {title} {onclick}>{@render children()}</button>

<style>
  .btn {
    font: inherit; font-size: 12px; font-weight: 600; cursor: default;
    border-radius: var(--r-btn); padding: 8px 14px; border: 1px solid transparent;
    transition: background var(--dur) var(--ease), border-color var(--dur) var(--ease),
                color var(--dur) var(--ease);
  }
  .btn:disabled { opacity: .4; }

  .gold { background: var(--gold); color: #120e04; }          /* 12.1:1 */
  .gold:hover:not(:disabled) { background: var(--gold-bright); }

  .ghost { background: rgba(255, 255, 255, .05); color: var(--text);
           border-color: var(--border); }
  .ghost:hover:not(:disabled) { background: rgba(255, 255, 255, .09);
                                border-color: #2d3750; }

  .danger { background: none; color: var(--text-dim); }
  .danger:hover:not(:disabled) { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
                                 color: var(--fail); }
</style>
