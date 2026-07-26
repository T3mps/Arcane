# Arcane Hub Visual Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restyle the Arcane Hub into the approved "Forge" shell — sidebar + cover-art grid, gold accent, dimmed treatment for projects the selected engine cannot open — and split its single 246-line `+page.svelte` into focused components.

**Architecture:** Frontend-only. `src/routes/+page.svelte` stays the only stateful file (it keeps every `invoke` call, all `$state`, and the `guard()` wrapper) and switches between two presentational views. Everything with real logic is extracted to `src/lib/format.ts` and unit-tested; everything else is a props-in/callbacks-out `.svelte` component verified by typecheck, build, and desk observation.

**Tech Stack:** Tauri 2, SvelteKit 2 (SPA via adapter-static), Svelte 5 runes (`$state`/`$props`), TypeScript, vitest, svelte-check.

**Spec:** `docs/superpowers/specs/2026-07-26-arcane-hub-visual-polish-design.md`. Approved visual reference: `.superpowers/brainstorm/54-1785063500/content/locked.html`.

## Global Constraints

- **No Rust changes.** No command added, renamed, or altered. `src/lib/api.ts` signatures and `HubState`/`RecentProject`/`EngineEntry` shapes are frozen. The only non-frontend edit in the entire plan is adding the string `"core:window:allow-toggle-maximize"` to `src-tauri/capabilities/default.json` (Task 4).
- **All comments ASCII, no BOM** (repo-wide rule, `CLAUDE.md`).
- **Token names copied verbatim** from `Tools/setup-wizard/src/lib/theme.css` so the two files stay diffable.
- **Colour logic is a rule, not a preference:** `--gold #f0c869` means *act* (primary button, active nav rail, cover monogram). `--fail #f1949f` means *won't open* (the abi badge). Dimmed surfaces mean *inert* (the whole incompatible card). `--ok #74dca2` means engine resolved. Gold never means trouble; coral never means action.
- **Invalid state carries two independent signals** — loss of warmth *and* a bordered coral badge. Never hue alone.
- **Fonts:** display `AldotheApache.ttf` only; body `Inter_18pt-Regular.ttf` (400) and `Inter_18pt-SemiBold.ttf` (600) only. Do **not** add `PF-Din-Text-Universal-Medium.ttf` — commercial licence, undocumented in this repo.
- **Sidebar ships exactly two nav items:** Projects, Engines. No Templates, no Learn.
- **Incompatible projects stay clickable** — parity with today. Do not disable them.
- **Contrast is already verified**, do not re-derive: coral `#f1949f` on `--surface-2 #10151f` = 8.2:1; `#120e04` on gold `#f0c869` = 12.1:1; `--text-muted #8593aa` on `#10151f` = 5.9:1. All pass AA. If you change a colour, re-measure.
- **Window minimum is 800x680**, so the project grid must reflow (`auto-fill`/`minmax`), never a hardcoded `repeat(3, 1fr)`.
- Run all `npm` commands from `Arcane/Hub/`. `node_modules` is already installed.
- **Never hardcode a colour that a token already defines.** To use a token at
  partial alpha, write `color-mix(in srgb, var(--token) N%, transparent)` --
  already the Hub's own idiom (`+page.svelte:196,237,238`) and safe on WebView2,
  which is Chromium. Writing `rgba(255, 92, 107, .12)` when `--fail-accent` is
  `#ff5c6b` desyncs silently the moment the token changes. The one intentional
  exception is the nebula tint `rgba(80, 150, 200, .06)` in Task 10's background,
  which corresponds to no token in this theme.
- **Never name a variable `state` in a `.svelte` file.** Diagnosed during
  execution: `let state = $state<HubState>(...)` makes svelte2tsx resolve
  `$state` as a legacy store-subscription of the variable `state`, so
  `npm run check` reports 6 bogus errors ("Block-scoped variable '$state' used
  before its declaration", "Untyped function calls may not accept type
  arguments"). The runtime compiler picks runes mode correctly, which is why the
  app runs fine and this is invisible until you typecheck. The Hub's current
  `+page.svelte` has exactly this bug; **Task 2 renames it to `hub`**, and
  Task 10 keeps that name. Verified: the rename alone takes svelte-check from
  6 errors to 0.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Hub/vitest.config.ts` | **Create.** Standalone vitest config (node env) so tests run without SvelteKit's plugin. |
| `Arcane/Hub/src/lib/format.ts` | **Create.** The only logic with branches: ABI compatibility, search filter, cover derivation. Unit-tested. |
| `Arcane/Hub/src/lib/format.test.ts` | **Create.** Tests for the above. |
| `Arcane/Hub/src/lib/theme.css` | **Create.** Tokens, `@font-face`, base styles, focus ring, scrollbar, reduced-motion. |
| `Arcane/Hub/static/fonts/*` | **Create.** Aldo + 2 Inter weights + Inter `OFL.txt`. |
| `Arcane/Hub/src/lib/components/WindowChrome.svelte` | **Create.** Drag region, minimize / toggle-maximize / close. |
| `Arcane/Hub/src/lib/components/Button.svelte` | **Create.** `gold` \| `ghost` \| `danger`. |
| `Arcane/Hub/src/lib/components/EmptyState.svelte` | **Create.** Centred icon + message + optional action slot. |
| `Arcane/Hub/src/lib/components/Sidebar.svelte` | **Create.** Nav + active-engine footer. |
| `Arcane/Hub/src/lib/components/ProjectCard.svelte` | **Create.** Cover, name, abi/when, dimmed variant. |
| `Arcane/Hub/src/lib/components/EngineRow.svelte` | **Create.** Engine row: select / remove. |
| `Arcane/Hub/src/lib/views/ProjectsView.svelte` | **Create.** Search, grid, inline new-project flow. |
| `Arcane/Hub/src/lib/views/EnginesView.svelte` | **Create.** Register / select / forget / nearby suggestion. |
| `Arcane/Hub/src/routes/+page.svelte` | **Rewrite.** State, `guard()`, error banner, shell layout, view switch. |
| `Arcane/Hub/src/app.html` | **Unchanged.** No edit needed — `theme.css` is imported from `+page.svelte`, which Vite hoists into the bundle as global CSS. |
| `Arcane/Hub/src-tauri/tauri.conf.json` | **Modify in Task 10** (not Task 4). `"decorations": false`, landed together with mounting WindowChrome. |
| `Arcane/Hub/src-tauri/capabilities/default.json` | **Modify.** Add `core:window:allow-toggle-maximize`. |
| `Arcane/Hub/static/{svelte,tauri,vite}.svg` | **Delete.** Scaffolding leftovers. |

---

### Task 1: Pure logic module + test harness

**Files:**
- Create: `Arcane/Hub/vitest.config.ts`
- Create: `Arcane/Hub/src/lib/format.ts`
- Test: `Arcane/Hub/src/lib/format.test.ts`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `isCompatible(projectAbi: number, engineAbi: number | null): boolean`
  - `filterProjects<T extends { name: string; path: string }>(items: T[], query: string): T[]`
  - `type Cover = { monogram: string; angle: number }`
  - `coverFor(name: string, path: string): Cover`

`npm test` currently exits 1 with "No test files found" and there is no vitest config. This task fixes both.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Hub/src/lib/format.test.ts`:

```ts
import { describe, it, expect } from "vitest";
import { isCompatible, filterProjects, coverFor } from "./format";

describe("isCompatible", () => {
  // Exact parity with the inline rule this replaces (+page.svelte:139):
  //   !selectedEngine || p.engineAbi === 0 || p.engineAbi === selectedEngine.engineAbi
  it("matching abi is compatible", () => {
    expect(isCompatible(7, 7)).toBe(true);
  });
  it("differing abi is not compatible", () => {
    expect(isCompatible(5, 7)).toBe(false);
  });
  it("unknown project abi (0) is treated as compatible", () => {
    // 0 means the manifest did not state one -- we cannot prove a conflict,
    // so we must not brand it broken.
    expect(isCompatible(0, 7)).toBe(true);
  });
  it("no engine selected is compatible", () => {
    // Nothing to conflict with; the UI disables launching separately.
    expect(isCompatible(5, null)).toBe(true);
  });
  it("unknown abi with no engine is compatible", () => {
    expect(isCompatible(0, null)).toBe(true);
  });
});

describe("filterProjects", () => {
  const items = [
    { name: "Aphelyon", path: "D:/dev/starworks/Gacha/Aphelyon.arcproj" },
    { name: "SampleProject", path: "D:/dev/starworks/Arcane/Samples/SampleProject" },
    { name: "OldPrototype", path: "D:/dev/archive/OldPrototype.arcproj" },
  ];
  it("empty query returns everything", () => {
    expect(filterProjects(items, "")).toHaveLength(3);
  });
  it("whitespace-only query returns everything", () => {
    expect(filterProjects(items, "   ")).toHaveLength(3);
  });
  it("matches name case-insensitively", () => {
    expect(filterProjects(items, "aphel").map((i) => i.name)).toEqual(["Aphelyon"]);
  });
  it("matches path as well as name", () => {
    // Typing a folder you remember must find the project.
    expect(filterProjects(items, "archive").map((i) => i.name)).toEqual(["OldPrototype"]);
  });
  it("no match returns empty", () => {
    expect(filterProjects(items, "zzz")).toEqual([]);
  });
  it("does not mutate the input array", () => {
    const copy = [...items];
    filterProjects(items, "aphel");
    expect(items).toEqual(copy);
  });
});

describe("coverFor", () => {
  it("monogram is the first alphanumeric character, uppercased", () => {
    expect(coverFor("aphelyon", "x").monogram).toBe("A");
    expect(coverFor("  sample", "x").monogram).toBe("S");
    expect(coverFor("3d-demo", "x").monogram).toBe("3");
  });
  it("falls back to ? when there is no alphanumeric character", () => {
    expect(coverFor("---", "x").monogram).toBe("?");
    expect(coverFor("", "x").monogram).toBe("?");
  });
  it("angle is deterministic for the same path", () => {
    // THE requirement: a card must not change appearance between launches.
    expect(coverFor("A", "D:/one").angle).toBe(coverFor("A", "D:/one").angle);
  });
  it("angle differs for different paths", () => {
    expect(coverFor("A", "D:/one").angle).not.toBe(coverFor("A", "D:/two").angle);
  });
  it("angle stays inside the intended band", () => {
    for (const p of ["D:/a", "D:/b", "D:/c", "D:/dev/x/y", "", "//?/UNC"]) {
      const a = coverFor("N", p).angle;
      expect(a).toBeGreaterThanOrEqual(100);
      expect(a).toBeLessThanOrEqual(200);
    }
  });
  it("angle does not depend on the name", () => {
    expect(coverFor("Zebra", "D:/one").angle).toBe(coverFor("Apple", "D:/one").angle);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd Arcane/Hub && npx vitest run src/lib/format.test.ts`
Expected: FAIL — `Failed to resolve import "./format"`.

- [ ] **Step 3: Add the vitest config**

Create `Arcane/Hub/vitest.config.ts`:

```ts
import { defineConfig } from "vitest/config";

// Standalone Vitest config, deliberately NOT the SvelteKit vite.config: these
// are pure-function unit tests and do not need (or want) the SvelteKit plugin
// in the pipeline. Same shape as Tools/setup-wizard/vitest.config.ts.
export default defineConfig({
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
});
```

- [ ] **Step 4: Write the implementation**

Create `Arcane/Hub/src/lib/format.ts`:

```ts
// The Hub's only branching logic, kept out of markup so it can be unit-tested.
// Everything else in src/lib is presentational.

/**
 * Whether `projectAbi` will open under `engineAbi`.
 *
 * Exact parity with the rule this replaces. Two permissive cases are
 * deliberate: abi 0 means the manifest did not state one (we cannot prove a
 * conflict, so we must not brand it broken), and a null engine means nothing
 * is selected to conflict with -- the UI gates launching separately.
 */
export function isCompatible(projectAbi: number, engineAbi: number | null): boolean {
  if (engineAbi === null) return true;
  if (projectAbi === 0) return true;
  return projectAbi === engineAbi;
}

/** Case-insensitive substring over BOTH name and path; blank query passes all. */
export function filterProjects<T extends { name: string; path: string }>(
  items: T[],
  query: string,
): T[] {
  const q = query.trim().toLowerCase();
  if (!q) return items.slice();
  return items.filter(
    (i) => i.name.toLowerCase().includes(q) || i.path.toLowerCase().includes(q),
  );
}

export type Cover = { monogram: string; angle: number };

/**
 * Cover art inputs for a project card: a monogram from the NAME and a gradient
 * angle from the PATH.
 *
 * Keyed on path, not name, because the path is the identity (two projects may
 * share a display name) and because it must be stable: a card that changed
 * appearance between launches would read as a bug. FNV-1a is used purely
 * because it is short, deterministic, and dependency-free -- no hash quality
 * is required here.
 */
export function coverFor(name: string, path: string): Cover {
  const ch = [...name].find((c) => /[a-z0-9]/i.test(c));
  const monogram = ch ? ch.toUpperCase() : "?";

  let h = 0x811c9dc5;
  for (let i = 0; i < path.length; i++) {
    h ^= path.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  // 100..200 degrees keeps every gradient diagonal and on-brand; a full 0..360
  // sweep would put some covers in flat vertical/horizontal bands.
  const angle = 100 + (h % 101);
  return { monogram, angle };
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd Arcane/Hub && npm test`
Expected: PASS — 17 tests across 3 suites (5 `isCompatible`, 6 `filterProjects`,
6 `coverFor`), exit code 0.

- [ ] **Step 6: Verify typecheck introduces nothing new**

Run: `cd Arcane/Hub && npm run check`
Expected: **6 errors, all in `src/routes/+page.svelte` lines 10-21**, all
pre-existing and none in the files this task created. They are the
`state`/`$state` naming bug described in Global Constraints and are fixed in
Task 2. Confirm no error names `format.ts`, `format.test.ts`, or
`vitest.config.ts`; if one does, that is yours to fix.

- [ ] **Step 7: Commit**

```bash
git add Arcane/Hub/vitest.config.ts Arcane/Hub/src/lib/format.ts Arcane/Hub/src/lib/format.test.ts
git commit -m "feat(hub): extract ABI/search/cover logic into a tested format module"
```

---

### Task 2: Theme tokens, fonts, and scaffolding cleanup

**Files:**
- Create: `Arcane/Hub/src/lib/theme.css`
- Create: `Arcane/Hub/static/fonts/AldotheApache.ttf` (copy)
- Create: `Arcane/Hub/static/fonts/Inter_18pt-Regular.ttf` (copy)
- Create: `Arcane/Hub/static/fonts/Inter_18pt-SemiBold.ttf` (copy)
- Create: `Arcane/Hub/static/fonts/OFL.txt` (copy, Inter's licence)
- Delete: `Arcane/Hub/static/svelte.svg`, `Arcane/Hub/static/tauri.svg`, `Arcane/Hub/static/vite.svg`

**Interfaces:**
- Consumes: nothing.
- Produces: CSS custom properties on `:root` consumed by every later task —
  `--bg-top --bg-bottom --surface --surface-2 --well --border --border-soft`,
  `--text --text-muted --text-dim`, `--gold --gold-bright`,
  `--ok --warn --fail --fail-accent`, `--r-win --r-panel --r-btn`,
  `--font-display --font-ui --font-mono`, `--dur --ease`.
  Also global classes `.display`, `.label`.

- [ ] **Step 1: Copy the font files**

```bash
cd D:/dev/starworks/Gacha
mkdir -p Arcane/Hub/static/fonts
cp Arcane/data/font/aldotheapache/AldotheApache.ttf      Arcane/Hub/static/fonts/
cp Arcane/data/font/inter/static/Inter_18pt-Regular.ttf  Arcane/Hub/static/fonts/
cp Arcane/data/font/inter/static/Inter_18pt-SemiBold.ttf Arcane/Hub/static/fonts/
cp Arcane/data/font/inter/OFL.txt                        Arcane/Hub/static/fonts/
```

Verify: `ls Arcane/Hub/static/fonts` lists exactly those four files.

- [ ] **Step 2: Delete the scaffolding SVGs**

```bash
cd D:/dev/starworks/Gacha
git rm Arcane/Hub/static/svelte.svg Arcane/Hub/static/tauri.svg Arcane/Hub/static/vite.svg
```

`git rm`, not plain `rm`: it removes the file AND stages the deletion in one
step. A plain `rm` followed by `git rm --cached` would untrack the path while
leaving the index confused about a file that no longer exists on disk.

Verify nothing referenced them: `grep -rn "svelte.svg\|tauri.svg\|vite.svg" Arcane/Hub/src` must print nothing.

- [ ] **Step 3: Write the theme**

Create `Arcane/Hub/src/lib/theme.css`:

```css
/* Arcane Hub design tokens. Token NAMES are copied verbatim from
   Tools/setup-wizard/src/lib/theme.css so the two files stay diffable; the Hub
   deliberately differs in body font (Inter, not PF DIN -- commercial licence,
   undocumented in this repo) and in owning its own font copies under static/
   rather than reaching into Tools/, which is retired prototype.

   Colour logic, enforced by convention everywhere below:
     --gold  = act        (primary button, active nav rail, cover monogram)
     --fail  = won't open (the abi badge on an incompatible card)
     dimmed  = inert      (the whole incompatible card)
     --ok    = resolved   (engine status dot)
   Gold never means trouble; coral never means action.
   ASCII only. */

@font-face {
  font-family: "Aldo";
  src: url("/fonts/AldotheApache.ttf") format("truetype");
  font-weight: 400 700; font-display: swap;
}
@font-face {
  font-family: "Inter";
  src: url("/fonts/Inter_18pt-Regular.ttf") format("truetype");
  font-weight: 400; font-display: swap;
}
@font-face {
  font-family: "Inter";
  src: url("/fonts/Inter_18pt-SemiBold.ttf") format("truetype");
  font-weight: 600; font-display: swap;
}

:root {
  --bg-top: #0a0e17; --bg-bottom: #080b12;
  --surface: #141927; --surface-2: #10151f; --well: #06090f;
  --border: #222a3b; --border-soft: #1c2230;
  --text: #eef2fa; --text-muted: #8593aa; --text-dim: #5d6a82;
  --gold: #f0c869; --gold-bright: #ffd161;
  --ok: #74dca2; --warn: #f0c869; --fail: #f1949f; --fail-accent: #ff5c6b;

  --font-display: "Aldo", "Bahnschrift", sans-serif;
  --font-ui: "Inter", ui-sans-serif, system-ui, sans-serif;
  --font-mono: "Cascadia Mono", Consolas, ui-monospace, monospace;

  --r-win: 12px; --r-panel: 8px; --r-btn: 7px;
  --dur: 200ms; --ease: cubic-bezier(.22, .61, .36, 1);
}

* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
body {
  font-family: var(--font-ui); color: var(--text); font-size: 13px; line-height: 1.5;
  background: var(--bg-bottom);
  -webkit-font-smoothing: antialiased;
  /* Desktop app, not a document: no text selection, no I-beam, no scroll bounce. */
  user-select: none; cursor: default; overflow: hidden;
}

.display { font-family: var(--font-display); text-transform: uppercase; letter-spacing: .07em; }
.label { font-size: 9.5px; letter-spacing: .15em; text-transform: uppercase; color: var(--text-dim); }

:focus-visible { outline: 2px solid var(--gold); outline-offset: 2px; border-radius: 4px; }

/* WebView2 is Chromium, so this applies. */
::-webkit-scrollbar { width: 9px; height: 9px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: #283146; border-radius: 5px;
                            border: 2px solid transparent; background-clip: padding-box; }
::-webkit-scrollbar-thumb:hover { background: #3a4860; background-clip: padding-box; }

@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after {
    animation-duration: .001ms !important; animation-iteration-count: 1 !important;
    transition-duration: .001ms !important;
  }
}
```

> **CORRECTED DURING EXECUTION — do not re-add a theme import here.** This step
> originally imported `theme.css` from `+page.svelte` "so the theme is live for
> Tasks 3-9". Task 2's reviewer caught that this collides with the legacy
> `<style>` block still in that file, which declares its own
> `:global(:root)` tokens including `--text`, plus `:global(body)` and
> `:global(*:focus-visible)`. Measured in the built bundle: legacy
> `--text: #c8cede` lands at byte offset 2002, AFTER `theme.css`'s
> `#eef2fa` at 528, so the legacy value wins and the focus ring renders violet
> instead of gold. Nothing in Tasks 3-9 renders or visually verifies anything,
> so the import bought nothing and cost a wrong-looking interim app.
> **`theme.css` is imported in Task 10**, in the same change that deletes the
> legacy block — the only point where both can be correct at once.

- [ ] **Step 4: Fix the `state`/`$state` typecheck bug**

Still in `Arcane/Hub/src/routes/+page.svelte`, rename the `state` variable to
`hub`. See Global Constraints for why: a variable named `state` makes
svelte2tsx read the `$state` rune as a store-subscription, producing 6 bogus
`npm run check` errors. There are five occurrences. Line 10 becomes:

```ts
  // NOT `state`: a variable of that name makes svelte2tsx parse the `$state`
  // rune as a legacy store-subscription and svelte-check reports 6 phantom
  // errors. The runtime compiler is unaffected, so this only shows up on
  // typecheck.
  let hub = $state<HubState>({ recents: [], engines: [] });
```

Then update its four readers:

```ts
    hub = await loadState();
```
```ts
    if (!selectedEngine || !hub.engines.some((e) => e.id === selectedEngine!.id)) {
      selectedEngine = hub.engines[0] ?? null;
    }
    suggestion = hub.engines.length === 0 ? await suggestEngine() : null;
```

and in the markup, every `state.engines` becomes `hub.engines` and every
`state.recents` becomes `hub.recents`.

Verify none remain: `grep -n '\bstate\.' src/routes/+page.svelte` must print
nothing, and `grep -c 'let state' src/routes/+page.svelte` must print `0`.

- [ ] **Step 5: Verify it builds, typechecks clean, and the fonts resolve**

Run: `cd Arcane/Hub && npm run check && npm run build`
Expected: **`svelte-check found 0 errors and 0 warnings`** — the 6 pre-existing
errors are gone as of Step 4. This is the first task whose gate is genuinely
clean, and every later task must keep it that way. `vite build` completes;
`build/_app/` exists.

Run: `ls Arcane/Hub/build/fonts`
Expected: the four font files were copied through by adapter-static.

- [ ] **Step 6: Commit**

```bash
# The svg deletions were already staged by `git rm` in Step 2.
git add Arcane/Hub/src/lib/theme.css Arcane/Hub/static/fonts Arcane/Hub/src/routes/+page.svelte
git commit -m "feat(hub): design tokens, bundled Aldo + Inter, drop scaffolding svgs"
```

---

### Task 3: Button and EmptyState

**Files:**
- Create: `Arcane/Hub/src/lib/components/Button.svelte`
- Create: `Arcane/Hub/src/lib/components/EmptyState.svelte`

**Interfaces:**
- Consumes: Task 2's tokens.
- Produces:
  - `Button` props: `{ variant?: "gold" | "ghost" | "danger", disabled?: boolean, title?: string, onclick?: () => void, children }`
  - `EmptyState` props: `{ title: string, body?: string, children? }` (`children` renders below the body as an action area)

These are leaves with no logic, so they have no unit tests; they are covered by typecheck plus the desk-verify list in Task 11.

- [ ] **Step 1: Write Button**

Create `Arcane/Hub/src/lib/components/Button.svelte`:

```svelte
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

<button class="btn {variant}" {disabled} {title} {onclick}>{@render children()}</button>

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
```

- [ ] **Step 2: Write EmptyState**

Create `Arcane/Hub/src/lib/components/EmptyState.svelte`:

```svelte
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
           padding: 34px 26px; text-align: center; background: rgba(255, 255, 255, .015); }
  .t { font-size: 14px; letter-spacing: .1em; color: var(--text-muted); margin: 0; }
  .b { font-size: 12.5px; color: var(--text-dim); line-height: 1.6;
       margin: 8px auto 0; max-width: 46ch; }
  .a { margin-top: 16px; display: flex; gap: 8px; justify-content: center; }
</style>
```

- [ ] **Step 3: Verify typecheck**

Run: `cd Arcane/Hub && npm run check`
Expected: 0 errors, 0 warnings.

- [ ] **Step 4: Commit**

```bash
git add Arcane/Hub/src/lib/components/Button.svelte Arcane/Hub/src/lib/components/EmptyState.svelte
git commit -m "feat(hub): Button and EmptyState primitives"
```

---

### Task 4: Custom window chrome

**Files:**
- Create: `Arcane/Hub/src/lib/components/WindowChrome.svelte`
- Modify: `Arcane/Hub/src-tauri/tauri.conf.json`
- Modify: `Arcane/Hub/src-tauri/capabilities/default.json`

**Interfaces:**
- Consumes: Task 2's tokens.
- Produces: `WindowChrome` — no props. Fixed 34px tall, `position: sticky; top: 0`.

Existing capabilities already grant `allow-minimize`, `allow-close`, and `allow-start-dragging`; only toggle-maximize is missing. Pattern mirrors `Tools/setup-wizard/src/lib/components/WindowChrome.svelte:11`.

- [ ] **Step 1: Add the capability**

In `Arcane/Hub/src-tauri/capabilities/default.json`, add one entry to `permissions` after `"core:window:allow-start-dragging"`:

```json
    "core:window:allow-toggle-maximize",
```

The full array becomes:

```json
  "permissions": [
    "core:default",
    "core:window:allow-minimize",
    "core:window:allow-close",
    "core:window:allow-start-dragging",
    "core:window:allow-toggle-maximize",
    "dialog:default",
    "dialog:allow-open"
  ]
```

- [ ] **Step 2: (moved) Do NOT turn off decorations in this task**

> **CORRECTED DURING EXECUTION.** This step originally set
> `"decorations": false` here. Task 4's reviewer raised it as a cross-task
> sequencing hazard and was right: `WindowChrome` is not mounted until Task 10,
> so between Task 4 and Task 10 the window would have **no OS titlebar and no
> replacement** — un-draggable, and unclosable except by Alt+F4 or Task Manager.
> `"decorations": false` now lands in **Task 10**, in the same change that mounts
> `WindowChrome`, which is the only point at which both are correct together.
> Same reasoning as deferring the `theme.css` import: never leave the interim app
> in a state a human would call broken.
>
> Nothing to do in this step. The capability added in Step 1 is additive and
> harmless before the chrome exists.

- [ ] **Step 3: Write WindowChrome**

Create `Arcane/Hub/src/lib/components/WindowChrome.svelte`:

```svelte
<script lang="ts">
  import { getCurrentWindow } from "@tauri-apps/api/window";
  // decorations:false means we own minimize/maximize/close and the drag region.
  // Buttons are children WITHOUT data-tauri-drag-region, which is how Tauri's
  // handler knows not to start a drag from them.
  const win = getCurrentWindow();
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- The drag region is chrome, not a control. Double-click-to-maximize is a
     redundant MOUSE convenience for an action already exposed on the focusable
     Maximize button below, so there is nothing a keyboard user loses and no
     sensible ARIA role for "titlebar". Svelte's rule cannot see that the
     behaviour is duplicated, hence the explicit suppression. -->
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
```

- [ ] **Step 4: Provide the logo asset**

```bash
cd D:/dev/starworks/Gacha
cp Arcane/data/images/arcane_logo.png Arcane/Hub/static/logo.png
```

- [ ] **Step 5: Verify typecheck and build**

Run: `cd Arcane/Hub && npm run check && npm run build`
Expected: **0 errors AND 0 warnings**; build completes. Warnings count: without
the `svelte-ignore` above, svelte-check emits
`a11y_no_static_element_interactions` for the drag region's `ondblclick`, which
would leave every later task's gate non-pristine.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Hub/src/lib/components/WindowChrome.svelte Arcane/Hub/static/logo.png \
        Arcane/Hub/src-tauri/tauri.conf.json Arcane/Hub/src-tauri/capabilities/default.json
git commit -m "feat(hub): custom window chrome, decorations off, toggle-maximize capability"
```

---

### Task 5: Sidebar

**Files:**
- Create: `Arcane/Hub/src/lib/components/Sidebar.svelte`

**Interfaces:**
- Consumes: Task 2's tokens. `EngineEntry` from `$lib/api`.
- Produces: `Sidebar` props:
  `{ view: "projects" | "engines", engine: EngineEntry | null, onNavigate: (v: "projects" | "engines") => void }`

- [ ] **Step 1: Write Sidebar**

Create `Arcane/Hub/src/lib/components/Sidebar.svelte`:

```svelte
<script module lang="ts">
  // MODULE script, not the instance script: a type has to be exported at module
  // scope to be importable as `import Sidebar, { type View } from "..."`.
  // Declaring it in the instance <script> below would not export it.
  export type View = "projects" | "engines";
</script>

<script lang="ts">
  import type { EngineEntry } from "$lib/api";
  // Exactly two items on purpose. Nav pointing at features that do not exist
  // makes a launcher feel like a mockup of itself; adding a third later is one
  // entry in this array, not a re-layout.
  let { view, engine, onNavigate }:
    { view: View; engine: EngineEntry | null; onNavigate: (v: View) => void } = $props();

  const items: { id: View; label: string; glyph: string }[] = [
    { id: "projects", label: "Projects", glyph: "\u25A3" },
    { id: "engines", label: "Engines", glyph: "\u2699" },
  ];
</script>

<aside class="side">
  <nav>
    {#each items as it (it.id)}
      <button class="nav" class:on={view === it.id} onclick={() => onNavigate(it.id)}
              aria-current={view === it.id ? "page" : undefined}>
        <span class="g" aria-hidden="true">{it.glyph}</span>{it.label}
      </button>
    {/each}
  </nav>

  <div class="eng">
    <div class="label">Active engine</div>
    {#if engine}
      <div class="v"><span class="dot" aria-hidden="true"></span>{engine.build}</div>
      <code>abi {engine.engineAbi}</code>
    {:else}
      <div class="v none">None registered</div>
    {/if}
  </div>
</aside>

<style>
  .side { width: 198px; flex: none; background: var(--surface-2);
          border-right: 1px solid var(--border-soft);
          padding: 10px 11px 14px; display: flex; flex-direction: column; }
  nav { display: flex; flex-direction: column; gap: 2px; }
  .nav { display: flex; align-items: center; gap: 10px; padding: 8px 10px;
         border-radius: 6px; font: inherit; font-size: 12.5px; text-align: left;
         background: none; border: 0; color: var(--text-muted); cursor: default;
         transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .nav:hover:not(.on) { background: rgba(255, 255, 255, .04); color: var(--text); }
  .nav.on { background: color-mix(in srgb, var(--gold) 12%, transparent); color: #f7dda0;
            box-shadow: inset 2px 0 0 var(--gold); }
  .g { width: 14px; text-align: center; opacity: .8; }

  .eng { margin-top: auto; background: var(--surface); border: 1px solid var(--border-soft);
         border-radius: var(--r-panel); padding: 10px 11px; }
  .v { font-size: 12px; margin-top: 5px; display: flex; align-items: center; gap: 6px; }
  .v.none { color: var(--warn); }
  .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--ok); flex: none; }
  code { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); }
</style>
```

- [ ] **Step 2: Verify typecheck**

Run: `cd Arcane/Hub && npm run check`
Expected: 0 errors, 0 warnings.

- [ ] **Step 3: Commit**

```bash
git add Arcane/Hub/src/lib/components/Sidebar.svelte
git commit -m "feat(hub): sidebar nav + active-engine footer"
```

---

### Task 6: ProjectCard

**Files:**
- Create: `Arcane/Hub/src/lib/components/ProjectCard.svelte`

**Interfaces:**
- Consumes: `coverFor` from Task 1; tokens from Task 2; `RecentProject` and `since` from `$lib/api`.
- Produces: `ProjectCard` props:
  `{ project: RecentProject, compatible: boolean, engineAbi: number | null, disabled?: boolean, onLaunch: () => void, onForget: () => void }`

The incompatible state must carry TWO signals: the card loses all warmth (surface, border, cover all neutral) AND shows a bordered coral badge. Never hue alone.

- [ ] **Step 1: Write ProjectCard**

Create `Arcane/Hub/src/lib/components/ProjectCard.svelte`:

```svelte
<script lang="ts">
  import { coverFor } from "$lib/format";
  import { since, type RecentProject } from "$lib/api";

  let { project, compatible, engineAbi, disabled = false, onLaunch, onForget }:
    {
      project: RecentProject; compatible: boolean; engineAbi: number | null;
      disabled?: boolean; onLaunch: () => void; onForget: () => void;
    } = $props();

  const cover = $derived(coverFor(project.name, project.path));
  // The full sentence the flat list used to print as body text. Kept as the
  // accessible description so the explanation survives the move to cards.
  const why = $derived(
    compatible
      ? project.path
      : `Built against abi ${project.engineAbi}; the selected engine is abi ${engineAbi}. It will refuse to open.`,
  );
</script>

<div class="card" class:bad={!compatible}>
  <button class="hit" {disabled} onclick={onLaunch} title={why} aria-label={project.name}>
    <span class="cover" style="--a: {cover.angle}deg" aria-hidden="true">{cover.monogram}</span>
    <span class="cb">
      <span class="nm">{project.name}</span>
      <span class="mt">
        {#if compatible}
          <span>abi {project.engineAbi ? project.engineAbi : "?"}</span>
        {:else}
          <em class="badge">abi {project.engineAbi}</em>
        {/if}
        <span>{since(project.lastOpenedUtc)}</span>
      </span>
    </span>
  </button>
  <button class="x" onclick={onForget} aria-label="Remove {project.name} from the list"
          title="Remove from list">&#10005;</button>
</div>

<style>
  .card { position: relative; border: 1px solid var(--border-soft);
          border-radius: var(--r-panel); overflow: hidden; background: var(--surface);
          transition: border-color var(--dur) var(--ease); }
  .card:hover { border-color: #2d3750; }
  .hit { display: block; width: 100%; text-align: left; background: none;
         border: 0; padding: 0; font: inherit; color: inherit; cursor: default; }
  .hit:disabled { opacity: .5; }

  .cover { display: grid; place-items: center; height: 64px;
           font-family: var(--font-display); font-size: 21px; color: var(--gold);
           background: linear-gradient(var(--a), #463714, #191b26); }
  .cb { display: block; padding: 9px 11px; }
  .nm { display: block; font-size: 12.5px; font-weight: 600; color: var(--text);
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .mt { display: flex; justify-content: space-between; align-items: center;
        margin-top: 5px; font-family: var(--font-mono); font-size: 9.5px;
        color: var(--text-dim); }

  /* INERT: no gold anywhere, surfaces drop back, name recedes to muted (5.9:1). */
  .bad { background: var(--surface-2); border-color: var(--border); }
  .bad .cover { background: linear-gradient(var(--a), #20242e, #14171e); color: #4d566a; }
  .bad .nm { color: var(--text-muted); }
  /* Second, non-chromatic signal: a bordered badge (coral is 8.2:1 here). */
  .badge { font-style: normal; font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent); border-radius: 3px; padding: 0 4px; }

  .x { position: absolute; top: 6px; right: 6px; width: 20px; height: 20px;
       display: grid; place-items: center; border: 0; border-radius: 4px;
       background: rgba(8, 11, 18, .6); color: var(--text-dim); font-size: 10px;
       cursor: default; opacity: 0;
       transition: opacity var(--dur) var(--ease), color var(--dur) var(--ease); }
  .card:hover .x, .x:focus-visible { opacity: 1; }
  .x:hover { color: var(--fail); }
</style>
```

- [ ] **Step 2: Verify typecheck**

Run: `cd Arcane/Hub && npm run check`
Expected: 0 errors, 0 warnings.

- [ ] **Step 3: Commit**

```bash
git add Arcane/Hub/src/lib/components/ProjectCard.svelte
git commit -m "feat(hub): project card with dimmed incompatible state"
```

---

### Task 7: EngineRow

**Files:**
- Create: `Arcane/Hub/src/lib/components/EngineRow.svelte`

**Interfaces:**
- Consumes: tokens from Task 2; `EngineEntry` from `$lib/api`; `Button` from Task 3.
- Produces: `EngineRow` props:
  `{ engine: EngineEntry, selected: boolean, busy?: boolean, onSelect: () => void, onForget: () => void }`

- [ ] **Step 1: Write EngineRow**

Create `Arcane/Hub/src/lib/components/EngineRow.svelte`:

```svelte
<script lang="ts">
  import Button from "./Button.svelte";
  import type { EngineEntry } from "$lib/api";

  let { engine, selected, busy = false, onSelect, onForget }:
    {
      engine: EngineEntry; selected: boolean; busy?: boolean;
      onSelect: () => void; onForget: () => void;
    } = $props();
</script>

<div class="row" class:sel={selected}>
  <button class="pick" onclick={onSelect} aria-pressed={selected}>
    <span class="nm">{engine.build}</span>
    <code class="path">{engine.path}</code>
  </button>
  <code class="abi">abi {engine.engineAbi}</code>
  <Button variant="danger" disabled={busy} onclick={onForget}>Remove</Button>
</div>

<style>
  .row { display: flex; align-items: center; gap: 12px; padding: 9px 11px;
         border: 1px solid var(--border-soft); border-radius: var(--r-panel);
         background: var(--surface); margin-bottom: 8px;
         transition: border-color var(--dur) var(--ease); }
  .row:hover { border-color: #2d3750; }
  /* Selection is the gold rail, same vocabulary as the sidebar's active item. */
  .sel { box-shadow: inset 2px 0 0 var(--gold); }
  .pick { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px;
          background: none; border: 0; padding: 0; text-align: left;
          font: inherit; color: inherit; cursor: default; }
  .nm { font-size: 13px; font-weight: 600; }
  .path { font-family: var(--font-mono); font-size: 10.5px; color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .abi { font-family: var(--font-mono); font-size: 10px; color: var(--text-dim); flex: none; }
</style>
```

- [ ] **Step 2: Verify typecheck**

Run: `cd Arcane/Hub && npm run check`
Expected: 0 errors, 0 warnings.

- [ ] **Step 3: Commit**

```bash
git add Arcane/Hub/src/lib/components/EngineRow.svelte
git commit -m "feat(hub): engine row"
```

---

### Task 8: ProjectsView

**Files:**
- Create: `Arcane/Hub/src/lib/views/ProjectsView.svelte`

**Interfaces:**
- Consumes: `filterProjects`/`isCompatible` (Task 1), `Button` + `EmptyState` (Task 3), `ProjectCard` (Task 6), `HubState`/`EngineEntry`/`RecentProject` from `$lib/api`.
- Produces: `ProjectsView` props:
  ```ts
  {
    recents: RecentProject[];
    engine: EngineEntry | null;
    busy: boolean;
    onLaunch: (p: RecentProject) => void;
    onForget: (p: RecentProject) => void;
    onOpen: () => void;
    onCreate: (name: string) => void;
  }
  ```

The grid must reflow: `repeat(auto-fill, minmax(190px, 1fr))`. At the 800px window minimum the main area is ~600px, where a hardcoded three-up would crush the cards.

- [ ] **Step 1: Write ProjectsView**

Create `Arcane/Hub/src/lib/views/ProjectsView.svelte`:

```svelte
<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import EmptyState from "$lib/components/EmptyState.svelte";
  import ProjectCard from "$lib/components/ProjectCard.svelte";
  import { filterProjects, isCompatible } from "$lib/format";
  import type { EngineEntry, RecentProject } from "$lib/api";

  let { recents, engine, busy, onLaunch, onForget, onOpen, onCreate }:
    {
      recents: RecentProject[]; engine: EngineEntry | null; busy: boolean;
      onLaunch: (p: RecentProject) => void; onForget: (p: RecentProject) => void;
      onOpen: () => void; onCreate: (name: string) => void;
    } = $props();

  let query = $state("");
  let creating = $state(false);
  let newName = $state("");

  const engineAbi = $derived(engine ? engine.engineAbi : null);
  const shown = $derived(filterProjects(recents, query));
  const incompatible = $derived(
    recents.filter((p) => !isCompatible(p.engineAbi, engineAbi)).length,
  );

  function submit() {
    if (!newName.trim()) return;
    onCreate(newName.trim());
    creating = false;
    newName = "";
  }
</script>

<header class="top">
  <div>
    <h2 class="display">Projects</h2>
    <p class="sub">
      {recents.length} {recents.length === 1 ? "project" : "projects"}
      {#if incompatible > 0}&middot; {incompatible} need a different engine{/if}
    </p>
  </div>
  <div class="acts">
    <Button disabled={busy || !engine} onclick={onOpen}>Open&hellip;</Button>
    <Button variant="gold" disabled={busy || !engine}
            onclick={() => (creating = !creating)}>New project</Button>
  </div>
</header>

{#if creating}
  <div class="new">
    <input bind:value={newName} placeholder="Project name" spellcheck="false"
           onkeydown={(e) => e.key === "Enter" && submit()} />
    <Button variant="gold" disabled={busy || !newName.trim()} onclick={submit}>
      Choose folder and create
    </Button>
  </div>
{/if}

{#if recents.length === 0}
  <EmptyState title="No projects yet"
              body="Open a folder containing a .arcproj, or create one. The Hub launches it with the engine selected in the sidebar." />
{:else}
  <input class="search" bind:value={query} placeholder="Search projects" spellcheck="false" />
  {#if shown.length === 0}
    <EmptyState title="No matches" body={`Nothing matches "${query}".`} />
  {:else}
    <div class="grid">
      {#each shown as p (p.path)}
        <ProjectCard project={p} engineAbi={engineAbi}
                     compatible={isCompatible(p.engineAbi, engineAbi)}
                     disabled={busy || !engine}
                     onLaunch={() => onLaunch(p)} onForget={() => onForget(p)} />
      {/each}
    </div>
  {/if}
{/if}

<style>
  .top { display: flex; align-items: flex-end; justify-content: space-between;
         gap: 16px; margin-bottom: 15px; }
  h2 { font-size: 22px; margin: 0; color: #f3f6fc;
       text-shadow: 0 0 20px color-mix(in srgb, var(--gold) 20%, transparent); }
  .sub { font-size: 11.5px; color: var(--text-muted); margin: 2px 0 0; }
  .acts { display: flex; gap: 8px; flex: none; }

  .new { display: flex; gap: 8px; margin-bottom: 13px; }
  .new input { flex: 1; }
  input { background: var(--well); border: 1px solid var(--border);
          border-radius: var(--r-btn); color: var(--text); font: inherit;
          font-size: 12px; padding: 7px 11px; user-select: text; cursor: text; }
  input::placeholder { color: var(--text-dim); }
  input:focus { border-color: var(--gold); outline: none; }
  .search { width: 100%; margin-bottom: 13px; }

  /* auto-fill, NOT repeat(3, 1fr): the window minimum is 800px wide. */
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr));
          gap: 11px; }
</style>
```

- [ ] **Step 2: Verify typecheck**

Run: `cd Arcane/Hub && npm run check`
Expected: 0 errors, 0 warnings.

- [ ] **Step 3: Commit**

```bash
git add Arcane/Hub/src/lib/views/ProjectsView.svelte
git commit -m "feat(hub): projects view with search and reflowing card grid"
```

---

### Task 9: EnginesView

**Files:**
- Create: `Arcane/Hub/src/lib/views/EnginesView.svelte`

**Interfaces:**
- Consumes: `Button` + `EmptyState` (Task 3), `EngineRow` (Task 7), `EngineEntry` from `$lib/api`.
- Produces: `EnginesView` props:
  ```ts
  {
    engines: EngineEntry[];
    selected: EngineEntry | null;
    suggestion: EngineEntry | null;
    busy: boolean;
    onRegister: () => void;
    onRegisterPath: (path: string) => void;
    onSelect: (e: EngineEntry) => void;
    onForget: (e: EngineEntry) => void;
  }
  ```

- [ ] **Step 1: Write EnginesView**

Create `Arcane/Hub/src/lib/views/EnginesView.svelte`:

```svelte
<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import EmptyState from "$lib/components/EmptyState.svelte";
  import EngineRow from "$lib/components/EngineRow.svelte";
  import type { EngineEntry } from "$lib/api";

  let { engines, selected, suggestion, busy, onRegister, onRegisterPath, onSelect, onForget }:
    {
      engines: EngineEntry[]; selected: EngineEntry | null; suggestion: EngineEntry | null;
      busy: boolean; onRegister: () => void; onRegisterPath: (path: string) => void;
      onSelect: (e: EngineEntry) => void; onForget: (e: EngineEntry) => void;
    } = $props();
</script>

<header class="top">
  <div>
    <h2 class="display">Engines</h2>
    <p class="sub">
      {engines.length} registered{#if selected} &middot; using {selected.build}{/if}
    </p>
  </div>
  <Button variant="gold" disabled={busy} onclick={onRegister}>Register engine</Button>
</header>

{#if engines.length === 0}
  <EmptyState title="No engine registered"
              body="The Hub launches projects with an engine you register. Point it at a folder containing ArcaneEditor.exe.">
    {#if suggestion}
      <!-- {@const} binds the narrowed value so TypeScript does not have to
           re-narrow a reactive prop inside the callback closure below. -->
      {@const s = suggestion}
      <Button variant="gold" disabled={busy} onclick={() => onRegisterPath(s.path)}>
        Found one nearby &mdash; register {s.build}
      </Button>
    {/if}
  </EmptyState>
{:else}
  {#each engines as e (e.id)}
    <EngineRow engine={e} selected={selected?.id === e.id} {busy}
               onSelect={() => onSelect(e)} onForget={() => onForget(e)} />
  {/each}
  <p class="hint">The selected engine launches every project and decides which are compatible.</p>
{/if}

<style>
  .top { display: flex; align-items: flex-end; justify-content: space-between;
         gap: 16px; margin-bottom: 15px; }
  h2 { font-size: 22px; margin: 0; color: #f3f6fc;
       text-shadow: 0 0 20px color-mix(in srgb, var(--gold) 20%, transparent); }
  .sub { font-size: 11.5px; color: var(--text-muted); margin: 2px 0 0; }
  .hint { font-size: 11.5px; color: var(--text-dim); margin: 12px 0 0; }
</style>
```

- [ ] **Step 2: Verify typecheck**

Run: `cd Arcane/Hub && npm run check`
Expected: 0 errors, 0 warnings.

- [ ] **Step 3: Commit**

```bash
git add Arcane/Hub/src/lib/views/EnginesView.svelte
git commit -m "feat(hub): engines view"
```

---

### Task 10: Rewire the page shell

**Files:**
- Modify (full rewrite): `Arcane/Hub/src/routes/+page.svelte`
- Modify: `Arcane/Hub/src-tauri/tauri.conf.json` (add `"decorations": false`)

**Interfaces:**
- Consumes: everything from Tasks 1-9.
- Produces: the running app. No exports.

This is the integration task. `+page.svelte` keeps every `invoke` call and all `$state`; nothing below it touches the API. Behaviour to preserve exactly: `refresh()` re-selects the first engine when the current one vanishes; `suggestEngine()` is only asked when there are zero engines; `guard()` clears the error, sets busy, refreshes on success, and always clears busy.

- [ ] **Step 1: Turn off OS decorations**

Deferred here from Task 4 on purpose: this is the same change that mounts
`WindowChrome`, so it is the first moment the window can lose its OS titlebar
without losing every way to move or close it.

In `Arcane/Hub/src-tauri/tauri.conf.json`, inside the single entry of
`app.windows`, add `"decorations": false` after `"resizable": true`:

```json
      {
        "title": "Arcane Hub",
        "width": 1000,
        "height": 680,
        "minWidth": 800,
        "minHeight": 520,
        "resizable": true,
        "decorations": false,
        "center": true
      }
```

- [ ] **Step 2: Rewrite the page**

Replace the entire contents of `Arcane/Hub/src/routes/+page.svelte`:

```svelte
<script lang="ts">
  import "$lib/theme.css";
  import { onMount } from "svelte";
  import { open } from "@tauri-apps/plugin-dialog";
  import WindowChrome from "$lib/components/WindowChrome.svelte";
  import Sidebar, { type View } from "$lib/components/Sidebar.svelte";
  import ProjectsView from "$lib/views/ProjectsView.svelte";
  import EnginesView from "$lib/views/EnginesView.svelte";
  import {
    loadState, registerEngine, forgetEngine, forgetProject,
    openProject, createProject, suggestEngine,
    type HubState, type EngineEntry, type RecentProject,
  } from "$lib/api";

  // NOT `state` -- see Global Constraints. A variable of that name makes
  // svelte2tsx read the `$state` rune as a legacy store-subscription, and
  // svelte-check reports 6 phantom errors while the app still runs fine.
  let hub = $state<HubState>({ recents: [], engines: [] });
  let selectedEngine = $state<EngineEntry | null>(null);
  let suggestion = $state<EngineEntry | null>(null);
  let error = $state("");
  let busy = $state(false);
  let view = $state<View>("projects");

  async function refresh() {
    hub = await loadState();
    if (!selectedEngine || !hub.engines.some((e) => e.id === selectedEngine!.id)) {
      selectedEngine = hub.engines[0] ?? null;
    }
    // Adjacency is a suggestion for the dev loop, never an assumption.
    suggestion = hub.engines.length === 0 ? await suggestEngine() : null;
  }

  onMount(async () => {
    await refresh();
    // Land on Engines when there is nothing to launch with -- the one thing the
    // user must do first. Replaces the old force-showing engines section.
    if (hub.engines.length === 0) view = "engines";
  });

  async function guard(fn: () => Promise<unknown>) {
    error = "";
    busy = true;
    try { await fn(); await refresh(); }
    catch (e) { error = String(e); }
    finally { busy = false; }
  }

  const addEngine = () => guard(async () => {
    const dir = await open({ directory: true, title: "Select an Arcane engine folder" });
    if (typeof dir === "string") await registerEngine(dir);
  });

  const addProject = () => guard(async () => {
    const dir = await open({ directory: true, title: "Select a project folder" });
    if (typeof dir === "string" && selectedEngine) {
      await openProject(dir, selectedEngine.path);
    }
  });

  const launch = (p: RecentProject) => guard(async () => {
    if (selectedEngine) await openProject(p.path, selectedEngine.path);
  });

  const makeProject = (name: string) => guard(async () => {
    if (!selectedEngine) return;
    const dir = await open({ directory: true, title: "Where should the project live?" });
    if (typeof dir !== "string") return;
    const root = await createProject(dir, name, selectedEngine.path);
    await openProject(root, selectedEngine.path);
  });
</script>

<div class="app">
  <WindowChrome />
  <div class="body">
    <Sidebar {view} engine={selectedEngine} onNavigate={(v) => (view = v)} />
    <main>
      {#if error}
        <p class="error" role="alert">{error}</p>
      {/if}
      {#if view === "projects"}
        <ProjectsView recents={hub.recents} engine={selectedEngine} {busy}
                      onLaunch={launch} onForget={(p) => guard(() => forgetProject(p.path))}
                      onOpen={addProject} onCreate={makeProject} />
      {:else}
        <EnginesView engines={hub.engines} selected={selectedEngine} {suggestion} {busy}
                     onRegister={addEngine}
                     onRegisterPath={(path) => guard(() => registerEngine(path))}
                     onSelect={(e) => (selectedEngine = e)}
                     onForget={(e) => guard(() => forgetEngine(e.path))} />
      {/if}
    </main>
  </div>
</div>

<style>
  /* The window's own gradient lives here rather than on body so the rounded
     corners from --r-win clip it. */
  .app { display: flex; flex-direction: column; height: 100vh; overflow: hidden;
         border-radius: var(--r-win);
         background:
           radial-gradient(760px 320px at 82% -14%, color-mix(in srgb, var(--gold) 7.5%, transparent), transparent 62%),
           radial-gradient(560px 260px at 4% 106%, rgba(80, 150, 200, .06), transparent 60%),
           linear-gradient(var(--bg-top), var(--bg-bottom)); }
  .body { flex: 1; display: flex; min-height: 0; }
  main { flex: 1; min-width: 0; overflow-y: auto; padding: 18px 24px 22px; }

  .error { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
           border: 1px solid color-mix(in srgb, var(--fail-accent) 40%, transparent);
           border-radius: var(--r-btn); color: var(--text); font-size: 12.5px;
           padding: 9px 12px; margin: 0 0 14px; user-select: text; }
</style>
```

- [ ] **Step 3: Verify the legacy global styles are gone**

The replacement above is a FULL-FILE rewrite, which is what finally removes the
old `<style>` block's `:global(:root)` token declarations, `:global(body)`, and
`:global(*:focus-visible)`. Those collide with `theme.css` by name (measured:
legacy `--text: #c8cede` landed after `theme.css`'s `#eef2fa` in the bundle and
won), which is why `theme.css` is imported here and not earlier.

Run these and expect no output from either:

```bash
cd Arcane/Hub
grep -n ':global(' src/routes/+page.svelte
grep -nE '\-\-(void|panel|line|arc|ember|bad|display|body|mono)\s*:' src/routes/+page.svelte
```

Then confirm exactly one `--text` declaration survives in the built CSS:

```bash
npm run build
grep -o '\-\-text: #[0-9a-f]\{6\}' build/_app/immutable/assets/*.css
```
Expected: a single line, `--text: #eef2fa`.

- [ ] **Step 4: Verify typecheck and build**

Run: `cd Arcane/Hub && npm run check && npm test && npm run build`
Expected: svelte-check 0 errors; 17 tests pass; build completes.

- [ ] **Step 5: Confirm the API surface really is untouched**

Run: `cd D:/dev/starworks/Gacha && git diff --stat Arcane/Hub/src/lib/api.ts Arcane/Hub/src-tauri/src/`
Expected: **no output.** Any output here means the plan's core constraint was violated.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Hub/src/routes/+page.svelte Arcane/Hub/src-tauri/tauri.conf.json
git commit -m "feat(hub): sidebar shell, view switch, and error banner placement"
```

---

### Task 11: Verification pass

**Files:**
- Modify: `docs/superpowers/plans/2026-07-26-arcane-hub-visual-polish.md` (tick the desk-verify list)

**Interfaces:**
- Consumes: the finished app.
- Produces: a recorded verification result.

- [ ] **Step 1: Full automated gate**

Run from `Arcane/Hub/`:

```bash
npm run check
npm test
npm run build
```

Expected: 0 svelte-check errors; 17 tests pass; build succeeds.

- [ ] **Step 2: Prove the Rust side is genuinely untouched**

```bash
cd D:/dev/starworks/Gacha/Arcane/Hub/src-tauri
cargo test
cargo clippy --all-targets -- -D warnings
```

Expected: 26 tests pass; clippy clean. (Nothing in this plan edits Rust, so a failure here means something unrelated broke.)

- [ ] **Step 3: Build and stage the real binary**

Run: `cd Arcane/Hub && npm run build:hub`
Expected: `stage: .../Arcane/bin/Release-windows-x86_64-md/Hub/arcane-hub.exe`.

- [ ] **Step 4: Desk-verify — nothing below has automated coverage**

Launch the staged exe and confirm each:

- [ ] Custom titlebar: drag anywhere on the bar moves the window; the three controls do not drag.
- [ ] Minimize, maximize (and restore), close all work; double-clicking the bar toggles maximize.
- [ ] Window corners are rounded, with no square black corner behind them.
- [ ] Resize to the 800px minimum: the card grid reflows to fewer columns and no card is crushed.
- [ ] A compatible project card shows a gold monogram; the incompatible one is grey with a coral bordered `abi N` badge.
- [ ] Hovering the incompatible card shows the full "Built against abi N..." sentence as a tooltip.
- [ ] Two projects with different paths have visibly different cover gradient angles; relaunching gives each the same angle as before.
- [ ] Search filters by name and by a path fragment; clearing it restores every card.
- [ ] With zero engines the app lands on the Engines view and offers the nearby suggestion if one exists.
- [ ] Removing the selected engine falls back to the first remaining one, or to "None registered".
- [ ] Tab moves through cards in visual order with a visible gold focus ring.
- [ ] Trigger an error (register a folder with no `ArcaneEditor.exe`): the banner appears above the view content, full width.

- [ ] **Step 5: Commit the verification record**

```bash
git add docs/superpowers/plans/2026-07-26-arcane-hub-visual-polish.md
git commit -m "docs(hub): record visual-polish verification results"
```

---

## Notes for the implementer

- **Svelte 5 runes, not Svelte 4.** `$state` / `$props` / `$derived`, and `onclick` rather than `on:click`. The existing `+page.svelte` is already runes-based; follow it.
- **`cursor: default` on buttons is deliberate**, inherited from `theme.css`'s desktop-app posture. This is a native window, not a web page — pointer cursors would be the tell. Text inputs re-enable `cursor: text` and `user-select: text` locally.
- **Do not add a component test harness.** The spec froze tooling at what already exists (vitest + svelte-check). Presentational components are desk-verified; that is a stated, accepted limitation, not an oversight.
- **If a colour needs changing**, re-measure contrast rather than eyeballing it. The three ratios in Global Constraints were computed, not guessed.
