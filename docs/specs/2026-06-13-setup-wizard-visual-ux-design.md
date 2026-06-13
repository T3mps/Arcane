# Setup Wizard - Visual & UX Overhaul (Direction D)

**Date:** 2026-06-13
**Status:** Approved (brainstorm 2026-06-13)
**Scope:** Re-skin and upgrade the UX of the existing Tauri 2 + SvelteKit setup wizard
(`Tools/setup-wizard/`) into a polished, on-brand "premium launcher" experience.
The orchestrator (`scripts/setup.ps1`), the `@@WIZ` marker contract, the Rust
parser, and the Svelte reducer are **unchanged** - this overhaul only changes how
`WizState` is presented.

## Context

The wizard works end-to-end but looks like bare functional HTML (default fonts,
no theme, three plain screens). The backend is solid: `setup.ps1` emits `@@WIZ`
markers, the Rust core parses them into `WizEvent`s and emits a `"wiz"` Tauri
event per line, and a unit-tested Svelte reducer folds those into `WizState`
(`steps`, `doctor[]`, `doctorOk`, `log[]`, `error`, `finished`). The screens are
`src/routes/+page.svelte` (router), `src/lib/screens/{Doctor,Options,Run}.svelte`,
over `src/lib/wizard.svelte.ts` (reducer). The project has a fully-realized visual
identity in `Client/src/ui/theme.lua` (neon-space / sci-fi minimalist): deep
blue-black canvas, gold + neon accents, condensed display type, hairline HUD
borders, ghost-glow buttons.

## Goal

Make the first thing a contributor sees feel like part of Aphelyon and a pleasure
to use: an authentic, calm-premium visual system plus concrete UX upgrades
(progress rail, install-link affordances, an Advanced disclosure, a real
Success/Failure terminal). Zero changes to setup behavior.

### In scope
- A design-system layer (color/type/motion tokens) applied across the UI.
- Bundled real fonts (Aldo display + DIN data) from the client.
- A frameless custom window shell (title bar + drag + min/close) with a starfield.
- A persistent step rail (Prerequisites / Options / Install).
- Rewrites of the three screens against reusable components, plus a new
  Success/Failure (`Result`) terminal screen.
- Tasteful motion (screen transitions, row reveal, running pulse, progress fill,
  success flourish), honoring `prefers-reduced-motion`.
- Rebuild + recommit `Setup.exe`.

### Out of scope (non-goals)
- Any change to `scripts/setup.ps1`, `doctor.bat`, the `@@WIZ` grammar, the Rust
  `orchestrator.rs` parser, or the `reduce()`/`WizState` logic. The reducer's
  Vitest suite and the Rust parser tests must still pass unchanged.
- New screens beyond `Result`. No Welcome/landing screen.
- Sound, telemetry, light theme, or i18n.
- Changing which workspaces/flags the wizard supports.

## Locked decisions (brainstorm 2026-06-13)

- **Direction D**: keep A's deep-space starfield background and condensed all-caps
  display title; take B's restraint - gold-forward accent, calm solid surfaces,
  soft warm glow (not neon HUD).
- **Frameless custom chrome** (`decorations: false`), our own title bar.
- **Flow**: Prerequisites -> Options -> Install -> Success/Failure. The plain
  "Setup complete." line is replaced by a real terminal screen.
- **Fonts**: bundle the real Aldo + DIN TTFs from `Client/data/font/`.

## Visual system

A single global stylesheet (`src/lib/theme.css`, imported once in
`src/routes/+layout.svelte` or `+page.svelte`) defines CSS custom properties.
All components consume tokens; no ad-hoc hex in components.

### Color tokens (derived from `theme.lua`, gold-forward per Direction D)
```
--bg-top:        #0a0e17   /* canvas gradient top   */
--bg-bottom:     #080b12   /* canvas gradient bottom */
--surface:       #141927   /* row / panel fill       */
--surface-2:     #10151f   /* recessed wells, log bg #06090f */
--border:        #222a3b   /* hairline border        */
--border-soft:   #1c2230
--text:          #eef2fa   /* primary text           */
--text-muted:    #8593aa   /* secondary / labels     */
--text-dim:      #5d6a82   /* upcoming / disabled    */
--gold:          #f0c869   /* PRIMARY accent (the mockup gold the user picked) */
--gold-bright:   #ffd161   /* brighter highlight / glow (theme.lua gold) */
--ok:            #74dca2   /* pass / done            */
--warn:          #f0c869   /* warn (shares gold)     */
--fail:          #f1949f   /* fail text; #ff5c6b accent */
--nebula-cyan:   rgba(80,150,200,.10)   /* atmosphere only */
--nebula-violet: rgba(180,120,210,.07)
```
Glow is expressed as low-alpha gold box/text-shadows, used sparingly (title,
active step, primary button, progress fill).

### Type tokens
- **Display** (`--font-display`): `Aldo` (bundled), uppercase, letter-spacing
  ~.06-.07em. Used for screen titles and the wordmark. Fallback:
  `"Bahnschrift","DIN Condensed","Oswald",sans-serif`.
- **Data/UI** (`--font-ui`): `DIN` (bundled) for body, labels, buttons, rows.
  Fallback: `ui-sans-serif, system-ui, sans-serif`.
- **Mono** (`--font-mono`): `ui-monospace, "Cascadia Code", Consolas, monospace`
  for the log pane and version strings. Not bundled.
- Sizes: title ~25-28px, section ~18px, body ~13px, label ~10-11px (uppercase),
  log ~11px. (Final values tuned during implementation.)

Bundled TTFs live at `Tools/setup-wizard/static/fonts/`
(`AldotheApache.ttf`, `PF-Din-Text-Universal-Medium.ttf`, copied verbatim from
`Client/data/font/`), declared via `@font-face` in `theme.css`. `static/` is
served at the web root by adapter-static, so URLs are `/fonts/<file>.ttf`.

### Shape, spacing, elevation
- Radius: window 12px (if rounded corners land), panels/rows 8px, buttons 7px,
  pills/icons 50%.
- Hairline 1px borders using `--border`; a 1px gold top accent rule under the
  title bar (`linear-gradient(90deg, transparent, rgba(240,200,105,.55), transparent)`).
- Surfaces are solid (calm), not the client's translucent shaders.

### Motion
- Library: Svelte built-in transitions/`tweened`/`crossfade` (no extra deps) or a
  tiny CSS-keyframe set. Durations 150-250ms, ease-out.
- Screen change: fly/fade between Prereqs/Options/Install/Result.
- Doctor rows: staggered reveal as they arrive (fade + 4px rise).
- Running step: soft 1.4s gold pulse on the spinner/row.
- Progress bar: animated width tween.
- Success: a one-shot restrained gold flourish (glow bloom + checkmark draw),
  ~600ms, no looping.
- **All motion gated on `@media (prefers-reduced-motion: reduce)`** - reduce to
  instant state changes; starfield stops drifting.

### Starfield
- A `Starfield` component behind all content (fixed, `pointer-events:none`,
  `z-index:0`). Implementation: small `<canvas>` with ~60-90 stars drifting
  slowly (parallax twinkle), capped low for CPU; or a CSS multi-radial-gradient
  star layer with a slow translate if canvas proves heavy. Plus two static faint
  nebula radial glows (cyan top-right, violet bottom-left). Respects reduced-motion
  (freezes). Density/liveliness: "gently drifting", subtle - it must never
  distract from the content.

## Window shell & chrome

- `tauri.conf.json`: `app.windows[0].decorations = false`, size **760 x 600**,
  `resizable: false`, `center: true`, title "Aphelyon Setup". Rounded corners are
  a nice-to-have via `transparent: true` + CSS radius; **if Windows transparency
  is finicky, ship square corners** (do not block on it).
- `WindowChrome` component (top of every screen): a 32px bar with
  `data-tauri-drag-region` (drag the window), the wordmark `APHELYON` (display
  font) + `· SETUP` (muted), and right-aligned **minimize** and **close** buttons.
  Below it, the 1px gold accent rule.
- Window controls call `@tauri-apps/api/window` `getCurrentWindow().minimize()` /
  `.close()` (JS only, no Rust). Capabilities must allow these (see Code mapping).
- **Close-mid-run safety**: if a run is in progress (`steps` has a `running`
  entry and `!finished`), the close button first `invoke('cancel')` (kills the
  spawned child so it is not orphaned) and then closes. A small inline confirm
  ("Setup is running - cancel and quit?") is acceptable but optional.

## Step rail

`StepRail` component under the chrome on Prereqs/Options/Install (hidden on the
final Result screen, or shown all-done). Three steps: `1 Prerequisites`,
`2 Options`, `3 Install`. Each shows upcoming (dim) / active (gold pill + glow) /
done (green check) based on the current `screen`. Numbered-row style (from the
mockup), not a vertical sidebar.

## Screens

### 1. Prerequisites (`Doctor.svelte`)
- **Source**: `st.doctor[]` (rows), `st.doctorOk` (gate). Doctor runs on launch
  and on Re-check (`run_doctor`).
- **Scanning state** (UI-derived, no reducer change): while the doctor run is in
  flight and `st.doctor` is empty (the router resets `st` before invoking),
  show a centered scanning animation ("Scanning your environment" + animated
  dots/sweep). As rows arrive they replace it with a staggered reveal.
- **Rows**: each is a `StatusRow` - circular status icon (green check / amber `!`
  / red `x`), bold item name, muted detail (`msg`). Status color from
  pass/warn/fail.
- **Install affordance (UX upgrade)**: a `fail` (and optionally `warn`) row shows
  a small **Install** / **Open** ghost button that opens the relevant help URL via
  the opener plugin. Mapping (UI-side lookup keyed by item name):
  - `Visual Studio` -> `https://visualstudio.microsoft.com/`
  - `docker` -> `https://www.docker.com/products/docker-desktop/`
  - `vcpkg` -> `https://github.com/microsoft/vcpkg` (or the repo's Server/BUILD.md note)
  - `overlay triplet` / vendored deps -> a short hint (no link).
  Unmapped items show no button. This is presentation-only; it does not change
  doctor output.
- **Footer**: `Re-check` (ghost, re-invokes doctor) and `Continue` (gold primary,
  `disabled` unless `st.doctorOk`). When blocked, a one-line hint under the button
  ("Fix the red items, then re-check"). Warnings never block.

### 2. Options (`Options.svelte`)
- Emits the same camelCase args object consumed by the Rust `SetupArgs`
  (`workspaces, vcpkgRoot, dbPort, skipVcpkg, config, build, doctorOnly:false`) -
  unchanged contract.
- **Layout**: three workspace toggles as selectable cards (Server / Arcane /
  Client) with one-line descriptions; a build-config segmented control
  (Debug / Release / Dist); a "Build after generating" switch.
- **Advanced disclosure (UX upgrade)**: `VCPKG_ROOT` (text + "auto-detect"
  placeholder) and `DB port` (number, min 1 max 65535, `?? 5432` fallback) live
  inside an **Advanced** section that is **collapsed by default**.
- **Summary line** above the action: a derived one-liner of what will run, e.g.
  "Will set up Server + Arcane (skip vcpkg) in Debug." Updates live with the form.
- **Footer**: `Back` (ghost -> Prereqs) and `Run setup` (gold primary -> Install).

### 3. Install (`Run.svelte`)
- **Source**: `st.steps` (status map), `st.log[]`, `st.error`, `st.finished`.
- **Expected-step list (UX upgrade)**: the screen receives the chosen run args
  (passed from the router alongside `st`) and computes the *expected* ordered step
  ids (doctor + per-workspace vcpkg/generate/db + client + build, honoring
  `skipVcpkg`/`build`). The checklist renders the full expected sequence with
  per-step state: done (green check), running (gold spinner + pulse), pending
  (dim, no icon). This shows what's coming, not just what has started.
- **Progress bar**: gold fill = done_count / expected_count (animated). Optional
  indeterminate shimmer on the running step.
- **Log pane** (`LogPane`): the streamed lines (`st.log`), mono, dark
  (`#06090f`), auto-scrolled to bottom, color-coded by simple heuristics
  (lines starting `>` muted, lines with `[x]`/`ok`/done green, errors red). Tail
  capped (e.g. last ~300 lines). A subtle fade at the top edge.
- **Footer while running**: `Cancel` (danger ghost, `invoke('cancel')`).
- When `st.finished` is set, the router advances to the Result screen (the 4th
  routed screen state).

### 4. Result (Success / Failure) - new (`Result.svelte`)
- **Source**: `st.finished` (`'ok'`/`'fail'`), `st.error`, `st.log`.
- **Success**: a restrained gold flourish + large check, headline "Setup
  complete", a short next-steps list, and actions:
  - **Open `Aphelyon.slnx`** -> `invoke('open_solution')`, a small new Rust
    command that opens `repo_root().join('Server','Aphelyon.slnx')` (the repo root
    is exe-relative and only known Rust-side, so this stays in Rust and reuses the
    existing `repo_root()` helper; it opens via the opener mechanism / `std::process`,
    needing no extra JS capability). If the solution file is missing the command
    returns an `Err` the UI disables/ignores.
  - **Copy log** -> copies `st.log.join('\n')` to the clipboard.
  - **Close** -> `getCurrentWindow().close()`.
- **Failure**: red-accented header "Setup failed", the failing step id + `st.error`
  surfaced prominently, a fix hint, and actions **Copy log**, **Retry** (re-runs
  with the same args -> back to Install), and **Close**.

## Component inventory

All under `Tools/setup-wizard/src/lib/`. Each is small, single-purpose, typed
`$props()`:
- `components/WindowChrome.svelte` - title bar, drag region, min/close.
- `components/Starfield.svelte` - background canvas/CSS starfield + nebula.
- `components/StepRail.svelte` - the 3-step progress indicator (`current` prop).
- `components/StatusRow.svelte` - one prereq/checklist row (icon + text + optional action).
- `components/LogPane.svelte` - auto-scrolling colorized log (`lines` prop).
- `components/Button.svelte` - variants `primary` (gold) / `ghost` / `danger`,
  `disabled`, optional icon. (Or plain styled `<button>`s if a component is overkill.)
- `screens/Doctor.svelte`, `Options.svelte`, `Run.svelte` (rewritten),
  `Result.svelte` (new).
- `theme.css` - tokens, `@font-face`, base element styles, motion + reduced-motion.
- Small pure helpers (unit-testable): `lib/steps.ts`
  (`expectedSteps(args) -> string[]`) and `lib/links.ts`
  (`installLink(item) -> string | null`).

## Code mapping (what changes; what stays)

**Changed/new files** (all under `Tools/setup-wizard/`):
- `src/routes/+page.svelte` - router gains a 4th screen (`result`), passes the
  chosen run args to `Run`/`Result`, routes to `result` when `st.finished` is set,
  imports `theme.css`, mounts `Starfield` + `WindowChrome` shell around the routed
  screen. Keeps the existing `onMount` listen+cleanup and the invoke-reject ->
  `done=fail` guards.
- `src/lib/theme.css` (new), `static/fonts/*.ttf` (new, copied), the components
  above (new), the three screens (rewritten), `Result.svelte` (new),
  `lib/steps.ts` + `lib/links.ts` (new, tested).
- `src-tauri/tauri.conf.json` - `decorations:false`, size 760x600,
  (optional `transparent:true`).
- `src-tauri/capabilities/default.json` - add window permissions
  (`core:window:allow-minimize`, `core:window:allow-close`,
  `core:window:allow-start-dragging`) and the opener permission for opening a URL
  from JS (`opener:allow-open-url`, for the Prereqs install links). The
  `open_solution` command is Rust-initiated, so it needs no JS capability.
  **Adapt the exact identifiers to the scaffold's plugin versions per the
  API-adaptation rule**; scope them minimally.
- `src-tauri/src/lib.rs` - gains ONE small read-only command `open_solution()`
  (opens `<repo_root>/Server/Aphelyon.slnx`, reusing `repo_root()`). Registered in
  the existing `generate_handler!`. Nothing else in the Rust core changes.
- `Setup.exe` (repo root) - rebuilt at the end.

**Unchanged (must stay byte-equivalent in behavior):**
- `scripts/setup.ps1`, `scripts/setup.bat`, `scripts/doctor.bat`.
- `src-tauri/src/orchestrator.rs` (parser + its 5 tests) - fully unchanged.
- `src-tauri/src/lib.rs` spawn/stream/cancel commands, `SetupArgs`, `Running`
  state, and `repo_root()` - unchanged. The ONLY addition is the small
  `open_solution()` command above. Window controls + install-link URL opens are
  JS via `@tauri-apps/api` / the opener plugin.
- `src/lib/wizard.svelte.ts` (`reduce`/`initialState`/`WizState`) and its 6
  Vitest cases. The new screens consume `WizState` as-is.

## State & data flow

- `WizState` remains the single source the UI renders. New UX bits are **derived**,
  not new reducer fields: scanning = (doctor run in flight and `doctor` empty);
  progress = done/expected from `steps` + the run args; failing-step = the `steps`
  entry with status `fail`.
- The router holds the chosen run args (already built when invoking `run_setup`)
  and passes them to `Run`/`Result` so the checklist/progress know the expected
  sequence and Retry can re-run identically.
- `@@WIZ` contract is untouched; `parse_line` -> `"wiz"` event -> `reduce` ->
  `WizState` is the same pipeline.

## Accessibility & input

- All controls reachable by keyboard; visible focus ring (gold, low-alpha).
- Buttons are real `<button>`s; inputs keep `<label>` association (the config
  select stays labeled). Status conveyed by icon + text, not color alone.
- `Enter` advances on Prereqs (if not blocked) and Options; `Esc` does **not**
  close mid-run (avoid accidental quit).
- Respect `prefers-reduced-motion`.

## Testing & verification

- **Keep green**: Rust `cargo test` (5 parser cases), Vitest reducer (6 cases) -
  unchanged.
- **New unit tests (TDD)**: `steps.test.ts` for `expectedSteps(args)` (e.g.
  server+arcane, `skipVcpkg`, client-only, `build`), and `links.test.ts` for
  `installLink(item)` (known items map, unknown -> null).
- **Build/lint**: `npm run build` exits 0; `npm run check` stays at 0 errors.
- **No `tauri dev`/release build in agent tool calls** (blocking GUI / long). The
  release build + the `Setup.exe` rebuild run in the background / at the end.
- **Human acceptance gate**: double-click the rebuilt `Setup.exe` and walk all
  four screens (scanning -> rows + install links, Options + Advanced, Install with
  live log + progress + cancel, Success with Open/Copy and Failure with Retry),
  plus reduced-motion. This is the visual sign-off; it cannot be done headlessly.

## Distribution

- Rebuild `Setup.exe` via `npm run tauri build -- --no-bundle` ->
  `target/release/setup-wizard.exe` -> copy to repo-root `Setup.exe`. The CI
  workflow already refreshes it on `Tools/setup-wizard/**` changes.
- The bundled TTFs add a few hundred KB to the wizard source and the exe; acceptable.

## Risks / open items (settle in the plan)

- **Windows frameless rounded corners / transparency** can be finicky; square
  corners are an acceptable fallback - do not block.
- **Starfield CPU**: cap density and prefer CSS or a throttled canvas; freeze on
  reduced-motion. Verify it idles cheaply.
- **Opener capability identifiers** vary by plugin version; adapt to the scaffold
  and keep scope minimal.
- **`Open Aphelyon.slnx`** depends on the exe sitting at the repo root (true for
  the committed `Setup.exe`); in `tauri dev` the path differs - acceptable (dev
  is not the shipped path).
