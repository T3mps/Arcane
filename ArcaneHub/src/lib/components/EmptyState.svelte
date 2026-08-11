<script lang="ts">
  import type { Snippet } from "svelte";
  let { title, body = "", children }:
    { title: string; body?: string; children?: Snippet } = $props();
</script>

<div class="empty">
  <p class="t display">{title}</p>
  {#if body}<p class="b">{body}</p>{/if}
  {#if children}<div class="a">{@render children()}</div>{/if}
</div>

<style>
  .empty { border: 1px dashed var(--border); border-radius: var(--r-panel);
           padding: 40px 30px; text-align: center; background: rgba(255, 255, 255, .015); }
  .t { font-size: 16px; letter-spacing: .1em; color: var(--text-muted); margin: 0; }
  .b { font-size: 13.5px; color: var(--text-dim); line-height: 1.6;
       margin: 10px auto 0; max-width: 46ch; }
  .a { margin-top: 16px; display: flex; gap: 8px; justify-content: center; }
</style>
