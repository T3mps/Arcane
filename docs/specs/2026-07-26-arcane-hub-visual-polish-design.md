# Arcane Hub: visual polish — design

Status: **APPROVED** (section-by-section, 2026-07-26). Ready for an implementation plan.

Scope: the Hub's presentation layer only. Layout, tokens, typography, window chrome,
and the decomposition of one oversized file. No Rust command, no `api.ts` signature,
and no on-disk state field changes.

Design was chosen from mockups rendered at 1:1 in the real fonts during the session;
the mockup HTML is preserved under `.superpowers/brainstorm/54-1785063500/content/`
(`directions.html`, `accent.html`, `gold-status.html`, `logo-red.html`, `locked.html`).
`locked.html` is the approved reference.

## Problem

The Hub works and has been desk-verified, but it looks like a generic dark web page
rather than a product. Concretely:

- The whole UI is one 246-line `src/routes/+page.svelte` holding state, all seven
  command calls, every layout, and every style. Layout C adds a sidebar, two views,
  cards, and search on top of that — roughly 450 lines in one file, which is past
  the point of being comfortably editable.
- It uses the default OS titlebar and system fonts (Segoe/Bahnschrift) while its
  sibling `Setup.exe` ships custom chrome, a bundled font pair, an animated
  background, and a component kit. The two do not read as related products.
- `static/` still contains the scaffolding leftovers `svelte.svg`, `tauri.svg`,
  `vite.svg`.
- Engines is not a view but a section that force-shows itself when the list is
  empty, so the primary surface changes shape depending on state.

## Non-goals

- Engine version management, installs, downloads, accounts, templates gallery.
- Any change to `.arcproj`, mounts, the asset registry, or the engine probe.
- Making incompatible projects refuse to launch (see Behaviour — deliberate parity).
- A component library or SvelteKit routing. The view switch is a variable; a
  two-view desktop window does not need a URL model.

## Decisions

### Layout: "Forge" — sidebar shell + cover grid

A fixed 198px sidebar (nav + active-engine footer) beside a main area of
`Projects` or `Engines`. Chosen over two alternatives that were also mocked:
refining the current flat list, and adding product chrome without restructuring.

Rationale: it is the only one of the three with somewhere to *put* the next
feature. The other two keep projects in a flat list that a future gallery would
have to fight for space with.

**Sidebar ships exactly two items: Projects and Engines.** Templates and Learn
appeared in the first mockup and were cut — nav pointing at features that do not
exist makes a launcher feel like a mockup of itself. Adding a third item later is
one entry, not a re-layout. Reaffirmed by the user.

### Accent: gold, from Setup.exe's tokens

**Recorded because it went against the recommendation:** violet was recommended
(the Hub's existing engine-tonemap hue, and it leaves the brand colour free for
status). The user chose gold, for family resemblance with `Setup.exe`. A red
sampled from `arcane_logo.png` (`#A24349`, verbatim — 57,661 px, everything else
antialias fringe) was also mocked and rejected.

That decision stands and is what gets built. Two consequences are recorded so they
are not rediscovered:

1. **Gold cannot also mean "warning".** It is the primary action colour, so the
   incompatible-project state moved to coral + dimming (below). In the violet
   scheme amber stayed free for warnings, which is the convention the editor and
   the current Hub already use; gold gives that up.
2. **Cheap hedge, built in regardless:** the entire palette lives in
   `src/lib/theme.css` as tokens. Re-hueing the Hub later is editing that one
   file, not a rewrite.

### Three-state colour logic

Stated as a rule so it cannot drift:

| Token | Means | Applied to |
|---|---|---|
| `--gold #f0c869` | **act** | primary button, active nav rail, cover monogram |
| `--fail #f1949f` | **won't open** | the `abi` badge on an incompatible card |
| dimmed surfaces | **inert** | the entire incompatible card |
| `--ok #74dca2` | engine resolved | status dot in the sidebar footer |

Gold never means trouble; coral never means action.

**The invalid state is carried by two independent signals** — loss of warmth
(surface, border, and cover all drop to neutral greys) *plus* a bordered coral
`abi N` badge. Chosen over a straight hue swap and over an alert red, because it
survives a colour-blind viewer or a bad panel, and because "you picked the wrong
engine" does not deserve an alarm colour.

Token names are copied verbatim from `Tools/setup-wizard/src/lib/theme.css` so the
two files can be diffed.

### Typography

- **Display: Aldo** (`Arcane/data/font/aldotheapache/AldotheApache.ttf`) — the
  editor's own brand font, so the Hub's headings match the product it launches.
- **Body: Inter** (`Arcane/data/font/inter/static/`) — chosen over Setup's
  `PF-Din-Text-Universal-Medium.ttf`.

**Why not DIN, recorded:** PF DIN Text is a commercial Parachute typeface and
there is **no license file for it anywhere in the repo**, while Inter and Roboto
both ship their `OFL.txt`. `Setup.exe` already distributes DIN, so the exposure
exists today, but adding it to a second shipped binary widens it. At 12–13px the
difference is marginal, and the gold palette, chrome, radii, and Aldo headings
carry the family resemblance without it. The user chose Inter.

Copied into `Arcane/Hub/static/fonts/`: `AldotheApache.ttf`,
`Inter_18pt-Regular.ttf` (400), `Inter_18pt-SemiBold.ttf` (600), and Inter's
`OFL.txt`. Two Inter weights only — 400 for body, 600 for card titles and button
labels; anything heavier is Aldo's job. The Hub does not reach into
`Arcane/data/` or `Tools/` at build time — `Tools/` is retired prototype and may
be deleted.

### Window chrome

`decorations: false` plus a `WindowChrome.svelte` carrying `data-tauri-drag-region`
— the same mechanism `Setup.exe` uses (`WindowChrome.svelte:11`).

Capabilities: `Arcane/Hub/src-tauri/capabilities/default.json` **already grants**
`core:window:allow-minimize`, `allow-close`, and `allow-start-dragging`. The single
addition is `core:window:allow-toggle-maximize` (verified to exist in
`src-tauri/gen/schemas/desktop-schema.json`), because the window is
`resizable: true` and a maximize button that no-ops is worse than no button.
Double-clicking the drag region maps to the same toggle.

**Accepted cost:** losing OS decorations loses the native snap-assist hover menu.
Acceptable for a 1000×680 launcher, and the trade `Setup.exe` already made.

## Component decomposition

`+page.svelte` remains the only stateful file — it keeps every `invoke` call, the
`$state` declarations, and the `guard()` wrapper. Components take props and emit
callbacks, so each is readable and testable on its own.

```
src/lib/theme.css              tokens, @font-face, base, motion, scrollbar, focus ring
src/lib/format.ts              pure logic (see Verification) — extracted, unit-tested
src/lib/components/
  WindowChrome.svelte          drag region + minimize / toggle-maximize / close
  Sidebar.svelte               nav + active-engine footer
  ProjectCard.svelte           cover, name, abi/when, dimmed variant
  EngineRow.svelte             engine row: select / remove
  Button.svelte                gold | ghost | danger
  EmptyState.svelte            no-projects and no-engines
src/lib/views/
  ProjectsView.svelte          search + grid + new-project flow
  EnginesView.svelte           register / select / forget
src/routes/+page.svelte        state, guard(), view switch
```

Also delete the scaffolding leftovers `static/svelte.svg`, `static/tauri.svg`,
`static/vite.svg`.

## Behaviour

Unchanged unless listed:

- **Search** filters `state.recents` client-side on name and path. No Rust change.
- **Engines becomes a view**, not a section that force-shows when empty. The
  "register your first engine" case becomes `EmptyState` inside that view, and the
  nearby-engine suggestion (`suggestEngine`) lives there too.
- **Cover art** is the project's first letter over a gold gradient whose angle is
  derived from a hash of the project path, so a wall of cards stays
  distinguishable. No new data, nothing persisted. The hash algorithm is the
  plan's choice, but it **must be deterministic across processes** — a project's
  card changing appearance between launches would read as a bug. (This rules out
  anything seeded by insertion order or `Math.random`.)
- **The grid reflows.** Three columns at the default 1000px width, but the window
  minimum is 800px, so the grid is `repeat(auto-fill, minmax(...))` rather than a
  fixed `repeat(3, 1fr)` — at 800px the main area is only ~600px and a hardcoded
  three-up would crush the cards.
- **New-project stays inline.** The current `creating` toggle that reveals a name
  input plus "Choose folder and create" is kept as-is inside `ProjectsView`, not
  promoted to a modal. Modals are a bigger interaction change than this pass wants.
- **The error banner** (`error` state, set by every `guard()` failure) renders in
  the main area directly above the active view's content, styled from
  `--fail-accent`. It must not live in the sidebar or the titlebar: it is a
  response to an action taken in the main area, and it needs the width.
- **The mismatch sentence** ("Built against abi 5; the selected engine is abi 7.
  It will refuse to open.") survives as the card's accessible description rather
  than a row of body text below the list.
- **Incompatible projects stay clickable**, exactly as today. This is a visual
  pass; a behaviour change should not be smuggled into it. Making them refuse to
  launch is a clean follow-up.

## Motion and accessibility

- Setup's `--dur 200ms` / `--ease cubic-bezier(.22,.61,.36,1)`, and its
  `prefers-reduced-motion: reduce` block that collapses animation and transition
  durations.
- Gold `:focus-visible` ring; cards are `<button>`s, so the grid is
  keyboard-navigable in DOM order.
- Setup's scrollbar treatment (WebView2 is Chromium, so `::-webkit-scrollbar`
  applies).
- Contrast is verified numerically during implementation, not asserted here. The
  one figure already measured: white on the rejected logo red was 6.1:1. Gold
  `#f0c869` carries near-black `#120e04` text on buttons; coral `#f1949f` on
  `--surface-2` must be checked and darkened if it misses 4.5:1.

## Verification

**Automated — tooling already exists, none added.** `package.json` already wires
`npm run check` (svelte-check) and `npm test` (vitest).

- `src/lib/format.ts` gets the three pieces of real logic, unit-tested: the
  ABI-compatibility predicate, the search filter, and the monogram/gradient
  derivation. The compatibility rule is the one thing this entire UI exists to
  express, so it gets a test rather than living inline in markup.
- `npm run check` must pass clean.
- `npm run build:hub` must bundle and stage.
- Rust: cargo tests and clippy are untouched by construction — the only
  non-frontend edit is one capability string.

**Not automated.** Every pixel. Layout, the chrome's drag and double-click
behaviour, the dimmed treatment, and focus order are desk-verify, with the same
honesty applied to the editor's ImGui surfaces. The desk-verify list belongs in
the implementation plan.

## Follow-ups and known risks

1. **Aldo's license is undocumented.** `AldotheApache.ttf` ships in the editor and
   in `Setup.exe` with no license file. Not this task's job to resolve, but it is
   now shipping in a third binary and someone should confirm the terms.
2. **PF DIN's license is undocumented** and it ships in `Setup.exe` today. Avoided
   here; still outstanding there.
3. **Incompatible projects remain launchable** — deliberate parity, easy follow-up.
4. **The gold/violet question is reversible** by design (one token file), if the
   Hub later wants to read as the engine's own product rather than Setup's sibling.
5. **`Tools/` is retired** and may be deleted; the Hub therefore vendors its own
   font copies rather than referencing `Tools/setup-wizard/static/fonts/`.
