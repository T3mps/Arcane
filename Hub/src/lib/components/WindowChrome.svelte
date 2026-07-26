<script lang="ts">
  import { getCurrentWindow } from "@tauri-apps/api/window";
  // decorations:false means we own minimize/maximize/close and the drag region.
  // Buttons are children WITHOUT data-tauri-drag-region, which is how Tauri's
  // handler knows not to start a drag from them.
  const win = getCurrentWindow();
</script>

<div class="chrome" data-tauri-drag-region ondblclick={() => win.toggleMaximize()}>
  <img class="mark" src="/logo.png" alt="" />
  <span class="title display">Arcane Hub</span>
  <div class="ctrls">
    <button class="ctrl" aria-label="Minimize" onclick={() => win.minimize()}>
      <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
        <rect x="1" y="5" width="9" height="1" fill="currentColor" />
      </svg>
    </button>
    <button class="ctrl" aria-label="Maximize" onclick={() => win.toggleMaximize()}>
      <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
        <rect x="1.5" y="1.5" width="8" height="8" fill="none"
              stroke="currentColor" stroke-width="1" />
      </svg>
    </button>
    <button class="ctrl close" aria-label="Close" onclick={() => win.close()}>
      <svg width="11" height="11" viewBox="0 0 11 11" aria-hidden="true">
        <path d="M1 1l9 9M10 1l-9 9" stroke="currentColor" stroke-width="1.2" />
      </svg>
    </button>
  </div>
  <div class="rule"></div>
</div>

<style>
  .chrome { position: sticky; top: 0; z-index: 10; height: 34px; flex: none;
            display: flex; align-items: center; gap: 9px; padding: 0 6px 0 13px; }
  /* pointer-events:none so the logo and title are part of the drag region
     rather than dead spots in the middle of it. */
  .mark { width: 15px; height: 15px; object-fit: contain; pointer-events: none; }
  .title { font-size: 11px; letter-spacing: .19em; color: var(--text-muted);
           pointer-events: none; }
  .ctrls { margin-left: auto; display: flex; gap: 2px; }
  .ctrl { width: 34px; height: 26px; display: grid; place-items: center;
          background: transparent; border: 0; border-radius: 5px;
          color: var(--text-dim); cursor: default;
          transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .ctrl:hover { background: rgba(255, 255, 255, .07); color: var(--text); }
  .close:hover { background: color-mix(in srgb, var(--fail-accent) 85%, transparent); color: #fff; }
  .rule { position: absolute; left: 0; right: 0; bottom: 0; height: 1px;
          background: linear-gradient(90deg, transparent, color-mix(in srgb, var(--gold) 50%, transparent), transparent); }
</style>
