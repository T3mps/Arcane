# Setup Wizard Visual & UX Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-skin the Tauri 2 + SvelteKit setup wizard into an on-brand "premium launcher" (Direction D: deep-space starfield + condensed Aldo titles, gold-forward calm surfaces) with real UX upgrades, without touching setup behavior.

**Architecture:** Pure presentation change. A global design-system stylesheet + bundled Aphelyon fonts + reusable Svelte components drive a frameless 4-screen flow (Prerequisites -> Options -> Install -> Result). The `@@WIZ` contract, `setup.ps1`, the Rust parser (`orchestrator.rs`), and the reducer (`wizard.svelte.ts`) are unchanged; the only Rust addition is a small `open_solution` command.

**Tech Stack:** Svelte 5 (runes) + SvelteKit (SPA, `ssr=false`), Tauri 2 (Rust), `@tauri-apps/api` (window controls) + `@tauri-apps/plugin-opener` (install links), Vitest (helper tests), `cargo test` (parser unchanged).

**Spec:** `docs/superpowers/specs/2026-06-13-setup-wizard-visual-ux-design.md`.

---

## Build-time environment (read once; applies to every task)

- **Project dir:** `Tools/setup-wizard/` (capital `T` -- that is the git-tracked path). Run `npm` from there.
- **Rust/cargo is NOT on the shell PATH** -- it is at `%USERPROFILE%\.cargo\bin` (Rust 1.96 stable MSVC). For ANY cargo command prepend it: Bash `export PATH="$HOME/.cargo/bin:$PATH"`; PowerShell `$env:Path = "$env:USERPROFILE\.cargo\bin;$env:Path"`. Run cargo from `Tools/setup-wizard/src-tauri/`. Node/npm ARE on PATH.
- **NEVER run `npm run tauri dev` / `cargo tauri dev`** -- it opens a blocking GUI window and hangs the tool call. **NEVER run the release `npm run tauri build` in the foreground** (10+ min) -- only Task 11 builds the exe, and it runs in the background.
- **Headless verification only:** `npm run test` (Vitest), `npm run build` (SvelteKit), `npm run check` (svelte-check), `cargo test`, `cargo build` (debug) -- all terminate. The visual look is a human acceptance gate (cannot be done headlessly).
- **Never** run `db-reset.bat`, `clean.bat --deep`, or `docker compose down -v`.
- **Encoding:** UTF-8 no BOM; ASCII-only in authored source. Use inline SVG / CSS for icons (check / `!` / `x`), NOT unicode glyphs, so sources stay ASCII and icons stay crisp. The `.ttf` font files are binary (`.gitattributes` already marks `*.ttf binary`).
- **Commit per task**, `type(scope):` subject + the `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer. Stage only the task's files; never stage the unrelated untracked `Server/cpp_coding_style.txt`.
- **API-adaptation rule:** Tauri 2 / Svelte 5 / plugin API snippets below are written from current conventions; if the scaffolded versions differ, adapt the import path / permission identifier / call shape and keep the behavior identical, recording the deviation in the commit body.

## Testing approach (why not everything is TDD)

The pure logic gets TDD: `lib/steps.ts` and `lib/links.ts` (Task 4) ship with failing-first Vitest cases. Svelte components have no unit-test harness in this project (adding `@testing-library/svelte` + jsdom is out of scope), so they are verified by `npm run build` (compiles) + `npm run check` (0 errors) + the human visual gate -- the spec endorses this split. The existing suites (`wizard.test.ts` 6 cases, `cargo test` 5 cases) MUST stay green after every task.

## File structure

```
Tools/setup-wizard/
  static/fonts/AldotheApache.ttf                 NEW (copied from Client/data/font/)
  static/fonts/PF-Din-Text-Universal-Medium.ttf  NEW (copied)
  src/lib/theme.css                              NEW  design tokens + @font-face + base + motion
  src/lib/steps.ts                               NEW  expectedSteps(args) -> string[]   (TESTED)
  src/lib/steps.test.ts                          NEW
  src/lib/links.ts                               NEW  installLink(item) -> string|null  (TESTED)
  src/lib/links.test.ts                          NEW
  src/lib/components/WindowChrome.svelte         NEW  title bar: drag + wordmark + min/close
  src/lib/components/Starfield.svelte            NEW  background canvas/CSS starfield + nebula
  src/lib/components/StepRail.svelte             NEW  3-step progress indicator
  src/lib/components/StatusRow.svelte            NEW  one prereq/checklist row
  src/lib/components/LogPane.svelte              NEW  auto-scroll colorized log
  src/lib/components/Button.svelte               NEW  primary/ghost/danger button
  src/lib/screens/Doctor.svelte                  REWRITE  Prerequisites
  src/lib/screens/Options.svelte                 REWRITE  Options + Advanced disclosure
  src/lib/screens/Run.svelte                     REWRITE  Install (checklist + progress + log)
  src/lib/screens/Result.svelte                  NEW  Success / Failure terminal
  src/routes/+page.svelte                        REWRITE  shell + 4-screen router + args handoff
  src-tauri/tauri.conf.json                      MODIFY  decorations:false, 760x600
  src-tauri/capabilities/default.json            MODIFY  window + opener-url permissions
  src-tauri/src/lib.rs                           MODIFY  + open_solution() command
Setup.exe                                        REBUILT (Task 11)
```

---

### Task 1: Design tokens + bundled fonts (`theme.css`)

**Files:**
- Create: `Tools/setup-wizard/static/fonts/AldotheApache.ttf`, `Tools/setup-wizard/static/fonts/PF-Din-Text-Universal-Medium.ttf`
- Create: `Tools/setup-wizard/src/lib/theme.css`

- [ ] **Step 1: Copy the real fonts (binary)**

Run (Bash):
```bash
mkdir -p Tools/setup-wizard/static/fonts
cp Client/data/font/AldotheApache.ttf Tools/setup-wizard/static/fonts/AldotheApache.ttf
cp "Client/data/font/PF-Din-Text-Universal-Medium.ttf" Tools/setup-wizard/static/fonts/PF-Din-Text-Universal-Medium.ttf
ls -la Tools/setup-wizard/static/fonts/
```
Expected: both `.ttf` files present, non-zero size.

- [ ] **Step 2: Write `src/lib/theme.css`**

```css
/* Aphelyon Setup -- design tokens (Direction D), bundled fonts, base styles, motion.
   Imported once by src/routes/+page.svelte. Tokens come from Client/src/ui/theme.lua,
   gold-forward per the approved Direction D. ASCII-only. */

@font-face {
  font-family: "Aldo";
  src: url("/fonts/AldotheApache.ttf") format("truetype");
  font-weight: 400 700; font-display: swap;
}
@font-face {
  font-family: "DIN";
  src: url("/fonts/PF-Din-Text-Universal-Medium.ttf") format("truetype");
  font-weight: 400 600; font-display: swap;
}

:root {
  --bg-top: #0a0e17; --bg-bottom: #080b12;
  --surface: #141927; --surface-2: #10151f; --well: #06090f;
  --border: #222a3b; --border-soft: #1c2230;
  --text: #eef2fa; --text-muted: #8593aa; --text-dim: #5d6a82;
  --gold: #f0c869; --gold-bright: #ffd161;
  --ok: #74dca2; --warn: #f0c869; --fail: #f1949f; --fail-accent: #ff5c6b;
  --nebula-cyan: rgba(80,150,200,.10); --nebula-violet: rgba(180,120,210,.07);

  --font-display: "Aldo", "Bahnschrift", "DIN Condensed", "Oswald", sans-serif;
  --font-ui: "DIN", ui-sans-serif, system-ui, sans-serif;
  --font-mono: ui-monospace, "Cascadia Code", Consolas, monospace;

  --r-win: 12px; --r-panel: 8px; --r-btn: 7px;
  --dur: 200ms; --ease: cubic-bezier(.22,.61,.36,1);
}

* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
body {
  font-family: var(--font-ui); color: var(--text); font-size: 13px; line-height: 1.5;
  background: var(--bg-bottom);
  -webkit-font-smoothing: antialiased;
  user-select: none; cursor: default;
  overflow: hidden;
}

h1, h2, .display { font-family: var(--font-display); text-transform: uppercase; letter-spacing: .06em; }

.title { font-family: var(--font-display); text-transform: uppercase; letter-spacing: .07em;
         font-size: 26px; font-weight: 600; color: #f3f6fc; margin: 0;
         text-shadow: 0 0 22px rgba(240,205,130,.22); }
.subtitle { color: var(--text-muted); font-size: 12px; margin: 3px 0 0; }
.label { font-size: 10px; letter-spacing: .14em; text-transform: uppercase; color: var(--text-muted); }

/* focus ring */
:focus-visible { outline: 2px solid rgba(240,200,105,.7); outline-offset: 2px; border-radius: 4px; }

/* shared motion helpers */
@keyframes aph-pulse { 0%,100% { opacity:.55 } 50% { opacity:1 } }
@keyframes aph-spin  { to { transform: rotate(360deg) } }
@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after { animation-duration: .001ms !important; animation-iteration-count: 1 !important;
                           transition-duration: .001ms !important; }
}
```

- [ ] **Step 3: Verify the build picks up fonts + CSS compiles**

Run (from `Tools/setup-wizard/`):
```
npm run build
```
Expected: exit 0. (The CSS is not yet imported by a route -- that happens in Task 10 -- so this only proves it is valid and the static fonts are bundled. `npm run check` is run in later tasks once components consume it.)

- [ ] **Step 4: Commit**

```bash
git add Tools/setup-wizard/static/fonts Tools/setup-wizard/src/lib/theme.css
git commit -m "feat(setup-wizard): design tokens + bundled Aldo/DIN fonts (Direction D theme.css)"
```

---

### Task 2: Frameless window + capabilities + WindowChrome

**Files:**
- Modify: `Tools/setup-wizard/src-tauri/tauri.conf.json`
- Modify: `Tools/setup-wizard/src-tauri/capabilities/default.json`
- Create: `Tools/setup-wizard/src/lib/components/WindowChrome.svelte`

- [ ] **Step 1: Frameless window in `tauri.conf.json`**

In `app.windows[0]`, set `decorations` to `false` and the size to 760x600. Keep the other keys the scaffold set. The window object becomes (merge -- keep `label`, `title`, etc.):
```json
{
  "title": "Aphelyon Setup",
  "width": 760,
  "height": 600,
  "resizable": false,
  "center": true,
  "decorations": false
}
```
(Do NOT set `transparent` yet -- rounded corners are an optional later nicety; square frameless ships fine.)

- [ ] **Step 2: Add window + opener permissions to `capabilities/default.json`**

The current `permissions` array is `["core:default","opener:default"]`. Add the three window controls and the opener-url permission. Result:
```json
{
  "$schema": "../gen/schemas/desktop-schema.json",
  "identifier": "default",
  "description": "Capability for the main window",
  "windows": ["main"],
  "permissions": [
    "core:default",
    "core:window:allow-minimize",
    "core:window:allow-close",
    "core:window:allow-start-dragging",
    "opener:default",
    "opener:allow-open-url"
  ]
}
```
If a permission identifier is rejected at build (plugin version mismatch), adapt to the scaffold's exact identifier (e.g. the opener plugin may expose `allow-open-url` under a different name) and record it in the commit body.

- [ ] **Step 3: Write `src/lib/components/WindowChrome.svelte`**

```svelte
<script lang="ts">
  import { getCurrentWindow } from '@tauri-apps/api/window';
  // onClose lets the parent intercept (e.g. cancel a running setup) before the
  // window actually closes. Defaults to a plain close.
  let { onClose }: { onClose?: () => void } = $props();
  const win = getCurrentWindow();
  function minimize() { win.minimize(); }
  function close() { if (onClose) onClose(); else win.close(); }
</script>

<div class="chrome" data-tauri-drag-region>
  <div class="brand"><span class="mark">APHELYON</span><span class="sub">&middot; Setup</span></div>
  <div class="ctrls">
    <button class="ctrl" aria-label="Minimize" onclick={minimize}>
      <svg width="11" height="11" viewBox="0 0 11 11"><rect x="1" y="5" width="9" height="1" fill="currentColor"/></svg>
    </button>
    <button class="ctrl close" aria-label="Close" onclick={close}>
      <svg width="11" height="11" viewBox="0 0 11 11"><path d="M1 1l9 9M10 1l-9 9" stroke="currentColor" stroke-width="1.2"/></svg>
    </button>
  </div>
  <div class="rule"></div>
</div>

<style>
  .chrome { position: relative; height: 34px; display: flex; align-items: center;
            justify-content: space-between; padding: 0 6px 0 14px; background: #0a0d15;
            border-bottom: 1px solid var(--border-soft); }
  .brand { display: flex; align-items: baseline; gap: 6px; pointer-events: none; }
  .mark { font-family: var(--font-display); letter-spacing: .22em; font-size: 12px; color: #cdd7e8; }
  .sub { font-size: 10px; letter-spacing: .14em; text-transform: uppercase; color: var(--text-dim); }
  .ctrls { display: flex; gap: 2px; }
  .ctrl { -webkit-app-region: no-drag; width: 30px; height: 24px; display: flex; align-items: center;
          justify-content: center; background: transparent; border: 0; border-radius: 5px;
          color: var(--text-muted); cursor: default; }
  .ctrl:hover { background: rgba(255,255,255,.07); color: var(--text); }
  .close:hover { background: rgba(255,92,107,.85); color: #fff; }
  .rule { position: absolute; left: 0; right: 0; bottom: -1px; height: 1px;
          background: linear-gradient(90deg, transparent, rgba(240,200,105,.55), transparent); }
</style>
```

- [ ] **Step 4: Verify it compiles (build + check)**

Run (from `Tools/setup-wizard/`):
```
npm run build
npm run check
```
Expected: build exit 0; `npm run check` 0 errors (the component is valid even though not yet mounted). If `check` flags the unused `getCurrentWindow` import path, adapt to the scaffold's `@tauri-apps/api` shape.

- [ ] **Step 5: Commit**

```bash
git add Tools/setup-wizard/src-tauri/tauri.conf.json Tools/setup-wizard/src-tauri/capabilities/default.json Tools/setup-wizard/src/lib/components/WindowChrome.svelte
git commit -m "feat(setup-wizard): frameless window + custom WindowChrome (drag, minimize, close) + window/opener capabilities"
```

---

### Task 3: Starfield background

**Files:**
- Create: `Tools/setup-wizard/src/lib/components/Starfield.svelte`

- [ ] **Step 1: Write `src/lib/components/Starfield.svelte`**

A low-cost canvas starfield with two static nebula glows; freezes on reduced-motion.

```svelte
<script lang="ts">
  import { onMount } from 'svelte';
  let canvas: HTMLCanvasElement;
  onMount(() => {
    const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    let raf = 0, w = 0, h = 0;
    type Star = { x: number; y: number; r: number; a: number; tw: number; vy: number };
    let stars: Star[] = [];
    function resize() {
      w = canvas.width = canvas.clientWidth; h = canvas.height = canvas.clientHeight;
      const n = Math.min(90, Math.floor((w * h) / 9000));
      stars = Array.from({ length: n }, () => ({
        x: Math.random() * w, y: Math.random() * h,
        r: Math.random() * 1.2 + 0.3, a: Math.random() * 0.6 + 0.2,
        tw: Math.random() * 0.02 + 0.004, vy: Math.random() * 0.04 + 0.01,
      }));
    }
    let t = 0;
    function frame() {
      ctx.clearRect(0, 0, w, h);
      for (const s of stars) {
        const a = s.a + Math.sin(t * s.tw * 60) * 0.18;
        ctx.globalAlpha = Math.max(0, Math.min(1, a));
        ctx.fillStyle = s.r > 1 ? '#ffe9c2' : '#dbeaff';
        ctx.beginPath(); ctx.arc(s.x, s.y, s.r, 0, Math.PI * 2); ctx.fill();
        if (!reduce) { s.y += s.vy; if (s.y > h) { s.y = 0; s.x = Math.random() * w; } }
      }
      ctx.globalAlpha = 1;
      if (!reduce) { t += 1; raf = requestAnimationFrame(frame); }
    }
    resize(); frame();
    if (reduce) { /* draw once, no loop */ }
    const ro = new ResizeObserver(() => { resize(); if (reduce) frame(); });
    ro.observe(canvas);
    return () => { cancelAnimationFrame(raf); ro.disconnect(); };
  });
</script>

<div class="field">
  <canvas bind:this={canvas}></canvas>
</div>

<style>
  .field { position: fixed; inset: 0; z-index: 0; pointer-events: none; overflow: hidden;
           background:
             radial-gradient(110% 80% at 80% -10%, var(--nebula-cyan), transparent 55%),
             radial-gradient(90% 70% at -8% 112%, var(--nebula-violet), transparent 55%),
             linear-gradient(var(--bg-top), var(--bg-bottom)); }
  canvas { width: 100%; height: 100%; display: block; }
</style>
```

- [ ] **Step 2: Verify compile**

Run (from `Tools/setup-wizard/`): `npm run check`
Expected: 0 errors.

- [ ] **Step 3: Commit**

```bash
git add Tools/setup-wizard/src/lib/components/Starfield.svelte
git commit -m "feat(setup-wizard): drifting starfield + nebula background (reduced-motion aware)"
```

---

### Task 4: Pure helpers `steps.ts` + `links.ts` (TDD)

**Files:**
- Create: `Tools/setup-wizard/src/lib/steps.ts`, `Tools/setup-wizard/src/lib/steps.test.ts`
- Create: `Tools/setup-wizard/src/lib/links.ts`, `Tools/setup-wizard/src/lib/links.test.ts`

- [ ] **Step 1: Write failing tests `src/lib/steps.test.ts`**

```ts
import { describe, it, expect } from 'vitest';
import { expectedSteps, type RunArgs } from './steps';

const base: RunArgs = { workspaces: ['server','arcane','client'], skipVcpkg: false, build: false };

describe('expectedSteps', () => {
  it('server+arcane full sequence', () => {
    expect(expectedSteps({ ...base, workspaces: ['server','arcane'] }))
      .toEqual(['doctor','server-vcpkg','server-generate','server-db','arcane-vcpkg','arcane-generate']);
  });
  it('skipVcpkg drops the vcpkg steps', () => {
    expect(expectedSteps({ ...base, workspaces: ['server','arcane'], skipVcpkg: true }))
      .toEqual(['doctor','server-generate','server-db','arcane-generate']);
  });
  it('client only', () => {
    expect(expectedSteps({ ...base, workspaces: ['client'] })).toEqual(['doctor','client']);
  });
  it('build appends a build step', () => {
    expect(expectedSteps({ ...base, workspaces: ['server'], build: true }))
      .toEqual(['doctor','server-vcpkg','server-generate','server-db','build']);
  });
});
```

- [ ] **Step 2: Write failing tests `src/lib/links.test.ts`**

```ts
import { describe, it, expect } from 'vitest';
import { installLink } from './links';

describe('installLink', () => {
  it('maps known prereqs to URLs', () => {
    expect(installLink('Visual Studio')).toContain('visualstudio.microsoft.com');
    expect(installLink('docker')).toContain('docker.com');
    expect(installLink('vcpkg')).toContain('github.com/microsoft/vcpkg');
  });
  it('returns null for unknown items', () => {
    expect(installLink('Account\\schema.sql')).toBeNull();
    expect(installLink('whatever')).toBeNull();
  });
});
```

- [ ] **Step 3: Run tests -- confirm they fail**

Run (from `Tools/setup-wizard/`): `npm run test`
Expected: FAIL (modules `./steps` and `./links` not found).

- [ ] **Step 4: Implement `src/lib/steps.ts`**

```ts
// Computes the ordered @@WIZ step ids a run will emit, from the chosen options.
// Mirrors the sequencing in scripts/setup.ps1 so the Install screen can show
// upcoming (pending) steps and an accurate progress fraction.
export interface RunArgs {
  workspaces: string[];
  skipVcpkg: boolean;
  build: boolean;
}

export function expectedSteps(a: RunArgs): string[] {
  const out: string[] = ['doctor'];
  if (a.workspaces.includes('server')) {
    if (!a.skipVcpkg) out.push('server-vcpkg');
    out.push('server-generate', 'server-db');
  }
  if (a.workspaces.includes('arcane')) {
    if (!a.skipVcpkg) out.push('arcane-vcpkg');
    out.push('arcane-generate');
  }
  if (a.workspaces.includes('client')) out.push('client');
  if (a.build) out.push('build');
  return out;
}
```

- [ ] **Step 5: Implement `src/lib/links.ts`**

```ts
// Maps a doctor prerequisite item name to an install/help URL, or null when
// there is nothing useful to link. Presentation only -- does not affect doctor.
const LINKS: Record<string, string> = {
  'Visual Studio': 'https://visualstudio.microsoft.com/',
  'docker': 'https://www.docker.com/products/docker-desktop/',
  'vcpkg': 'https://github.com/microsoft/vcpkg',
};

export function installLink(item: string): string | null {
  return LINKS[item] ?? null;
}
```

- [ ] **Step 6: Run tests -- confirm green**

Run (from `Tools/setup-wizard/`): `npm run test`
Expected: PASS -- the 6 existing reducer cases + the 4 `steps` + 2 `links` cases all pass.

- [ ] **Step 7: Commit**

```bash
git add Tools/setup-wizard/src/lib/steps.ts Tools/setup-wizard/src/lib/steps.test.ts Tools/setup-wizard/src/lib/links.ts Tools/setup-wizard/src/lib/links.test.ts
git commit -m "feat(setup-wizard): expectedSteps + installLink pure helpers (unit-tested)"
```

---

### Task 5: Building-block components (Button, StatusRow, LogPane, StepRail)

**Files:**
- Create: `Tools/setup-wizard/src/lib/components/Button.svelte`, `StatusRow.svelte`, `LogPane.svelte`, `StepRail.svelte`

- [ ] **Step 1: `Button.svelte`**

```svelte
<script lang="ts">
  let { variant = 'ghost', disabled = false, onclick, children }:
    { variant?: 'primary' | 'ghost' | 'danger'; disabled?: boolean; onclick?: () => void; children?: any } = $props();
</script>
<button class="btn {variant}" {disabled} onclick={onclick}>{@render children?.()}</button>
<style>
  .btn { font-family: var(--font-ui); font-size: 11px; font-weight: 600; letter-spacing: .03em;
         padding: 8px 18px; border-radius: var(--r-btn); cursor: default; border: 1px solid transparent;
         transition: background var(--dur) var(--ease), border-color var(--dur) var(--ease); }
  .btn:disabled { opacity: .45; }
  .ghost { background: #161c2a; color: #aab6cc; border-color: #283146; }
  .ghost:hover:not(:disabled) { background: #1c2334; border-color: #344056; }
  .primary { background: var(--gold); color: #1a1c22; border-color: var(--gold-bright);
             box-shadow: 0 4px 16px rgba(240,200,105,.28); }
  .primary:hover:not(:disabled) { background: var(--gold-bright); }
  .danger { background: #2a1620; color: #f1949f; border-color: #5a2330; }
  .danger:hover:not(:disabled) { background: #381a26; }
</style>
```

- [ ] **Step 2: `StatusRow.svelte`**

```svelte
<script lang="ts">
  // status: 'pass'|'warn'|'fail'|'running'|'pending' ; action optional (install link)
  let { name, detail = '', status, actionLabel, onAction }:
    { name: string; detail?: string; status: string; actionLabel?: string; onAction?: () => void } = $props();
</script>
<div class="row {status}">
  <span class="ico">
    {#if status === 'pass'}
      <svg width="12" height="12" viewBox="0 0 12 12"><path d="M2 6.5l2.5 2.5L10 3" stroke="currentColor" stroke-width="1.6" fill="none"/></svg>
    {:else if status === 'fail'}
      <svg width="11" height="11" viewBox="0 0 11 11"><path d="M2 2l7 7M9 2l-7 7" stroke="currentColor" stroke-width="1.5"/></svg>
    {:else if status === 'running'}
      <span class="spin"></span>
    {:else if status === 'warn'}
      <b>!</b>
    {/if}
  </span>
  <div class="txt"><b>{name}</b>{#if detail}<span>{detail}</span>{/if}</div>
  {#if actionLabel && onAction}<button class="act" onclick={onAction}>{actionLabel}</button>{/if}
</div>
<style>
  .row { display: flex; align-items: center; gap: 11px; padding: 9px 11px; border-radius: var(--r-panel);
         margin-bottom: 8px; background: var(--surface); border: 1px solid var(--border); }
  .row.pending { opacity: .45; }
  .ico { width: 19px; height: 19px; border-radius: 50%; display: flex; align-items: center;
         justify-content: center; flex-shrink: 0; font-size: 11px; font-weight: 700; }
  .pass .ico { background: #173527; color: var(--ok); }
  .warn .ico { background: #352c16; color: var(--warn); }
  .fail .ico { background: #3a1c22; color: var(--fail); }
  .running .ico { background: #15283a; color: var(--gold); }
  .txt { flex: 1; } .txt b { font-weight: 600; color: var(--text); } .txt span { display: block; font-size: 10px; color: var(--text-muted); }
  .act { -webkit-app-region: no-drag; font-size: 10px; color: var(--gold); background: transparent;
         border: 1px solid rgba(240,200,105,.4); border-radius: 5px; padding: 4px 9px; cursor: default; }
  .act:hover { background: rgba(240,200,105,.12); }
  .spin { width: 12px; height: 12px; border-radius: 50%; border: 1.6px solid rgba(240,200,105,.25);
          border-top-color: var(--gold); animation: aph-spin .8s linear infinite; }
</style>
```

- [ ] **Step 3: `LogPane.svelte`**

```svelte
<script lang="ts">
  let { lines = [] }: { lines?: string[] } = $props();
  let el: HTMLDivElement | undefined = $state();
  // auto-scroll to bottom whenever lines change
  $effect(() => { void lines.length; if (el) el.scrollTop = el.scrollHeight; });
  function cls(l: string): string {
    if (/^\s*\[!\]|error|fail/i.test(l)) return 'err';
    if (/^\s*\[x\]|generated|complete|ok\b/i.test(l)) return 'ok';
    if (/^\s*>/.test(l)) return 'mut';
    return '';
  }
</script>
<div class="log" bind:this={el}>
  {#each lines.slice(-300) as l}<div class={cls(l)}>{l}</div>{/each}
</div>
<style>
  .log { background: var(--well); border: 1px solid #1a2130; border-radius: var(--r-panel);
         padding: 10px 11px; font-family: var(--font-mono); font-size: 11px; line-height: 1.55;
         color: #9fb0c8; height: 150px; overflow-y: auto; }
  .log .ok { color: var(--ok); } .log .err { color: var(--fail); } .log .mut { color: var(--text-dim); }
</style>
```

- [ ] **Step 4: `StepRail.svelte`**

```svelte
<script lang="ts">
  // current: 'doctor'|'options'|'run'|'result' -> map to 1/2/3 with done states
  let { current }: { current: string } = $props();
  const steps = [
    { id: 'doctor', n: 1, label: 'Prerequisites' },
    { id: 'options', n: 2, label: 'Options' },
    { id: 'run', n: 3, label: 'Install' },
  ];
  const order = ['doctor', 'options', 'run', 'result'];
  function state(id: string): 'done' | 'on' | 'up' {
    const ci = order.indexOf(current), si = order.indexOf(id);
    if (current === 'result') return 'done';
    return si < ci ? 'done' : si === ci ? 'on' : 'up';
  }
</script>
<div class="rail">
  {#each steps as s}
    {@const st = state(s.id)}
    <div class="step {st}">
      <span class="num">{#if st === 'done'}<svg width="11" height="11" viewBox="0 0 12 12"><path d="M2 6.5l2.5 2.5L10 3" stroke="currentColor" stroke-width="1.6" fill="none"/></svg>{:else}{s.n}{/if}</span>
      {s.label}
    </div>
  {/each}
</div>
<style>
  .rail { display: flex; gap: 18px; padding: 14px 20px 4px; }
  .step { font-size: 10px; letter-spacing: .14em; text-transform: uppercase; color: var(--text-dim);
          display: flex; align-items: center; gap: 7px; }
  .num { width: 19px; height: 19px; border-radius: 50%; border: 1px solid #2a3344; display: flex;
         align-items: center; justify-content: center; font-size: 10px; }
  .on { color: #f3d68a; }
  .on .num { border-color: rgba(240,200,105,.7); color: #1a1c22; background: var(--gold); font-weight: 700;
             box-shadow: 0 0 12px rgba(240,200,105,.4); }
  .done .num { border-color: #3a6f52; color: var(--ok); }
</style>
```

- [ ] **Step 5: Verify compile**

Run (from `Tools/setup-wizard/`): `npm run build` then `npm run check`
Expected: build exit 0; check 0 errors. (Svelte 5 `{@render children?.()}` + `$props()` snippets must compile; if the scaffold's Svelte version wants a different children/snippet shape, adapt and keep the API.)

- [ ] **Step 6: Commit**

```bash
git add Tools/setup-wizard/src/lib/components/Button.svelte Tools/setup-wizard/src/lib/components/StatusRow.svelte Tools/setup-wizard/src/lib/components/LogPane.svelte Tools/setup-wizard/src/lib/components/StepRail.svelte
git commit -m "feat(setup-wizard): reusable components - Button, StatusRow, LogPane, StepRail"
```

---

### Task 6: Prerequisites screen (`Doctor.svelte` rewrite)

**Files:**
- Modify (rewrite): `Tools/setup-wizard/src/lib/screens/Doctor.svelte`

- [ ] **Step 1: Rewrite `Doctor.svelte`**

Consumes `st` (WizState), gates Continue on `st.doctorOk`, shows a scanning state until rows arrive, and adds install-link buttons for known failing/warning items.

```svelte
<script lang="ts">
  import type { WizState } from '$lib/wizard.svelte';
  import { installLink } from '$lib/links';
  import { openUrl } from '@tauri-apps/plugin-opener';
  import StatusRow from '$lib/components/StatusRow.svelte';
  import Button from '$lib/components/Button.svelte';

  let { st, onContinue, onRecheck }:
    { st: WizState; onContinue: () => void; onRecheck: () => void } = $props();

  // Scanning while the doctor run is in flight and no rows have arrived yet
  // (the router resets st before invoking run_doctor) and we are not finished.
  const scanning = $derived(st.doctor.length === 0 && !st.finished);

  function open(item: string) { const u = installLink(item); if (u) openUrl(u); }
</script>

<h1 class="title">Prerequisites</h1>
<p class="subtitle">{scanning ? 'Scanning your environment...' : 'Checking your environment'}</p>

<div class="rows">
  {#if scanning}
    <div class="scan"><span class="orb"></span> Looking for Visual Studio, Docker, vcpkg...</div>
  {:else}
    {#each st.doctor as row (row.item)}
      <StatusRow name={row.item} detail={row.msg ?? ''} status={row.status}
        actionLabel={(row.status !== 'pass' && installLink(row.item)) ? 'Install' : undefined}
        onAction={() => open(row.item)} />
    {/each}
  {/if}
</div>

<div class="foot">
  {#if !st.doctorOk && !scanning}<span class="hint">Fix the red items, then re-check.</span>{/if}
  <Button variant="ghost" onclick={onRecheck}>Re-check</Button>
  <Button variant="primary" disabled={!st.doctorOk || scanning} onclick={onContinue}>Continue</Button>
</div>

<style>
  .rows { margin-top: 14px; min-height: 210px; }
  .scan { display: flex; align-items: center; gap: 10px; color: var(--text-muted); padding: 24px 6px; font-size: 12px; }
  .orb { width: 12px; height: 12px; border-radius: 50%; border: 1.6px solid rgba(240,200,105,.25);
         border-top-color: var(--gold); animation: aph-spin .8s linear infinite; }
  .foot { display: flex; align-items: center; justify-content: flex-end; gap: 10px; margin-top: 12px; }
  .hint { font-size: 10px; color: var(--fail); margin-right: auto; }
</style>
```

- [ ] **Step 2: Verify compile**

Run (from `Tools/setup-wizard/`): `npm run check`
Expected: 0 errors. If `@tauri-apps/plugin-opener` does not export `openUrl`, adapt to the scaffold's opener JS API (e.g. `open`) and keep the URL-open behavior; record the deviation.

- [ ] **Step 3: Commit**

```bash
git add Tools/setup-wizard/src/lib/screens/Doctor.svelte
git commit -m "feat(setup-wizard): Prerequisites screen - scanning state, status rows, install links, gated Continue"
```

---

### Task 7: Options screen (`Options.svelte` rewrite)

**Files:**
- Modify (rewrite): `Tools/setup-wizard/src/lib/screens/Options.svelte`

- [ ] **Step 1: Rewrite `Options.svelte`**

Emits the same camelCase args. Workspace cards, segmented config, an Advanced disclosure (collapsed), and a live summary.

```svelte
<script lang="ts">
  import Button from '$lib/components/Button.svelte';
  let { onBack, onRun }: { onBack: () => void; onRun: (args: unknown) => void } = $props();

  let server = $state(true), arcane = $state(true), client = $state(true);
  let vcpkgRoot = $state(''), dbPort = $state(5432), skipVcpkg = $state(false);
  let config = $state('Debug'), build = $state(false), advanced = $state(false);

  const workspaces = $derived([server && 'server', arcane && 'arcane', client && 'client'].filter(Boolean) as string[]);
  const summary = $derived(
    workspaces.length === 0 ? 'Select at least one workspace.'
      : `Will set up ${workspaces.join(' + ')}${skipVcpkg ? ' (skip vcpkg)' : ''} in ${config}${build ? ', then build' : ''}.`
  );

  function go() {
    if (workspaces.length === 0) return;
    onRun({ workspaces, vcpkgRoot: vcpkgRoot || null, dbPort: dbPort ?? 5432, skipVcpkg, config, build, doctorOnly: false });
  }
  const cards = [
    { key: 'server', label: 'Server', desc: 'Services + Postgres' },
    { key: 'arcane', label: 'Arcane', desc: 'Engine workspace' },
    { key: 'client', label: 'Client', desc: 'Love2D (no build yet)' },
  ];
  function toggle(k: string) { if (k === 'server') server = !server; if (k === 'arcane') arcane = !arcane; if (k === 'client') client = !client; }
  function on(k: string) { return k === 'server' ? server : k === 'arcane' ? arcane : client; }
</script>

<h1 class="title">Options</h1>
<p class="subtitle">Choose what to set up</p>

<div class="cards">
  {#each cards as c}
    <button class="ws {on(c.key) ? 'sel' : ''}" onclick={() => toggle(c.key)}>
      <span class="check">{#if on(c.key)}<svg width="11" height="11" viewBox="0 0 12 12"><path d="M2 6.5l2.5 2.5L10 3" stroke="currentColor" stroke-width="1.6" fill="none"/></svg>{/if}</span>
      <span class="wl">{c.label}</span><span class="wd">{c.desc}</span>
    </button>
  {/each}
</div>

<div class="cfg">
  <span class="label">Config</span>
  <div class="seg">
    {#each ['Debug','Release','Dist'] as c}<button class={config === c ? 'on' : ''} onclick={() => config = c}>{c}</button>{/each}
  </div>
  <label class="sw"><input type="checkbox" bind:checked={build} /> Build after generating</label>
</div>

<button class="adv" onclick={() => advanced = !advanced}>{advanced ? '-' : '+'} Advanced</button>
{#if advanced}
  <div class="advbox">
    <label>VCPKG_ROOT <input bind:value={vcpkgRoot} placeholder="auto-detect" /></label>
    <label>DB port <input type="number" min="1" max="65535" bind:value={dbPort} /></label>
    <label class="sw"><input type="checkbox" bind:checked={skipVcpkg} /> Skip vcpkg build</label>
  </div>
{/if}

<p class="sum">{summary}</p>
<div class="foot">
  <Button variant="ghost" onclick={onBack}>Back</Button>
  <Button variant="primary" disabled={workspaces.length === 0} onclick={go}>Run setup</Button>
</div>

<style>
  .cards { display: flex; gap: 10px; margin-top: 14px; }
  .ws { flex: 1; text-align: left; background: var(--surface); border: 1px solid var(--border);
        border-radius: var(--r-panel); padding: 11px; cursor: default; color: var(--text); position: relative; }
  .ws.sel { border-color: rgba(240,200,105,.55); box-shadow: 0 0 0 1px rgba(240,200,105,.18) inset; }
  .check { position: absolute; top: 9px; right: 9px; width: 16px; height: 16px; border-radius: 4px;
           display: flex; align-items: center; justify-content: center; color: var(--gold); border: 1px solid #2a3344; }
  .ws.sel .check { background: rgba(240,200,105,.14); border-color: rgba(240,200,105,.5); }
  .wl { display: block; font-weight: 600; font-size: 13px; } .wd { display: block; font-size: 10px; color: var(--text-muted); margin-top: 2px; }
  .cfg { display: flex; align-items: center; gap: 14px; margin: 16px 0 4px; }
  .seg { display: inline-flex; border: 1px solid var(--border); border-radius: 6px; overflow: hidden; }
  .seg button { background: transparent; color: var(--text-muted); border: 0; padding: 6px 12px; font-size: 11px; cursor: default; }
  .seg button.on { background: var(--gold); color: #1a1c22; font-weight: 600; }
  .sw { display: inline-flex; align-items: center; gap: 7px; font-size: 12px; color: var(--text-muted); }
  .adv { background: transparent; border: 0; color: var(--gold); font-size: 11px; padding: 12px 0 4px; cursor: default; }
  .advbox { display: flex; flex-direction: column; gap: 8px; background: var(--surface-2); border: 1px solid var(--border);
            border-radius: var(--r-panel); padding: 12px; }
  .advbox label { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--text-muted); }
  .advbox input[type="text"], .advbox input:not([type]) , .advbox input[type="number"] {
    background: var(--well); border: 1px solid var(--border); color: var(--text); border-radius: 5px; padding: 5px 8px; font: inherit; }
  .sum { font-size: 11px; color: var(--text-muted); margin: 14px 0 6px; }
  .foot { display: flex; justify-content: flex-end; gap: 10px; }
</style>
```

- [ ] **Step 2: Verify compile**

Run (from `Tools/setup-wizard/`): `npm run check`
Expected: 0 errors.

- [ ] **Step 3: Commit**

```bash
git add Tools/setup-wizard/src/lib/screens/Options.svelte
git commit -m "feat(setup-wizard): Options screen - workspace cards, segmented config, Advanced disclosure, live summary"
```

---

### Task 8: Install screen (`Run.svelte` rewrite)

**Files:**
- Modify (rewrite): `Tools/setup-wizard/src/lib/screens/Run.svelte`

- [ ] **Step 1: Rewrite `Run.svelte`**

Receives `st` AND the chosen `args` (passed by the router) so it can show the full expected step list + an accurate progress fraction.

```svelte
<script lang="ts">
  import type { WizState } from '$lib/wizard.svelte';
  import { expectedSteps, type RunArgs } from '$lib/steps';
  import LogPane from '$lib/components/LogPane.svelte';
  import StatusRow from '$lib/components/StatusRow.svelte';
  import Button from '$lib/components/Button.svelte';
  import { invoke } from '@tauri-apps/api/core';

  let { st, args }: { st: WizState; args: RunArgs } = $props();

  const labels: Record<string, string> = {
    doctor: 'Prerequisites', 'server-vcpkg': 'Server vcpkg deps', 'server-generate': 'Generate Server',
    'server-db': 'Postgres + schema', 'arcane-vcpkg': 'Arcane vcpkg deps', 'arcane-generate': 'Generate Arcane',
    client: 'Client', build: 'Build',
  };
  const expected = $derived(expectedSteps(args));
  const done = $derived(expected.filter((id) => st.steps[id] === 'ok').length);
  const pct = $derived(expected.length ? Math.round((done / expected.length) * 100) : 0);
  function rowStatus(id: string): string {
    const s = st.steps[id];
    return s === 'ok' ? 'pass' : s === 'fail' ? 'fail' : s === 'running' ? 'running' : 'pending';
  }
</script>

<h1 class="title">Installing</h1>
<p class="subtitle">Setting up {args.workspaces.join(' + ')}</p>

<div class="prog"><div class="fill" style="width:{pct}%"></div></div>

<div class="list">
  {#each expected as id}
    <StatusRow name={labels[id] ?? id} status={rowStatus(id)} />
  {/each}
</div>

<LogPane lines={st.log} />

{#if !st.finished}
  <div class="foot"><Button variant="danger" onclick={() => invoke('cancel')}>Cancel</Button></div>
{/if}

<style>
  .prog { height: 5px; border-radius: 3px; background: var(--surface-2); border: 1px solid var(--border);
          margin: 12px 0 12px; overflow: hidden; }
  .fill { height: 100%; border-radius: 3px; background: linear-gradient(90deg,#caa54a,var(--gold));
          box-shadow: 0 0 12px rgba(240,200,105,.5); transition: width var(--dur) var(--ease); }
  .list { max-height: 150px; overflow-y: auto; margin-bottom: 12px; }
  .foot { display: flex; justify-content: flex-end; margin-top: 12px; }
</style>
```

- [ ] **Step 2: Verify compile**

Run (from `Tools/setup-wizard/`): `npm run check`
Expected: 0 errors. (The router passes `args` in Task 10; this screen compiles standalone because `args` is a typed prop.)

- [ ] **Step 3: Commit**

```bash
git add Tools/setup-wizard/src/lib/screens/Run.svelte
git commit -m "feat(setup-wizard): Install screen - expected-step checklist, gold progress, live log, cancel"
```

---

### Task 9: `open_solution` Rust command + Result screen

**Files:**
- Modify: `Tools/setup-wizard/src-tauri/src/lib.rs`
- Create: `Tools/setup-wizard/src/lib/screens/Result.svelte`

- [ ] **Step 1: Add the `open_solution` command to `lib.rs`**

Add this command (it reuses the existing `repo_root()` helper) and register it. Place the function near `cancel`:

```rust
#[tauri::command]
fn open_solution() -> Result<(), String> {
    let sln = repo_root().join("Server").join("Aphelyon.slnx");
    if !sln.exists() {
        return Err(format!("not found: {}", sln.display()));
    }
    // Open with the default associated app (Visual Studio) via the shell.
    std::process::Command::new("cmd")
        .args(["/C", "start", "", &sln.to_string_lossy()])
        .spawn()
        .map_err(|e| e.to_string())?;
    Ok(())
}
```

Then add it to the handler list: change
```rust
.invoke_handler(tauri::generate_handler![run_setup, run_doctor, cancel])
```
to
```rust
.invoke_handler(tauri::generate_handler![run_setup, run_doctor, cancel, open_solution])
```

- [ ] **Step 2: Verify Rust still compiles + parser tests pass**

Run (cargo on PATH, from `Tools/setup-wizard/src-tauri/`):
```
cargo test
cargo build
```
Expected: `cargo test` 5 parser cases pass; `cargo build` exit 0.

- [ ] **Step 3: Write `src/lib/screens/Result.svelte`**

```svelte
<script lang="ts">
  import type { WizState } from '$lib/wizard.svelte';
  import Button from '$lib/components/Button.svelte';
  import { invoke } from '@tauri-apps/api/core';
  import { getCurrentWindow } from '@tauri-apps/api/window';

  let { st, onRetry }: { st: WizState; onRetry: () => void } = $props();
  const ok = $derived(st.finished === 'ok');
  let copied = $state(false);
  function copyLog() { navigator.clipboard.writeText(st.log.join('\n')).then(() => { copied = true; setTimeout(() => copied = false, 1500); }); }
  function openSln() { invoke('open_solution').catch(() => {}); }
  function close() { getCurrentWindow().close(); }
</script>

<div class="wrap {ok ? 'ok' : 'fail'}">
  <div class="badge">
    {#if ok}
      <svg width="34" height="34" viewBox="0 0 34 34"><circle cx="17" cy="17" r="15" fill="none" stroke="currentColor" stroke-width="1.5" opacity=".5"/><path d="M10 17.5l4.5 4.5L24 12" stroke="currentColor" stroke-width="2.2" fill="none"/></svg>
    {:else}
      <svg width="34" height="34" viewBox="0 0 34 34"><circle cx="17" cy="17" r="15" fill="none" stroke="currentColor" stroke-width="1.5" opacity=".5"/><path d="M11 11l12 12M23 11L11 23" stroke="currentColor" stroke-width="2.2"/></svg>
    {/if}
  </div>
  <h1 class="title">{ok ? 'Setup complete' : 'Setup failed'}</h1>
  {#if ok}
    <p class="subtitle">Open the solution and start building.</p>
    <ul class="next"><li>Open <b>Server/Aphelyon.slnx</b> in Visual Studio</li><li>Run <b>Server/scripts/start-all.bat</b>, then <b>Client/run.bat</b></li></ul>
  {:else}
    <p class="subtitle err">{st.error ?? 'A step did not complete. See the log.'}</p>
  {/if}
  <div class="foot">
    {#if ok}<Button variant="primary" onclick={openSln}>Open Aphelyon.slnx</Button>{/if}
    {#if !ok}<Button variant="primary" onclick={onRetry}>Retry</Button>{/if}
    <Button variant="ghost" onclick={copyLog}>{copied ? 'Copied' : 'Copy log'}</Button>
    <Button variant="ghost" onclick={close}>Close</Button>
  </div>
</div>

<style>
  .wrap { display: flex; flex-direction: column; align-items: center; text-align: center; padding: 18px 10px; }
  .badge { margin: 6px 0 10px; }
  .ok .badge { color: var(--ok); filter: drop-shadow(0 0 16px rgba(116,220,162,.45)); animation: aph-bloom .6s var(--ease); }
  .fail .badge { color: var(--fail); }
  .next { text-align: left; font-size: 12px; color: var(--text-muted); line-height: 1.8; margin: 10px 0 14px; padding-left: 18px; }
  .next b { color: var(--text); }
  .err { color: var(--fail); max-width: 520px; }
  .foot { display: flex; gap: 10px; margin-top: 10px; flex-wrap: wrap; justify-content: center; }
  @keyframes aph-bloom { 0% { transform: scale(.7); opacity: 0; } 60% { transform: scale(1.08); } 100% { transform: scale(1); opacity: 1; } }
</style>
```

- [ ] **Step 4: Verify compile + tests**

Run (from `Tools/setup-wizard/`): `npm run check` (0 errors). Run (from `src-tauri/`, cargo on PATH): `cargo test` (5 pass).

- [ ] **Step 5: Commit**

```bash
git add Tools/setup-wizard/src-tauri/src/lib.rs Tools/setup-wizard/src/lib/screens/Result.svelte
git commit -m "feat(setup-wizard): Result screen (success/failure) + open_solution Rust command"
```

---

### Task 10: Router + shell wiring (`+page.svelte` rewrite)

**Files:**
- Modify (rewrite): `Tools/setup-wizard/src/routes/+page.svelte`

- [ ] **Step 1: Rewrite `+page.svelte`**

Imports `theme.css`, mounts `Starfield` + `WindowChrome`, routes the 4 screens with a fade/fly transition, passes the chosen `args` to `Run`/`Result`, advances to `result` when `finished` is set, and on close cancels a running setup first. Keeps the existing `onMount` listener+cleanup and the invoke-reject -> `done=fail` guards.

```svelte
<script lang="ts">
  import '$lib/theme.css';
  import { onMount } from 'svelte';
  import { fly } from 'svelte/transition';
  import { listen } from '@tauri-apps/api/event';
  import { invoke } from '@tauri-apps/api/core';
  import { getCurrentWindow } from '@tauri-apps/api/window';
  import { reduce, initialState, type WizEvent, type WizState } from '$lib/wizard.svelte';
  import type { RunArgs } from '$lib/steps';
  import Starfield from '$lib/components/Starfield.svelte';
  import WindowChrome from '$lib/components/WindowChrome.svelte';
  import StepRail from '$lib/components/StepRail.svelte';
  import Doctor from '$lib/screens/Doctor.svelte';
  import Options from '$lib/screens/Options.svelte';
  import Run from '$lib/screens/Run.svelte';
  import Result from '$lib/screens/Result.svelte';

  let screen = $state<'doctor' | 'options' | 'run' | 'result'>('doctor');
  let st = $state<WizState>(initialState());
  // Store the FULL args object from the last run so Retry can replay it verbatim
  // (the Rust SetupArgs needs every field, incl. dbPort). The Run screen only
  // needs the RunArgs subset for expectedSteps, derived below.
  let lastArgs = $state<any>(null);
  const runArgs = $derived<RunArgs>(lastArgs
    ? { workspaces: lastArgs.workspaces, skipVcpkg: lastArgs.skipVcpkg, build: lastArgs.build }
    : { workspaces: ['server'], skipVcpkg: false, build: false });

  // advance to the result screen once the orchestrator reports done
  $effect(() => { if (st.finished && screen === 'run') screen = 'result'; });

  async function runDoctor() {
    st = initialState();
    try { await invoke('run_doctor'); }
    catch (err) { st = reduce(st, { kind: 'log', line: String(err) }); st = reduce(st, { kind: 'done', status: 'fail' }); }
  }
  async function runSetup(args: any) {
    lastArgs = args;
    st = { ...initialState() };
    screen = 'run';
    try { await invoke('run_setup', { args }); }
    catch (err) { st = reduce(st, { kind: 'log', line: String(err) }); st = reduce(st, { kind: 'done', status: 'fail' }); }
  }
  function retry() { if (lastArgs) runSetup(lastArgs); }

  function onClose() {
    const running = Object.values(st.steps).includes('running') && !st.finished;
    if (running) invoke('cancel').finally(() => getCurrentWindow().close());
    else getCurrentWindow().close();
  }

  onMount(() => {
    let un: (() => void) | undefined;
    listen<WizEvent>('wiz', (e) => { st = reduce(st, e.payload); }).then((u) => { un = u; runDoctor(); });
    return () => un?.();
  });
</script>

<svelte:window onkeydown={(e) => { if (e.key === 'Enter' && screen === 'doctor' && st.doctorOk) screen = 'options'; }} />

<Starfield />
<div class="app">
  <WindowChrome {onClose} />
  {#if screen !== 'result'}<StepRail current={screen} />{/if}
  <main>
    {#key screen}
      <div class="screen" in:fly={{ y: 10, duration: 200 }}>
        {#if screen === 'doctor'}
          <Doctor {st} onContinue={() => (screen = 'options')} onRecheck={runDoctor} />
        {:else if screen === 'options'}
          <Options onBack={() => (screen = 'doctor')} onRun={runSetup} />
        {:else if screen === 'run'}
          <Run {st} args={runArgs} />
        {:else}
          <Result {st} onRetry={retry} />
        {/if}
      </div>
    {/key}
  </main>
</div>

<style>
  .app { position: relative; z-index: 1; height: 100vh; display: flex; flex-direction: column; }
  main { flex: 1; overflow: hidden; padding: 8px 24px 22px; }
  .screen { height: 100%; }
</style>
```

Note: `retry()` replays the exact `lastArgs` object captured in `runSetup`, so the Rust `SetupArgs` gets every field (including `dbPort`); the Run screen's `runArgs` is a derived `RunArgs` subset of it for `expectedSteps`. Keyboard: the `<svelte:window onkeydown>` advances Prerequisites -> Options on Enter when `doctorOk`; the frameless window has no native Esc-to-close, so Esc is inert mid-run by default (the spec's keyboard requirement is met without an explicit Esc handler).

- [ ] **Step 2: Verify build + check + tests all green**

Run (from `Tools/setup-wizard/`):
```
npm run build
npm run check
npm run test
```
Expected: build exit 0; check 0 errors; test = 6 reducer + 4 steps + 2 links all pass.

- [ ] **Step 3: Commit**

```bash
git add Tools/setup-wizard/src/routes/+page.svelte
git commit -m "feat(setup-wizard): shell + 4-screen router (starfield, chrome, step rail, transitions, args handoff, cancel-on-close)"
```

---

### Task 11: Final verification + rebuild & recommit `Setup.exe`

**Files:**
- Modify: `Setup.exe` (repo root)

- [ ] **Step 1: Full headless verification**

Run (from `Tools/setup-wizard/`): `npm run build && npm run check && npm run test`
Expected: build 0; check 0 errors; all tests pass.
Run (cargo on PATH, from `src-tauri/`): `cargo test`
Expected: 5 parser cases pass.

- [ ] **Step 2: Reduced-motion audit (read, do not run a GUI)**

Confirm by reading the source that every animation/transition is covered by the `@media (prefers-reduced-motion: reduce)` block in `theme.css` (it neutralizes all `animation`/`transition` globally) and that `Starfield.svelte` checks `matchMedia('(prefers-reduced-motion: reduce)')` and does not loop when set. No code change unless a gap is found; if found, fix and note it.

- [ ] **Step 3: Rebuild the portable exe (BACKGROUND -- it is a long release build)**

Run **in the background** (cargo on PATH), from `Tools/setup-wizard/`:
```
npm run tauri build -- --no-bundle
```
Wait for completion (exit 0). Output: `Tools/setup-wizard/src-tauri/target/release/setup-wizard.exe`.

- [ ] **Step 4: Copy to repo root and commit**

```bash
cp Tools/setup-wizard/src-tauri/target/release/setup-wizard.exe Setup.exe
git add Setup.exe
git commit -m "build(setup-wizard): rebuild Setup.exe with the Direction D visual overhaul"
```

- [ ] **Step 5: Human acceptance gate (manual -- note for the user)**

Double-click the rebuilt `Setup.exe` and walk all four screens: scanning -> prereq rows + install links, Options + Advanced + summary, Install with live log + gold progress + cancel, Success (Open/Copy) and a Failure path (Retry). Confirm the frameless drag/min/close, the starfield, and (if your OS has it set) reduced-motion. This is the visual sign-off and cannot be done headlessly.

---

## Exit criteria

- The wizard renders Direction D: frameless window with custom chrome + drag/min/close, deep-space starfield, gold-forward calm surfaces, bundled Aldo/DIN fonts, a 3-step rail, and a fade between screens.
- Prerequisites shows a scanning state, status rows, and install-link buttons for known failing prereqs; Continue gates on `doctorOk`.
- Options has workspace cards, a segmented config, an Advanced disclosure (collapsed), and a live summary; emits the same camelCase args.
- Install shows the full expected-step checklist (pending/running/done), a gold progress bar, and the colorized live log with Cancel.
- A real Result screen: Success (Open Aphelyon.slnx via `open_solution`, Copy log, Close) and Failure (error + Retry + Copy log + Close).
- `npm run build` 0, `npm run check` 0 errors, `npm run test` all pass (6 reducer + 4 steps + 2 links), `cargo test` 5 pass.
- `@@WIZ` contract, `setup.ps1`, `orchestrator.rs`, and `reduce()/WizState` unchanged; the only Rust addition is `open_solution`.
- `Setup.exe` rebuilt and committed at the repo root.

## Out of scope

setup.ps1 / doctor.bat / `@@WIZ` grammar / parser / reducer changes; a Welcome screen; sound; light theme; i18n; a Svelte component unit-test harness.
