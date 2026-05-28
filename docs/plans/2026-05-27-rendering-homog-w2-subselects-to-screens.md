# Homogenization W2 — Subselects → Full Screens Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote `weapon_select` and `character_select` from `Widget`-with-duplicated-transition-machinery to full `Screen`s with `Profile.OVERLAY`, so the screen stack + compositor own their open/close transitions. Eliminates ~50 lines of duplicated FSM per component and fixes the audit's "undersized at non-1× render-scale" bug for free (base `Screen.beginTransitionDraw` is render-scale-aware; the bespoke `_transitionCanvas` at `love.graphics.newCanvas(screenW, screenH)` was not).

**Architecture:** Each subselect today is a `Widget` added as a child of its parent screen, gated on `self.visible`, with the parent explicitly bridging input (mouse/key/wheel) into the widget when visible. After migration each is a `Screen.new`-derived object pushed via `UI.push` from the parent's open action; the screen stack delivers input directly; `escapeCloses = true` + an internal `UI.pop()` from action callbacks (equip/select/etc.) drive close. `renderProfile = Profile.OVERLAY` keeps the parent's ui/hud composing underneath unchanged.

**Tech Stack:** LÖVE 11.x / LuaJIT, `Screen` base (`ui/core/screen.lua`), `ScreenStack` (`ui/core/screenstack.lua`), `Profile.OVERLAY` (`systems/render/Profile.lua`).

**Interface audit (done at plan time, locked):** Both subselects have **zero `self.parent.*` reads** — already a clean two-arrow interface today. **`weapon_select` has TWO consumers** (inventory AND party — the party screen chains character_select → weapon_select via its `onEquipWeapon` callback). Both consumer migrations must land in the same task as the component promotion or the un-migrated parent crashes.

- **Parent → Subselect:** `:show(...)` (becomes constructor args), `:hide()` (becomes `UI.pop()` from internal action handler), `:setInventory(...)`, `:setEquipment(...)`, `:setCharacters(...)`, `:setPartyCharacterIds(...)`, `:setEquipmentData(...)` (all become constructor args — a snapshot at open time, no live mutation while modal is up).
- **Subselect → Parent:** `onClose`, `onEquip`/`onUnequip` (weapon), `onSelect`/`onRemove`/`onEquipWeapon` (character) — all stay as callbacks fired up; the parent passes them in via constructor opts.

Snapshot-at-open is safe because the only state mutation that could happen behind the modal's back is itself driven by the modal (the user equips a weapon → `onEquip` fires → parent calls `InventoryManager:equipWeapon` → network round-trip → eventual `onStateChanged` callback → parent calls `refreshDisplay`; by then the modal has already popped itself).

**Constraints (standing):**
- Working tree heavily dirty — **targeted `git add <file>` only — NEVER `git add -A` or `git add .`.**
- Never skip hooks (`--no-verify`) or bypass signing.
- Combat ABILITIES gameplay off-limits (not relevant to W2).
- Match existing patterns: hand-coded screens opt into the contract via `Screen.new({...})` + `setmetatable` chain to a Lua class; see `ui/screens/dailies/WaveGame.lua` lines 285–309 for the canonical hand-coded scene-producer constructor.

---

## File Structure

**Task 1 modifies (atomic, single commit):**
- `GachaClient/ui/screens/inventory/weapon_select.lua` (~1090 lines → ~1030) — base swap Widget→Screen, delete transition FSM + `:show`/`:hide`/`:getTransitionValues`/`:beginTransitionDraw`/`:endTransitionDraw`, fold setters into constructor opts, rename class.
- `GachaClient/ui/screens/inventory/init.lua` (~368 lines → ~280) — replace add-child + lifecycle with `UI.push(WeaponSelectScreen.new{...})`, delete input bridge, delete close/equip/unequip methods, delete cleanup hide.
- `GachaClient/ui/screens/party/init.lua` (~508 lines → ~440) — same migration as inventory for the party's weapon_select usage. Touches the same file Task 2 also touches (different methods/blocks; no overlap).

**Task 2 modifies (atomic, single commit):**
- `GachaClient/ui/screens/party/components/character_select.lua` (~1051 lines → ~990) — same pattern as Task 1's component refactor.
- `GachaClient/ui/screens/party/init.lua` — replace add-child + lifecycle with `UI.push(CharacterSelectScreen.new{...})`, delete input bridge, delete close/assign/remove methods, delete cleanup hide. (Independent of Task 1's edits on the same file — different methods.)

**No file moves.** `weapon_select.lua` stays at `ui/screens/inventory/weapon_select.lua` and `character_select.lua` stays at `ui/screens/party/components/character_select.lua` — they're already in screen-shaped paths.

**Untouched:**
- Both subselects' draw bodies (list/info-pane/buttons rendering), data filtering, scroll, hover/select logic, confirm-modal handling, layout cache — all stays. Only the transition / lifecycle / base-class plumbing changes.
- `characters_tab.lua`, `inventory_card.lua`, `party_member.lua` — none reference the subselects directly (grep confirmed).

---

## Target shape — the new Screen constructor (canonical)

Pattern both subselects adopt. Shown for `WeaponSelectScreen`; `CharacterSelectScreen` is symmetric.

```lua
-- ui/screens/inventory/weapon_select.lua (top)
local UI      = require "ui"
local Screen  = UI.Screen
local Widget  = UI.Widget          -- only used for Widget.draw recursion in :draw
local Label   = UI.Label
local Modal   = UI.Modal
local theme   = UI.theme
local draw    = require "ui.util.draw"
local Profile = require "systems.render.Profile"

local WeaponService = require "services.WeaponService"

local WeaponSelectScreen = {}
WeaponSelectScreen.__index = WeaponSelectScreen

-- (layout constants HEADER_HEIGHT / LIST_WIDTH_RATIO / ROW_HEIGHT etc. stay
--  exactly as they were in the widget version)

function WeaponSelectScreen.new(opts)
    opts = opts or {}
    local screen = Screen.new({
        escapeCloses = true,
        blocksInput  = true,
        transition   = "fade_grow",
    })
    setmetatable(screen, { __index = setmetatable(WeaponSelectScreen, { __index = Screen }) })

    screen.renderProfile = Profile.OVERLAY
    screen.gradeSettings = nil

    -- ...rest of init (see Task 1 Step 2b for the full body)
    return screen
end

function WeaponSelectScreen:_dismiss()
    if self.onClose then self.onClose() end
    UI.pop()
end

-- ...other methods unchanged in body; only the metatable / base class differ.

return WeaponSelectScreen
```

Key contract points:

| Concern | Widget today | Screen after migration |
|---|---|---|
| Base class | `Widget` via `setmetatable(self, WeaponSelect)` | `Screen` via `Screen.new({transition="fade_grow"})` + class metatable chain |
| Open | parent calls `:show(character)` | parent calls `UI.push(WeaponSelectScreen.new{...})` |
| Close | parent calls `:hide()` or internal `:hide()` | internal `self:_dismiss()` → `onClose()` + `UI.pop()`; ESC via `escapeCloses = true` |
| Render profile | none (widget) | `Profile.OVERLAY` |
| Transition canvas | bespoke `_transitionCanvas` via raw `newCanvas(screenW, screenH)` | base `Screen.beginTransitionDraw` (pool-managed, render-scale-aware) |
| Input | parent bridges via `handleX` checks | `ScreenStack` delivers directly with `blocksInput = true` |
| Width/height | parent sets `self.width = screenW` each frame | not applicable; Screen uses the full window |

---

## Task 1: Promote weapon_select to Screen + migrate BOTH consumers (inventory + party)

**Files (one commit):**
- Modify: `GachaClient/ui/screens/inventory/weapon_select.lua`
- Modify: `GachaClient/ui/screens/inventory/init.lua`
- Modify: `GachaClient/ui/screens/party/init.lua`

### Step 1: Read the current state of weapon_select.lua

Use the Read tool to load `GachaClient/ui/screens/inventory/weapon_select.lua` end-to-end. Note the exact line boundaries of the deletion sites below — line numbers in this plan reference the file as it exists today and may shift after each edit.

Expected blocks to delete (anchor on the unique opening of each):
- Field `self.transition = { state = "none", progress = 0, duration = 0.15, onComplete = nil }` (in `.new(options)`)
- Fields `self._transitionCanvas = nil` and `self._prevCanvas = nil` (in `.new(options)`)
- Method `function WeaponSelect:show(character)` (starts ~line 151)
- Method `function WeaponSelect:hide()` (starts ~line 189)
- Method `function WeaponSelect:getTransitionValues()` (starts ~line 205)
- Method `function WeaponSelect:beginTransitionDraw()` (starts ~line 216)
- Method `function WeaponSelect:endTransitionDraw()` (starts ~line 244)
- Inside `function WeaponSelect:update(dt)`, the transition-advance block — find the unique `local t = self.transition` anchor and delete that block
- The `_transitionCanvas` / `_prevCanvas` use inside `:draw()` (keep the `beginTransitionDraw`/`endTransitionDraw` calls themselves — they'll resolve to the base Screen versions)

### Step 2: Rewrite weapon_select.lua as a Screen subclass

Apply each sub-step as a separate Edit call. Anchor each Edit on a unique substring.

#### 2a. Replace the file header

Find:

```lua
-- ui/screens/inventory/weapon_select.lua
-- Weapon selection overlay with vertical list and info panel
-- Layout matches character_select.lua: scrollable list on left, info pane on right

local UI = require "ui"
local Widget = UI.Widget
local Label = UI.Label
local Modal = UI.Modal
local theme = UI.theme
local draw = require "ui.util.draw"

local WeaponService = require "services.WeaponService"

local WeaponSelect = {}
WeaponSelect.__index = WeaponSelect
setmetatable(WeaponSelect, {__index = Widget})
```

Replace with:

```lua
-- ui/screens/inventory/weapon_select.lua
-- Weapon selection overlay screen. Pushed over the parent (InventoryScreen or PartyScreen)
-- via UI.push; renders in the ui layer (Profile.OVERLAY) so the parent's UI composes
-- underneath. Internal :_dismiss helper pops the screen after firing onClose; ESC pops
-- it via Screen's escapeCloses.

local UI      = require "ui"
local Screen  = UI.Screen
local Widget  = UI.Widget
local Label   = UI.Label
local Modal   = UI.Modal
local theme   = UI.theme
local draw    = require "ui.util.draw"
local Profile = require "systems.render.Profile"

local WeaponService = require "services.WeaponService"

local WeaponSelectScreen = {}
WeaponSelectScreen.__index = WeaponSelectScreen
```

#### 2b. Rewrite the constructor

Find the existing `function WeaponSelect.new(options) ... end` (lines ~32–91). Replace with:

```lua
function WeaponSelectScreen.new(opts)
    opts = opts or {}
    local screen = Screen.new({
        escapeCloses = true,
        blocksInput  = true,
        transition   = "fade_grow",
    })
    setmetatable(screen, { __index = setmetatable(WeaponSelectScreen, { __index = Screen }) })

    screen.renderProfile = Profile.OVERLAY
    screen.gradeSettings = nil

    -- Data snapshot at open time
    screen.weapons          = {}
    screen.character        = opts.character
    screen.equipment        = opts.equipment or {}
    screen.weaponRefinement = opts.weaponRefinement or {}
    screen.readOnly         = opts.readOnly or false

    -- Filter inventory to weapons only (was WeaponSelect:setInventory body)
    for _, item in ipairs(opts.inventory or {}) do
        if item.item_type == ITEM_TYPE.WEAPON then
            table.insert(screen.weapons, item)
        end
    end

    -- Action callbacks
    screen.onClose   = opts.onClose
    screen.onEquip   = opts.onEquip
    screen.onUnequip = opts.onUnequip

    -- Selection / view state
    screen.scrollY        = 0
    screen.maxScrollY     = 0
    screen.hoveredIndex   = nil
    screen.selectedIndex  = nil
    screen.selectedWeapon = nil

    -- Button-hit-test state
    screen.backButtonHovered    = false
    screen.backButtonBounds     = { x = 0, y = 0, w = 0, h = 0 }
    screen.equipButtonHovered   = false
    screen.equipButtonBounds    = { x = 0, y = 0, w = 0, h = 0 }
    screen.unequipButtonHovered = false
    screen.unequipButtonBounds  = { x = 0, y = 0, w = 0, h = 0 }

    -- Layout cache
    screen.listWidth      = 0
    screen.infoPanelX     = 0
    screen.infoPanelWidth = 0

    -- Double-click tracking
    screen.lastClickTime  = 0
    screen.lastClickIndex = 0
    screen.doubleClickThreshold = 0.4

    -- Confirmation modal (created on demand)
    screen.confirmModal = nil

    return screen
end
```

**Important:** if the original constructor performed any post-filter sort or any other initialization that this rewrite missed (e.g., sorting weapons), preserve it by reading the original body in Step 1 and appending those lines into the new body. The rewrite above is the structural shape; do not lose business logic.

#### 2c. Add `_dismiss` helper after the constructor

```lua
function WeaponSelectScreen:_dismiss()
    if self.onClose then self.onClose() end
    UI.pop()
end
```

#### 2d. Delete setter methods

Delete:
- `function WeaponSelect:setInventory(inventory) ... end` (~line 93–116)
- `function WeaponSelect:setEquipment(equipment) ... end` (~line 118–120)
- `function WeaponSelect:setWeaponRefinement(weaponRefinement) ... end` (~line 122–124)

`getRefinement(weaponId)` STAYS — called from within the draw/info pane code.

#### 2e. Delete transition methods

Delete:
- `function WeaponSelect:show(character) ... end`
- `function WeaponSelect:hide() ... end`
- `function WeaponSelect:getTransitionValues() ... end`
- `function WeaponSelect:beginTransitionDraw() ... end`
- `function WeaponSelect:endTransitionDraw() ... end`

#### 2f. Update `:update(dt)`

Find the existing `function WeaponSelect:update(dt)`. Locate and delete the transition-FSM advance block (anchor on `local t = self.transition`). If the function has no remaining body, replace with:

```lua
function WeaponSelectScreen:update(dt)
    Screen.update(self, dt)
end
```

If it has other body (layout updates, hover state), prepend `Screen.update(self, dt)` at the top and keep the rest.

#### 2g. Rename every `WeaponSelect:` method to `WeaponSelectScreen:`

Use Edit with `replace_all = true` on the substring `function WeaponSelect:` → `function WeaponSelectScreen:`.

#### 2h. Replace internal `:hide()` calls with `:_dismiss()`

Find every `self:hide()` call in the file (typically 3–5: equip-success path, unequip path, back-button onClick, confirmation modal confirm path). Replace each with `self:_dismiss()`.

#### 2i. Update the trailing `return`

Find:

```lua
return WeaponSelect
```

Replace with:

```lua
return WeaponSelectScreen
```

#### 2j. Verify internal consistency

```bash
grep -n "WeaponSelect\b" GachaClient/ui/screens/inventory/weapon_select.lua | head
```

Expected: zero matches.

```bash
grep -n "transition\b\|_transitionCanvas\|_prevCanvas\|self:hide()" GachaClient/ui/screens/inventory/weapon_select.lua
```

Expected: zero matches.

### Step 3: Migrate inventory/init.lua callsites

#### 3a. Update the require line

Find:

```lua
local WeaponSelect = require "ui.screens.inventory.weapon_select"
```

Replace with:

```lua
local WeaponSelectScreen = require "ui.screens.inventory.weapon_select"
```

#### 3b. Delete widget-creation block in `:buildUI()` (lines ~106–123)

Find and delete:

```lua
    -- Weapon selection overlay (hidden by default)
    self.weaponSelect = WeaponSelect.new({
        x = 0,
        y = 0,
        width = screenW,
        height = screenH,
        visible = false,
        onClose = function()
            self:closeWeaponSelect()
        end,
        onEquip = function(weapon)
            self:equipWeapon(weapon)
        end,
        onUnequip = function()
            self:unequipWeapon()
        end
    })
    self:addChild(self.weaponSelect)
```

#### 3c. Delete layout updates in `:updateLayout()` (lines ~171–173)

```lua
    -- Weapon select overlay
    self.weaponSelect.width = screenW
    self.weaponSelect.height = screenH
```

#### 3d. Delete data forwarding in `:refreshDisplay()` (lines ~213–214)

```lua
        -- WeaponSelect receives the full inventory too (filters internally by item_type)
        self.weaponSelect:setInventory(data.inventory)
        self.weaponSelect:setEquipment(data.equipment)
```

#### 3e. Replace `:openWeaponSelect`, `:closeWeaponSelect`, `:equipWeapon`, `:unequipWeapon` methods (lines ~218–255)

Find and replace this entire block:

```lua
function InventoryScreen:openWeaponSelect(character)
    self.weaponSelect:show(character)
    self.closeHint.text = "Press ESC to go back"
end

function InventoryScreen:closeWeaponSelect()
    self.weaponSelect:hide()
    self.closeHint.text = "Press ESC to close"
end

function InventoryScreen:equipWeapon(weapon)
    if not self.selectedCharacter then
        self:closeWeaponSelect()
        return
    end

    local charId = self.selectedCharacter.id
    -- Use instance_id for weapons in the new system
    local instanceId = weapon and (weapon.instance_id or weapon.id) or nil

    self.inventoryManager:equipWeapon(charId, instanceId)

    self:closeWeaponSelect()
end

function InventoryScreen:unequipWeapon()
    if not self.selectedCharacter then
        self:closeWeaponSelect()
        return
    end

    local charId = self.selectedCharacter.id

    -- Unequip by passing nil as weapon ID
    self.inventoryManager:equipWeapon(charId, nil)

    self:closeWeaponSelect()
end
```

With:

```lua
function InventoryScreen:openWeaponSelect(character)
    if not character then return end
    local data = self.inventoryManager:getDisplayData()
    self.closeHint.text = "Press ESC to go back"
    UI.push(WeaponSelectScreen.new({
        character = character,
        inventory = data.inventory,
        equipment = data.equipment,
        onClose = function()
            self.closeHint.text = "Press ESC to close"
        end,
        onEquip = function(weapon)
            local instanceId = weapon and (weapon.instance_id or weapon.id) or nil
            self.inventoryManager:equipWeapon(character.id, instanceId)
        end,
        onUnequip = function()
            self.inventoryManager:equipWeapon(character.id, nil)
        end,
    }))
end
```

`character` is captured by closure (the function parameter), eliminating the need for a `selectedCharacter` re-read at action time. The subselect's own `_dismiss` runs `onClose` + `UI.pop`, so the hint reset belongs in `onClose`.

#### 3f. Update `:cleanup()` (lines ~74–77, 81)

Find and delete:

```lua
    -- Hide weapon select if open
    if self.weaponSelect.visible then
        self.weaponSelect:hide()
    end
```

And the line:

```lua
    self.weaponSelect:setInventory({})
```

#### 3g. Delete the input bridge for weaponSelect

In `:handleKeyPressed` (lines ~294–305), delete the block beginning `if self.weaponSelect.visible then` and ending at the matching `end` (~line 305). Keep the rest of `handleKeyPressed` (refresh-with-R, left/right routing to charactersView, fallback to Screen.handleKeyPressed).

In `:handleWheelMoved` (lines ~325–329), delete:

```lua
    -- Route to weapon select if open
    if self.weaponSelect.visible then
        return self.weaponSelect:handleWheelMoved(x, y)
    end
```

In `:handleMousePressed` (lines ~338–342), delete:

```lua
    -- Route to weapon select if open
    if self.weaponSelect.visible then
        return self.weaponSelect:handleMousePressed(x, y, button)
    end
```

In `:handleMouseMoved` (lines ~354–358), delete:

```lua
    -- Route to weapon select if open
    if self.weaponSelect.visible then
        return self.weaponSelect:handleMouseMoved(x, y, dx, dy)
    end
```

#### 3h. Verify inventory/init.lua

```bash
grep -n "weaponSelect\b" GachaClient/ui/screens/inventory/init.lua
```

Expected: zero matches.

```bash
grep -n "WeaponSelect" GachaClient/ui/screens/inventory/init.lua
```

Expected: exactly TWO matches — the require line and the `UI.push(WeaponSelectScreen.new({...}))` call.

### Step 4: Migrate party/init.lua's weapon_select usage

PartyScreen uses both weaponSelect AND characterSelect. This task migrates ONLY the weapon_select pieces; the characterSelect migration happens in Task 2.

#### 4a. Update the require line

Find:

```lua
local WeaponSelect = require "ui.screens.inventory.weapon_select"
```

Replace with:

```lua
local WeaponSelectScreen = require "ui.screens.inventory.weapon_select"
```

#### 4b. Delete the weapon_select widget creation in `:buildUI()` (lines ~137–155)

Find and delete:

```lua
    -- Weapon selection overlay (for equipping from character select)
    self.weaponSelect = WeaponSelect.new({
        x = 0,
        y = 0,
        width = screenW,
        height = screenH,
        visible = false,
        readOnly = self.readOnly,
        onClose = function()
            self:closeWeaponSelect()
        end,
        onEquip = function(weapon)
            self:equipWeapon(weapon)
        end,
        onUnequip = function()
            self:unequipWeapon()
        end
    })
    self:addChild(self.weaponSelect)
```

#### 4c. Delete data forwarding to weaponSelect in `:refreshDisplay()` (lines ~234–235)

```lua
        -- Update weapon select with weapons array
        self.weaponSelect:setInventory(data.weapons or {})
        self.weaponSelect:setEquipment(data.equipment or {})
```

#### 4d. Delete layout updates in `:updateLayout()` (lines ~358–360)

```lua
    -- Update weapon select size
    self.weaponSelect.width = screenW
    self.weaponSelect.height = screenH
```

#### 4e. Replace `:openWeaponSelect`, `:closeWeaponSelect`, `:equipWeapon`, `:unequipWeapon` methods (lines ~292–333)

Find and replace this block:

```lua
function PartyScreen:openWeaponSelect(character)
    self.selectedCharacterForWeapon = character

    -- Ensure weapon select has latest data before showing
    local data = self.inventoryManager:getDisplayData()
    if data and not data.isLoading and not data.error then
        self.weaponSelect:setInventory(data.weapons or {})
        self.weaponSelect:setEquipment(data.equipment or {})
    end

    self.weaponSelect:show(character)
end

function PartyScreen:closeWeaponSelect()
    self.weaponSelect:hide()
    self.selectedCharacterForWeapon = nil
end

function PartyScreen:equipWeapon(weapon)
    if not self.selectedCharacterForWeapon then
        self:closeWeaponSelect()
        return
    end

    local charId = self.selectedCharacterForWeapon.id
    -- Weapons are keyed by instance_id in the new system
    local instanceId = weapon and (weapon.instance_id or weapon.id) or nil

    self.inventoryManager:equipWeapon(charId, instanceId)
    self:closeWeaponSelect()
end

function PartyScreen:unequipWeapon()
    if not self.selectedCharacterForWeapon then
        self:closeWeaponSelect()
        return
    end

    local charId = self.selectedCharacterForWeapon.id
    self.inventoryManager:equipWeapon(charId, nil)
    self:closeWeaponSelect()
end
```

With:

```lua
function PartyScreen:openWeaponSelect(character)
    if not character then return end
    local data = self.inventoryManager:getDisplayData()
    if data.isLoading or data.error then return end
    UI.push(WeaponSelectScreen.new({
        character = character,
        inventory = data.weapons or {},
        equipment = data.equipment or {},
        readOnly  = self.readOnly,
        onEquip = function(weapon)
            local instanceId = weapon and (weapon.instance_id or weapon.id) or nil
            self.inventoryManager:equipWeapon(character.id, instanceId)
        end,
        onUnequip = function()
            self.inventoryManager:equipWeapon(character.id, nil)
        end,
    }))
end
```

`character` is captured by closure; the old `selectedCharacterForWeapon` field is no longer needed. `closeWeaponSelect`, `equipWeapon`, `unequipWeapon` all delete (their logic is now inline in the constructor opts; `:_dismiss()` inside the modal handles pop).

#### 4f. Delete `selectedCharacterForWeapon` field initialization (if any)

Search the file:

```bash
grep -n "selectedCharacterForWeapon" GachaClient/ui/screens/party/init.lua
```

Delete every reference (likely just the assignment in the now-deleted `:openWeaponSelect` — already gone via 4e — and possibly an initialization in the constructor).

#### 4g. Delete the input bridge for weaponSelect in party

Search for and delete every `if self.weaponSelect.visible then ... self.weaponSelect:handleX(...) ... end` block in `:handleKeyPressed`, `:handleWheelMoved`, `:handleMousePressed`, `:handleMouseMoved`. (Symmetric to inventory Step 3g.) Use grep first:

```bash
grep -n "weaponSelect" GachaClient/ui/screens/party/init.lua
```

After Step 4b, 4c, 4d, 4e, 4f, the only remaining matches should be inside the input handlers. Delete those blocks.

#### 4h. Verify

```bash
grep -n "weaponSelect\b" GachaClient/ui/screens/party/init.lua
```

Expected: zero matches.

```bash
grep -n "WeaponSelect" GachaClient/ui/screens/party/init.lua
```

Expected: exactly TWO matches — the require line and the `UI.push(WeaponSelectScreen.new({...}))` call.

### Step 5: Headless syntax check

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness
```

Expected: `ALL ASSET HARNESS CHECKS PASSED`. The harness syntax-checks the files in its `touched` list. If `ui/screens/inventory/weapon_select.lua` is not in that list (read `GachaClient/tests/assets_harness/main.lua` line 17–27), add it (and `ui/screens/inventory/init.lua` if not present, and `ui/screens/party/init.lua` if not present). Any harness edit belongs in the same Task 1 commit.

If the harness reports any FAIL after the edits, the deletion left at least one file in a bad state. Surface as BLOCKED with the exact failure.

### Step 6: Visual gate — USER GATED

Subagent dispatched for Task 1 STOPS at this step. Main session pauses for user. User runs the game:

1. **F3 → Inventory** → click a character card → click "Equip Weapon" → `WeaponSelectScreen` fades in (the `fade_grow` transition). Click a weapon → fades out, character's equipped weapon updates. `closeHint` at the bottom of inventory reads "Press ESC to go back" while modal is up, reverts to "Press ESC to close" after.
2. **F3 → Inventory** → equip → **ESC** → modal fades out, focus returns to inventory cleanly.
3. **F2 → Party** → click an empty slot → character_select widget appears (still old widget code; this task doesn't touch it). Click a character → party slot filled. (Verify the character_select widget is still functional — its codepath is untouched in Task 1.)
4. **F2 → Party** → click a filled slot → character_select widget shows current selection → click the weapon slot of a character → `WeaponSelectScreen` (the new Screen) opens ON TOP of the character_select widget. Pick a weapon → modal pops; character_select widget remains visible underneath. (This validates the cross-modal chain — character_select widget over party + WeaponSelectScreen modal on top.)
5. **Settings → Render Scale → set to 0.75** (or similar non-1×) → repeat steps 1–2. The modal should render at the correct on-screen size (audit bug fix).
6. Resize the window while the modal is open: no canvas mismatch artifacts.

If anything regresses, hold the commit. Most likely failure modes:
- Missed `:hide()` → `:_dismiss()` swap (modal opens but never closes via action)
- `transition = "fade_grow"` preset doesn't exist in `ui/core/transition.lua` — check available preset names there; if it differs, use the correct one
- Input bridge deleted too aggressively (non-modal inventory/party input stops working)
- `selectedCharacterForWeapon` references missed in party (would crash on equip)

### Step 7: Commit (after user confirms)

```bash
git add GachaClient/ui/screens/inventory/weapon_select.lua GachaClient/ui/screens/inventory/init.lua GachaClient/ui/screens/party/init.lua
git commit -m "feat(render): weapon_select widget -> overlay Screen (homog W2 task 1)"
```

If the assets_harness was edited (Step 5), include it in the same commit:

```bash
git add GachaClient/ui/screens/inventory/weapon_select.lua GachaClient/ui/screens/inventory/init.lua GachaClient/ui/screens/party/init.lua GachaClient/tests/assets_harness/main.lua
git commit -m "feat(render): weapon_select widget -> overlay Screen (homog W2 task 1)"
```

Verify: `git log -1 --stat` shows 3 (or 4 with harness) files modified.

---

## Task 2: Promote character_select to a Screen + migrate party parent

**Files (one commit):**
- Modify: `GachaClient/ui/screens/party/components/character_select.lua`
- Modify: `GachaClient/ui/screens/party/init.lua`

This task is the symmetric mirror of Task 1's component refactor, but only the party callsite migrates (inventory does not use character_select).

### Step 1: Read the current state of character_select.lua

Same as Task 1 Step 1. Identify the transition / show / hide / get/begin/endTransitionDraw blocks and the four setter methods (`setCharacters`, `setPartyCharacterIds`, `setEquipmentData`, `setCharacterEcho`).

### Step 2: Rewrite character_select.lua as a Screen subclass

#### 2a. File header replacement

Find:

```lua
-- ui/screens/party/components/character_select.lua
-- Overlay for selecting a character to add to party
-- Layout: Scrollable list on left, info pane on right

local UI = require "ui"
local Widget = UI.Widget
local Label = UI.Label
local Button = UI.Button
local theme = UI.theme
local draw = require "ui.util.draw"

local CharacterSelect = {}
CharacterSelect.__index = CharacterSelect
setmetatable(CharacterSelect, {__index = Widget})
```

Replace with:

```lua
-- ui/screens/party/components/character_select.lua
-- Character selection overlay screen. Pushed over PartyScreen via UI.push; renders in the
-- ui layer (Profile.OVERLAY). Internal :_dismiss helper pops the screen after firing onClose;
-- ESC pops it via Screen's escapeCloses.

local UI      = require "ui"
local Screen  = UI.Screen
local Widget  = UI.Widget
local Label   = UI.Label
local Button  = UI.Button
local theme   = UI.theme
local draw    = require "ui.util.draw"
local Profile = require "systems.render.Profile"

local CharacterSelectScreen = {}
CharacterSelectScreen.__index = CharacterSelectScreen
```

#### 2b. Rewrite the constructor

Find existing `function CharacterSelect.new(options) ... end` (lines ~29–91). Replace with:

```lua
function CharacterSelectScreen.new(opts)
    opts = opts or {}
    local screen = Screen.new({
        escapeCloses = true,
        blocksInput  = true,
        transition   = "fade_grow",
    })
    setmetatable(screen, { __index = setmetatable(CharacterSelectScreen, { __index = Screen }) })

    screen.renderProfile = Profile.OVERLAY
    screen.gradeSettings = nil

    -- Data snapshot at open time
    screen.characters              = {}
    screen.partyCharacterIds       = {}    -- ID-set lookup
    screen.equipmentData           = opts.equipmentData or {}
    screen.characterEcho           = opts.characterEcho or {}
    screen.targetSlot              = opts.targetSlot or 0
    screen.currentSlotHasCharacter = opts.currentSlotHasCharacter or false
    screen.currentSlotCharacterId  = opts.currentSlotCharacterId
    screen.readOnly                = opts.readOnly or false

    -- Filter to characters only (was setCharacters body)
    for _, char in ipairs(opts.characters or {}) do
        if char.item_type == ITEM_TYPE.CHARACTER then
            table.insert(screen.characters, char)
        end
    end
    -- (preserve whatever rarity/name sort the widget version performed — read original
    --  setCharacters body in Step 1 and inline it here)

    -- partyCharacterIds: convert list to ID-set (was setPartyCharacterIds body)
    for _, id in ipairs(opts.partyCharacterIds or {}) do
        screen.partyCharacterIds[id] = true
    end

    -- Action callbacks
    screen.onClose       = opts.onClose
    screen.onSelect      = opts.onSelect
    screen.onRemove      = opts.onRemove
    screen.onEquipWeapon = opts.onEquipWeapon

    -- Selection / view state
    screen.scrollY            = 0
    screen.maxScrollY         = 0
    screen.hoveredIndex       = nil
    screen.selectedIndex      = nil
    screen.selectedCharacter  = nil

    -- Button-hit-test state
    screen.backButtonHovered   = false
    screen.backButtonBounds    = { x = 0, y = 0, w = 0, h = 0 }
    screen.selectButtonHovered = false
    screen.selectButtonBounds  = { x = 0, y = 0, w = 0, h = 0 }

    -- Layout cache
    screen.listWidth      = 0
    screen.infoPanelX     = 0
    screen.infoPanelWidth = 0

    -- Double-click tracking
    screen.lastClickTime  = 0
    screen.lastClickIndex = 0
    screen.doubleClickThreshold = 0.4

    -- Weapon slot interaction
    screen.weaponSlotHovered = false
    screen.weaponSlotBounds  = { x = 0, y = 0, w = 0, h = 0 }

    return screen
end
```

#### 2c. Add `_dismiss` helper

```lua
function CharacterSelectScreen:_dismiss()
    if self.onClose then self.onClose() end
    UI.pop()
end
```

#### 2d. Delete setter methods

Delete:
- `function CharacterSelect:setCharacters(characters)` (~line 93–112)
- `function CharacterSelect:setPartyCharacterIds(ids)` (~line 114–119)
- `function CharacterSelect:setEquipmentData(equipment)` (~line 121–123)
- `function CharacterSelect:setCharacterEcho(echoData)` (~line 125–127)

#### 2e. Delete transition methods

Delete:
- `function CharacterSelect:show(slotIndex, hasCharacter, currentCharacterId)` (~line 129)
- `function CharacterSelect:hide()` (~line 168)
- `function CharacterSelect:getTransitionValues()` (~line 184)
- `function CharacterSelect:beginTransitionDraw()` (~line 195)
- `function CharacterSelect:endTransitionDraw()` (~line 223)

#### 2f. Update `:update(dt)`

Same pattern as Task 1 Step 2f. Locate the transition-FSM advance block (anchor on `local t = self.transition`) and delete it. Prepend `Screen.update(self, dt)` if not already chained.

#### 2g. Rename every `CharacterSelect:` method to `CharacterSelectScreen:`

Edit with `replace_all = true` on `function CharacterSelect:` → `function CharacterSelectScreen:`.

#### 2h. Replace internal `:hide()` calls with `:_dismiss()`

Find every `self:hide()` (typically 3–5 sites: select-success path, remove path, back-button onClick, etc.). Replace each with `self:_dismiss()`.

#### 2i. Update the trailing `return`

Find:

```lua
return CharacterSelect
```

Replace with:

```lua
return CharacterSelectScreen
```

#### 2j. Verify

```bash
grep -n "CharacterSelect\b" GachaClient/ui/screens/party/components/character_select.lua | head
```

Expected: zero matches.

```bash
grep -n "transition\b\|_transitionCanvas\|_prevCanvas\|self:hide()" GachaClient/ui/screens/party/components/character_select.lua
```

Expected: zero matches.

### Step 3: Migrate party/init.lua's characterSelect callsite

After Task 1, party/init.lua no longer references weaponSelect at all. This step migrates the remaining characterSelect references.

#### 3a. Update the require line

Find:

```lua
local CharacterSelect = require "ui.screens.party.components.character_select"
```

Replace with:

```lua
local CharacterSelectScreen = require "ui.screens.party.components.character_select"
```

#### 3b. Delete the widget-creation block in `:buildUI()` (lines ~114–135)

Find and delete:

```lua
    -- Character selection overlay
    self.characterSelect = CharacterSelect.new({
        x = 0,
        y = 0,
        width = screenW,
        height = screenH,
        visible = false,
        readOnly = self.readOnly,
        onSelect = function(slotIndex, character)
            self:assignToSlot(slotIndex, character)
        end,
        onClose = function()
            self:closeCharacterSelect()
        end,
        onRemove = function(slotIndex)
            self:removeFromSlot(slotIndex)
        end,
        onEquipWeapon = function(character)
            self:openWeaponSelect(character)
        end
    })
    self:addChild(self.characterSelect)
```

#### 3c. Delete data forwarding to characterSelect in `:refreshDisplay()` (lines ~222, 231)

Delete these lines:

```lua
        self.characterSelect:setCharacters(data.characters)
```

and

```lua
        self.characterSelect:setEquipmentData(equipmentLookup)
```

The `equipmentLookup` local variable is no longer used after this delete; remove the `local equipmentLookup = {}` block (lines ~225–230) too if it's now dead.

#### 3d. Delete layout updates in `:updateLayout()` (lines ~354–356)

```lua
    -- Update character select size
    self.characterSelect.width = screenW
    self.characterSelect.height = screenH
```

#### 3e. Replace `:onSlotClick`, `:assignToSlot`, `:removeFromSlot`, `:closeCharacterSelect` (lines ~258–290)

Find and replace this block:

```lua
function PartyScreen:onSlotClick(slotIndex)
    -- Block slot clicks in read-only mode
    if self.readOnly then
        return
    end

    -- Open character selection for this slot
    local charId = self.inventoryManager:getPartySlot(slotIndex)
    local hasCharacter = charId ~= nil
    self.characterSelect:setPartyCharacterIds(self.inventoryManager:getPartyCharacterIds())
    self.characterSelect:show(slotIndex, hasCharacter, charId)
end

function PartyScreen:assignToSlot(slotIndex, character)
    if not character then
        self:closeCharacterSelect()
        return
    end

    -- Delegate to InventoryManager (handles swap logic internally)
    self.inventoryManager:setPartySlot(slotIndex, character.id)

    self:closeCharacterSelect()
end

function PartyScreen:removeFromSlot(slotIndex)
    self.inventoryManager:clearPartySlot(slotIndex)
    self:closeCharacterSelect()
end

function PartyScreen:closeCharacterSelect()
    self.characterSelect:hide()
end
```

With:

```lua
function PartyScreen:onSlotClick(slotIndex)
    if self.readOnly then return end

    local data = self.inventoryManager:getDisplayData()
    if data.isLoading or data.error then return end

    -- Build equipment lookup (charId -> OwnedWeapon) — was inlined in refreshDisplay before
    local equipmentLookup = {}
    for charId, instanceId in pairs(data.equipment or {}) do
        if instanceId then
            equipmentLookup[charId] = self.inventoryManager:getWeaponByInstanceId(instanceId)
        end
    end

    local charId = self.inventoryManager:getPartySlot(slotIndex)
    local hasCharacter = charId ~= nil

    UI.push(CharacterSelectScreen.new({
        characters              = data.characters,
        equipmentData           = equipmentLookup,
        partyCharacterIds       = self.inventoryManager:getPartyCharacterIds(),
        targetSlot              = slotIndex,
        currentSlotHasCharacter = hasCharacter,
        currentSlotCharacterId  = charId,
        readOnly                = self.readOnly,
        onSelect = function(_slotIdx, character)
            if not character then return end
            self.inventoryManager:setPartySlot(slotIndex, character.id)
        end,
        onRemove = function(_slotIdx)
            self.inventoryManager:clearPartySlot(slotIndex)
        end,
        onEquipWeapon = function(character)
            self:openWeaponSelect(character)
        end,
    }))
end
```

`slotIndex` is captured by closure; the callbacks ignore the slotIdx parameter the original passed (which was always the same value the screen was opened for).

`closeCharacterSelect`, `assignToSlot`, `removeFromSlot` all delete (their logic is now inline in the constructor opts; `:_dismiss()` inside the modal handles pop).

#### 3f. Delete cleanup's characterSelect references

Search the file:

```bash
grep -n "characterSelect" GachaClient/ui/screens/party/init.lua
```

After Steps 3a–3e, the remaining matches should all be in `:draw`, `:cleanup`, or input handlers. Delete:

- In `:cleanup` (around lines ~66–67, 77 — exact line varies):
  ```lua
      if self.characterSelect.visible then
          self.characterSelect:hide()
      end
      ...
      self.characterSelect:setCharacters({})
  ```

#### 3g. Delete the conditional draw + input bridge for characterSelect

In `:draw` (around lines ~404–410), delete the entire `if not self.characterSelect.visible then ... end` block AND the immediately-following `if self.characterSelect.visible then self.characterSelect:draw() end` block. The hint label should now always draw (it was previously hidden when the modal was up — with the modal now being a separate screen on top of party, the hint is hidden by the modal's panel naturally).

Actually, re-check: the original logic was conditionally hiding the hint when the modal was up. Now that the modal is on top of party, the hint underneath might still show through the modal's transparent areas. If the hint shows through the modal's backdrop in an ugly way, add a screen.onResume / screen.onExit hook on the party to toggle the hint visibility. But first try the simple delete and see if the modal's backdrop fully covers it visually (the modal's background panel + dim layer likely already obscures the hint at the bottom of the screen).

In `:handleKeyPressed`, `:handleWheelMoved`, `:handleMousePressed`, `:handleMouseMoved`, delete every `if self.characterSelect.visible then ... self.characterSelect:handleX(...) ... end` block. (Symmetric to inventory Step 3g; party has the same pattern around lines ~440–447, 462–463, 475–476, 496–497.)

#### 3h. Verify

```bash
grep -n "characterSelect\b" GachaClient/ui/screens/party/init.lua
```

Expected: zero matches.

```bash
grep -n "CharacterSelect" GachaClient/ui/screens/party/init.lua
```

Expected: exactly TWO matches — the require line and the `UI.push(CharacterSelectScreen.new({...}))` call.

### Step 4: Headless syntax check

```bash
ThirdParty/love2d/lovec.exe GachaClient/tests/assets_harness
```

Expected: `ALL ASSET HARNESS CHECKS PASSED`. Add character_select.lua to the harness's `touched` list if not already present.

### Step 5: Visual gate — USER GATED

Same shape as Task 1. User runs:

1. **F2 → Party** → click an empty slot → `CharacterSelectScreen` fades in. Click a character → fades out, slot fills.
2. **F2 → Party** → click a filled slot → `CharacterSelectScreen` shows the slot's current character → click "Remove" or pick another → pops, slot updates.
3. **F2 → Party** → click slot → `CharacterSelectScreen` → click a character's weapon slot → `WeaponSelectScreen` opens ON TOP. Pick a weapon → `WeaponSelectScreen` pops; `CharacterSelectScreen` remains visible. (This validates the two-modal-stack flow with both as Screens.)
4. **ESC** at any point pops the topmost modal cleanly.
5. **Settings → Render Scale → set to 0.75** → repeat → modal sized correctly.
6. Resize while a modal is open: no canvas mismatch.

If anything regresses, hold the commit. Most likely failure modes overlap with Task 1's, plus:
- The two-modal stack interaction (CharacterSelectScreen → WeaponSelectScreen) — both `Profile.OVERLAY`; verify ScreenStack handles N-deep cleanly
- party's hint label showing through the modal — if visually ugly, hide it via the modal's onClose/onOpen lifecycle (would need a small follow-up edit)

### Step 6: Commit (after user confirms)

```bash
git add GachaClient/ui/screens/party/components/character_select.lua GachaClient/ui/screens/party/init.lua
git commit -m "feat(render): character_select widget -> overlay Screen (homog W2 task 2)"
```

If assets_harness was edited:

```bash
git add GachaClient/ui/screens/party/components/character_select.lua GachaClient/ui/screens/party/init.lua GachaClient/tests/assets_harness/main.lua
git commit -m "feat(render): character_select widget -> overlay Screen (homog W2 task 2)"
```

---

## Verification — overall

| Check | Where | Gate |
|---|---|---|
| weapon_select.lua + both consumers compile | assets_harness loadfile | Task 1 Step 5 |
| InventoryScreen weapon-equip flow works | F3 inventory smoke test | Task 1 Step 6 |
| PartyScreen weapon-equip flow works (with old character_select widget still in play) | F2 party → slot → weapon-slot smoke test | Task 1 Step 6 |
| WeaponSelectScreen renders correctly at render-scale ≠ 1 | F3 or F2 at render-scale 0.75 | Task 1 Step 6 (the audit bug fix) |
| character_select.lua + party consumer compile | assets_harness loadfile | Task 2 Step 4 |
| PartyScreen full flow (character_select Screen → weapon_select Screen chain) | F2 party at render-scale 0.75 | Task 2 Step 5 |
| No `transition`/`_transitionCanvas` residue in either component | grep both component files | Tasks 1 & 2 Step 2j |
| No `weaponSelect`/`characterSelect` field references in parent screens after their respective tasks | grep both parents | Tasks 1 & 2 final verification |

## Risks (carried from spec, with W2-specific mitigations)

| Risk | Mitigation |
|---|---|
| Snapshot-at-open misses a state update during modal session | Modal-driven mutations route through callbacks → `InventoryManager` → `Network.setParty` → eventual `onStateChanged` → parent re-renders AFTER modal pops. No mid-session live data is shown by the modal. |
| `transition = "fade_grow"` preset name doesn't exist | Verify by reading `ui/core/transition.lua` for valid preset names at Step 2b. The pre-migration widget uses `Transition` module already — pick the same name (likely `"fade_grow"` per the widget's existing comment). |
| ScreenStack pop ordering on parent exit | Read `ui/core/screenstack.lua`. If parent-pop doesn't auto-unwind child modals, surface as NEEDS_CONTEXT before deleting the parent's safety `if self.X.visible then self.X:hide() end` block in cleanup. |
| Cross-modal chain (character_select → weapon_select) | Both `Profile.OVERLAY`; ScreenStack is N-deep. Task 1 validates the chain with character_select still as a widget; Task 2 validates the chain with both as Screens. |
| Input bridge over-deletion | Each parent's `:handleX` had subselect-gating + their own routing (left/right for char nav, R for refresh, etc.). Step 3g (Task 1) and Step 3g (Task 2) list exactly which blocks to delete — confirm by re-reading the function bodies during execution and KEEP non-subselect routing. |
| `selectedCharacterForWeapon` field missed | The party screen's `selectedCharacterForWeapon` field is replaced by closure capture in the new `:openWeaponSelect`. Step 4f greps for stragglers. |
| Hint label showing through modal | The party's `hintLabel` was conditionally hidden when `characterSelect.visible`. After Task 2, the hint may show through the modal's backdrop. Visual gate (Task 2 Step 5) catches this; small follow-up to hide via screen.onPause or similar if needed. |
| Working-tree contamination | Each commit's `git add` enumerates the file paths explicitly. No `-A`, no `.`. |

## Out of scope for W2

- **Event bus** ([project_ui_event_bus.md](https://example.invalid) memory). The callbacks here are deliberately 1-line-rewritable to `Bus.emit(...)` calls once a bus design lands.
- **Reorganizing characters_tab.lua or party_member.lua.** They consume the same parent-level event flow; no change needed.
- **Confirmation modal architecture** inside weapon_select (the "reassign confirmation"). It's its own widget pattern; if it works as a widget today, it works as a widget inside a Screen tomorrow. Don't restructure.
- **Render-scale ≠ 1 fix for other bespoke canvases** elsewhere (FrostedGlass, FrameAA, RenderScale, etc.) — that's the separate "Canvas-pool inconsistency" workstream.
