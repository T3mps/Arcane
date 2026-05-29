# Pause Menu Redesign — Genshin-style Launcher Hub

**Date:** 2026-05-28
**Status:** Brainstormed → spec'd. Plan to follow.
**Driver:** Replace the centred-panel pause with a left-rail + grid-launcher pattern modelled on Genshin Impact's pause menu. Pause becomes the canonical production-navigation surface.
**Reference:** `GachaClient/.screenshots/references/pause.png`
**Cross-references:** `[[project_inventory_party_rework]]`, `[[project_currency_model]]`, `[[homogenized_rendering]]`.

---

## Goals and non-goals

**Goal.** Replace the current centred-panel pause menu (300×~280, four icon+label rows) with a Genshin-style launcher hub: thin left rail (X / Settings / Logout) + main panel showing a profile card on top-left and a 4×2 tile grid for the four game screens (Gacha, Inventory, Party, Dailies). Pause becomes the canonical in-game navigation surface; F-key shortcuts (already remapped by the user: F3 = Gacha, F4 = Dailies, T = Party, C = Inventory) coexist as fast dev jumps.

**The new pause owns:**
- The **left rail** (close / settings / logout meta-actions)
- The **profile card** (player identity surface — name, UID, currency wallet, login streak, last-login date)
- The **launcher grid** (navigation tiles to the four game screens)
- The **dimmed-world background composite** (existing pattern, kept; moved into `drawScene`)
- The **two-phase slide animation** on open/close (rail leads, main panel trails; reverse on close)

**The new pause does NOT own:**
- The destinations (Gacha / Inventory / Party / Dailies / Settings screens stay as their own screens; pause just pushes them on the screen stack)
- Combat / abilities gameplay (`[[feedback_abilities_system_ownership]]`)
- A redesign of any destination (F2 inventory + F3 party internals slated for separate rework per `[[project_inventory_party_rework]]`)
- F-key dev keybinds (user already remapped; they stay as fast jumps)

**Explicit non-goals:**
- Full Genshin-feature-parity launcher. We have four real tiles for M1; the grid is sized for ~8 tiles to leave room for future additions (no shop, events, battle pass, co-op, etc.).
- New gameplay surfaces. This is purely navigation.

---

## Layout

Designed at the 1920×1080 reference; everything anchors so it scales.

### Left rail

- **Width:** 56px
- **Position:** anchored to the screen's left edge, full height
- **Three vertical regions:**
  - **Top cluster** (~70px tall): `X` close icon + 1px divider line below
  - **Middle cluster** (vertically centred in remaining space): tab icons — M1 just the `settings` gear icon
  - **Bottom cluster** (~70px tall): 1px divider line + `log-out` icon
- Icons centred horizontally in the rail. Each icon is a 40×40 hit zone; the icon glyph inside is 24×24.

### Main panel

- **Left margin:** rail-right + 32px gutter
- **Top margin:** 32px
- **Right margin:** 48px
- **Bottom margin:** 32px

No visible "panel" container — the dimmed world shows through the gutter between rail and content. The profile card and tile grid float independently.

### Profile card (top of main panel)

- **Width:** fills the main panel content area
- **Height:** 180px
- **Layout:** 140×140 avatar on the left + text column on the right
  - Player name (large font)
  - UID + copy button (tiny font, muted; clipboard icon hover-brightens)
  - Login-streak chip (pill-shaped, gold tint, flame icon + "N days")
  - Last-login date (tiny font, muted; e.g. "Last login: 2026-05-28 21:34")
  - Currency wallet row at the bottom — 5 icon+number pairs (credits / universalCredits / tickets / limitedTickets / scrap)

### Tile grid (below profile card, 24px gap)

- **Grid shape:** 4 columns × 2 rows
- **Tile size:** 160×160
- **Gaps:** 24px column gap, 24px row gap
- **M1 tiles** (top row, slot 1–4): `[Gacha] [Inventory] [Party] [Dailies]`
- **Slots 5–8:** empty / not drawn (placeholders for future screens)
- Each tile: rounded-corner frosted-glass card with a centred icon (48×48) above a centred label (medium font)

### Background dim layer

- The full-screen dimmed-world rectangle moves from `PauseScreen:draw()` to a proper `PauseScreen:drawScene(ctx)` (see Prerequisite below).
- Dim opacity: 60% black (was 72% in the current pause — softens slightly so the world feels present-but-recessed).

---

## Open / close animation

Two-phase sequential slide. Total open: 220 ms. Total close: 180 ms (close is snappier than open).

### Open sequence

```
t = 0 ─────────────► t = 220ms
│
├─ Rail slide:    t = 0   → 100ms   (100ms, ease-out-cubic)
│  X = -56px      → X = 0px
│
└─ Main panel slide:  t = 80ms → 220ms  (140ms, ease-out-cubic)
   X = -fullWidth   → X = 0px
   (starts 80ms after rail starts so the rail leads visibly by ~80% of its travel)
```

- Rail slides in **first** from the left edge, ease-out-cubic.
- 80 ms later, while the rail is still finishing its slide, the main panel starts.
- The main panel (profile card + tile grid) animates as one composite — no per-tile stagger.
- ease-out-cubic gives a "decisive landing" — no overshoot.

### Close sequence (reverse, faster)

```
t = 0 ─────────────► t = 180ms
│
├─ Main panel slide:  t = 0   → 120ms   (120ms, ease-in-cubic)
│  X = 0px        → X = -fullWidth
│
└─ Rail slide:    t = 60ms  → 180ms  (120ms, ease-in-cubic)
   X = 0px       → X = -56px
   (starts 60ms after main panel starts so the rail trails out behind it)
```

- Main panel slides OUT first (mirrors how it arrived last).
- Rail slides out second.
- ease-in-cubic gives an "accelerating departure" feeling.

### State machine

```
CLOSED → OPENING → OPEN → CLOSING → CLOSED
         (220ms)         (180ms)
```

- Input ignored during OPENING and CLOSING.
- ESC during OPENING: ignored.
- ESC during OPEN: triggers CLOSING.
- ESC during CLOSING: no-op.
- Clicking X: triggers CLOSING.
- Clicking a tile: pause stays on the stack underneath the destination; no animation runs.

### Implementation

A single timer `self.animT` + `self.animState ∈ { "closed", "opening", "open", "closing" }`. Each frame:
- `railProgress = clamp((animT - 0) / 100ms, 0, 1)` → eased → drives rail X
- `panelProgress = clamp((animT - 80ms) / 140ms, 0, 1)` → eased → drives main panel X

Inverted for the close phase. ~50 lines of state machine in `src/ui/screens/pause/animation.lua`, headless-testable.

---

## Visual aesthetic

Builds on the existing theme primitives (`ui.theme`, `ui.icons`, `ui.util.draw`, `ui.components.FrostedGlass`). Same neon-HUD language as the current pause, restructured.

### Left rail

- **Background:** frosted glass sampling the dimmed-world canvas + `theme.colors.surface` overlay at 0.85 alpha
- **Right edge:** 1px `theme.colors.borderLight` vertical line (delimiter between rail and main panel)
- **Top/bottom dividers** (around X and Logout): 1px horizontal lines, `theme.colors.borderLight` at 0.5 alpha, inset 8px
- **Icon idle:** `theme.colors.textMuted`
- **Icon hover:** `theme.colors.accent` (cyan) + 3px-wide accent-coloured bar on the right edge of the rail item, full height of the hit zone, at 0.85 alpha
- **Icon pressed (active tab — Settings open):** solid accent bar on the right edge at 1.0 alpha + icon at `theme.colors.accent`
- **Logout icon:** idle `theme.colors.gold` (signals destructive-but-confirmable), hover → `theme.colors.red`
- **X icon:** idle `theme.colors.textMuted`, hover → `theme.colors.text` (neutral, no accent)

### Profile card

- **Background:** frosted glass + `theme.colors.surface` at 0.94 alpha, rounded corners (radius 14), 1px `theme.colors.borderLight` border
- **Accent stripe:** 2px-tall `theme.colors.accent` at 0.85 alpha along the top edge (matches current pause-panel pattern)
- **Avatar:** 140×140, rounded-corner (radius 12) clipped image; placeholder (`Icons.draw("user")` over a flat colour) until real portrait data exists
- **Name:** theme `large` font, `theme.colors.text`
- **UID + copy button:** `tiny` font with `textMuted` for the UID; 20×20 `clipboard` Lucide icon to the right, hover-brightens
- **Login streak chip:** pill-shaped, `flame` icon + "N days", `theme.colors.gold` tint
- **Last login date:** `tiny` font, `textMuted`
- **Currency wallet row** (bottom of card): 5 icon+number pairs evenly spaced, right-aligned numbers. Per `[[project_currency_model]]`: credits (Stellar Jade), universalCredits (Oneiric Shards), tickets, limitedTickets, scrap (Mora).

### Tile grid

- **Tile:** 160×160, rounded corners (radius 12)
- **Idle:** frosted glass + `theme.colors.surface` at 0.85 alpha, 1px `theme.colors.borderLight` border
- **Hover:** surface lifts to 0.94 alpha + `theme.colors.accent` border at 0.85 alpha + a subtle accent glow (second-pass rounded-rect at slightly larger radius, low alpha)
- **Pressed** (brief moment between click and screen transition): tile scales down to 0.96× from centre — ~80ms
- **Icon:** 48×48 centred horizontally, vertically at 35% mark of the tile. `theme.colors.accent` hover, `theme.colors.text` idle.
- **Label:** `medium` font, centred, at 70% mark of the tile
- **M1 tiles:** Gacha (`gift` or `sparkles`), Inventory (`package`), Party (`users`), Dailies (`calendar` or `trophy`)

---

## Behavior

### Opening

- **ESC during PLAYING** → opens pause. Animation triggers.
- **ESC while another non-pause screen is on top** → that screen handles ESC; pause doesn't interfere.

### Rail interactions

- **X (close):** triggers CLOSING animation; on completion `UI.pop()`.
- **Settings:** `UI.push(ui.screens.settings)`. Pause stays mounted underneath, dormant. When user pops Settings (ESC inside it), pause becomes visible again — *no re-animation*; it was already in OPEN state.
- **Logout:** confirmation modal (small centred dialog: "Save & log out?" + Cancel/Confirm). Confirm → existing `:saveAndLogout()` flow (preserve from current `init.lua`). Rail logout icon shows a small spinner during the network round-trip.

### Tile interactions

- **Tile click** (Gacha / Inventory / Party / Dailies):
  - `UI.push(<destination>)` — pause stays on stack underneath
  - Destination opens via its own normal screen transition
  - ESC inside destination → `UI.pop()` → destination removed → pause becomes visible again at OPEN state
  - ESC inside pause → triggers CLOSING animation → `UI.pop()` → world

This is the screen-stack walk-back pattern: each ESC pops one screen at a time. Matches the rest of the UI.

### Settings tab active-state indicator

The rail's Settings icon shows the "pressed" visual treatment (accent bar at full opacity) while the Settings screen is on the stack above pause.

Mechanism: pause tracks `self._activeChild` set on the tile / rail click that pushed the screen. Cleared in pause's `:update` each frame by checking "am I top of stack now? if so, my child was popped, clear the ref."

### Input gating

- Mouse + keyboard fully blocked from reaching the world while pause is on top
- During OPENING / CLOSING: only ESC is recognised (and it acts per state machine rules)
- Hover detection runs every frame in OPEN state; reset on state transitions

---

## Implementation outline

### Prerequisite (must land first, separate commit)

**Fix pause's world-rendering.** Add `PauseScreen:drawScene(ctx)`:
- Draws nothing for the "background producer" role (no clear, no overlay — let the world scene canvas remain intact)
- Composites a dim layer (`setColor(0, 0, 0, 0.6)` full-screen rect) into the scene canvas before the UI layer renders

The Pipeline's `_composeScreensScene` already renders the world when PLAYING + world exist, then iterates `screens` and calls `:drawScene` on each. The current pause has no `drawScene`, so it's skipped — the world renders into the scene canvas but pause's `:draw()` paints opaque black on top in the UI pass. Moving the dim into `drawScene` puts it INSIDE the scene canvas, then `:draw()` only handles the rail + profile card + grid widgets on top.

This is the hard prereq — the new design's frosted glass + transparency only makes sense over a visible dimmed world.

**Files for prereq:** `src/ui/screens/pause/init.lua` only. ~10 lines added (drawScene method), ~3 lines removed (the full-opacity black rect at top of `:draw()`).

**Validation:** visual — pause opens, world is visible at 40% brightness behind the panel. No other pause-related visual change.

### M1 files

| File | Status | Estimated LoC |
|---|---|---|
| `src/ui/screens/pause/init.lua` | rewrite | ~350 (was 205) |
| `src/ui/screens/pause/rail.lua` | new | ~80 |
| `src/ui/screens/pause/profile_card.lua` | new | ~120 |
| `src/ui/screens/pause/tile_grid.lua` | new | ~100 |
| `src/ui/screens/pause/animation.lua` | new | ~50 |
| `src/services/SessionData.lua` *or extend existing* | possibly new | thin facade for player name / UID / currency / streak / last-login |
| `src/ui/icons/` | possibly add | any missing tile/rail icons not in Lucide |
| `src/tests/render_harness/main.lua` | extend | animation state machine tests (~60 LoC added) |

### Animation API

`src/ui/screens/pause/animation.lua` exports:

```lua
local Anim = require "ui.screens.pause.animation"
local anim = Anim.new()
anim:open()                     -- transitions CLOSED -> OPENING
anim:close()                    -- transitions OPEN -> CLOSING
anim:update(dt)                 -- advances animT; auto-completes to OPEN / CLOSED
local railX, panelX = anim:offsets()  -- screen-space X offsets for rendering
anim:state()                    -- "closed"|"opening"|"open"|"closing"
anim:inputEnabled()             -- false during opening/closing
```

PauseScreen calls `anim:open()` in construction and `anim:close()` on ESC/X. Render uses `anim:offsets()` to position rail / panel.

### Screen base-class transition override

PauseScreen overrides `getTransitionValues()` to return `(1, 1)` so the Screen base class's scale/alpha transition is a no-op — our slide is the entire transition:

```lua
function PauseScreen:getTransitionValues()
    return 1, 1   -- pause uses its own slide animation; no scale/alpha pulse
end
```

If the override mechanism is different in `src/ui/core/screen.lua`, adapt — but the principle stands: disable the parent class's transition.

### Settings active-state tracking

PauseScreen stashes the pushed screen ref:

```lua
function PauseScreen:_openSettings()
    self._activeChild = require("ui.screens.settings").new()
    UI.push(self._activeChild)
end

function PauseScreen:update(dt)
    -- ... existing update body ...
    -- Clear child ref when we're back on top of the stack.
    if self._activeChild and UI.stack:peek() == self then
        self._activeChild = nil
    end
end

function PauseScreen:isSettingsActive()
    return self._activeChild ~= nil
end
```

### Data plumbing for profile card

| Field | Source | M1 strategy |
|---|---|---|
| Player name | `SessionState.playerName` via `Network.getSessionInfo()` | Real data |
| UID | `SessionState.playerId` via `Network.getSessionInfo()` | Real data |
| Currency wallet | `state.player.wallet` (or equivalent; verify exact path) — populated by `Network.getState()` response | Real data |
| Login streak | **not tracked anywhere** | STUB: "1 day" fixed; flag for server work |
| Last login date | **not tracked clientside** | STUB: "Today" fixed; flag for server work |

Comments in the code: `-- STUB: requires server to send login_streak + last_login fields in GetState response.`

### Headless tests

`src/tests/render_harness/main.lua` gets a new "PauseAnim (PauseM1)" block:
- Synthetic dt sequence drives CLOSED → OPENING → OPEN → CLOSING → CLOSED
- Offsets correct at key timestamps (rail X at t=50ms; panel X at t=150ms; both at 0px at t=220ms; etc.)
- ease-out-cubic math
- inputEnabled() gating during transitions

No visual test for the actual rendering — that's the visual gate at M1's end.

### Currency icons (M1 stubs)

| Field | Real name | M1 Lucide proxy |
|---|---|---|
| credits | Stellar Jade | `gem` |
| universalCredits | Oneiric Shards | `sparkles` |
| tickets | (standard pulls) | `ticket` |
| limitedTickets | (limited pulls) | `ticket-percent` or `ticket-x` |
| scrap | Mora | `coins` |

Document substitutions in code; swap when real art lands.

---

## Risks and mitigations

### R1 — `drawScene` prereq might not slot cleanly into the existing scene compositor

The Pipeline's `_composeScreensScene` has a fast path (single settled screen, alpha=1, scale=1 → straight into scene canvas) vs. scratch-canvas path (transition-active, opaque-into-scratch then composite). Pause's `drawScene` needs to play well with both.

**Mitigation:** before the rewrite, add a stub `drawScene` to current pause that just paints the dim. Confirm the world becomes visible. Land the prereq fix as its own commit (bisectable). If the compositor gets ornery, that's a small backout point.

### R2 — Profile card needs data that isn't surfaced yet

Login streak and last-login date don't exist clientside. Server doesn't send them.

**Mitigation:** M1 ships stubs (`1 day` / `Today`). Visual slots exist; wiring is one line when the server-side fields arrive. Document with `-- STUB:` comments.

### R3 — Currency icons don't have art yet

Lucide doesn't have direct matches for our 5 currencies; we don't have custom art.

**Mitigation:** Lucide proxies for M1 (table in Implementation Outline). Document the substitutions; swap when real art lands.

### R4 — Screen transition override mechanism uncertain

Sketched `getTransitionValues = function() return 1, 1 end` may not be exactly the API.

**Mitigation:** read `src/ui/core/screen.lua` + `src/ui/core/screenstack.lua` before implementation to confirm the disable mechanism. Fall back to alternatives (flag, transition-duration=0) as needed. ~5 min investigation.

### R5 — Settings tab "active" indicator needs a reliable signal

Polling `UI.stack` is the cleanest, but the introspection API may not be there.

**Mitigation:** the "stash child ref on push, clear in update when self is top of stack" pattern is the fallback. Most resilient: every screen has an update; pause does the check once per frame.

### R6 — Animation timings (220 / 180 ms) are starting points

Numbers are reasoned guesses, not finals.

**Mitigation:** constants live at the top of `animation.lua`. Ship Section 3's values; tune via the visual gate. Two-three iteration cycles probably converges.

### R7 — Frosted-glass sampling order

Rail and tiles use frosted-glass against the scene canvas. The dim layer must composite BEFORE frosted samples (R1 fix handles this by putting the dim inside `drawScene`).

**Mitigation:** verify in the visual gate that frosted glass on the rail / tiles feels connected to the visible background, not floating against an undimmed scene.

### R8 — F-keys race with pause for the same destinations

User remapped F3/F4/T/C to the same screens the tiles open. If pause is up and user presses F3, the dev keybind triggers a second push of Gacha onto the stack on top of the existing tile-driven push.

**Mitigation:** InputDispatch's `_devKeybind` should check "am I in PLAYING state with no UI screens?" before firing. If pause is on the stack, F-keys are no-ops. This matches the current dev-keybind gating already in InputDispatch. Verify in implementation; no spec change needed.

---

## Cross-references

- Reference image: `GachaClient/.screenshots/references/pause.png`
- Currency model: `[[project_currency_model]]`
- Inventory/party rework deferral: `[[project_inventory_party_rework]]`
- Combat abilities boundary: `[[feedback_abilities_system_ownership]]`
- Homogenized rendering mandate: `[[feedback_homogenized_rendering]]`
- App architecture (service registration, dispatch): `[[project_app_architecture]]`

---

## Self-review

- [x] Genshin-style left-rail + grid pattern matches reference image
- [x] Rail M1 contents (X / Settings / Logout) explicit; tab spot for future expansion documented
- [x] Profile card scope (full version: name + UID + streak + last-login + currency wallet) explicit; M1 stubs for unavailable fields flagged
- [x] Tile grid M1 fills only 4 of 8 slots; future room documented
- [x] Two-phase slide animation timings + easing explicit; values are tunable constants
- [x] Open / close state machine with input-gating semantics complete
- [x] Tile click → push (not close-and-push) — preserves screen-stack walk-back UX
- [x] Settings active-state tracking via stash + top-of-stack check
- [x] Prerequisite (move dim into `drawScene`) called out as separate commit
- [x] Currency icon stubs documented with explicit Lucide proxy table
- [x] F-key dev-keybind coexistence noted; gating fix in InputDispatch flagged
- [x] 8 risks enumerated with mitigations
- [x] No protocol changes; no new dependencies; combat ABILITIES boundary respected
- [x] Implementation scoped to ~700 new LoC across 6 files; tractable for ~3 sessions
