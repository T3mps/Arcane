# Phase 5 — IO Worker Thread Design

**Date:** 2026-05-28
**Status:** Brainstormed → spec'd. Plan to follow.
**Spec'd in master:** `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` § Phase 5.
**Audit reference:** `docs/audits/2026-05-28-aaa-gap-audit-perf-batching.md` (G-CAP-2).
**Pre-research:** `docs/research/2026-05-28-love2d-threading-spike.md` (the threading-feasibility spike that locked the hybrid design).

---

## Goal

Move the two main-thread-blocking I/O workloads off the render path:

1. **Network I/O** — `network_tcp.lua` runs `socket.tcp()`/`:connect`/`:send`/`:receive` on the main thread. The `RECV_TIMEOUT = 0` mitigation removed per-tick blocking but the work itself still couples into the frame loop.
2. **Disk reads + ImageData decode** — `Assets.image(key)` does file read + PNG decode + GL upload synchronously on first request. Hot-reload + atlas bake compound this.

After Phase 5, the main thread does only: GL submission, game logic, channel drain. Networking lives on a dedicated long-lived worker thread; disk decode flows through a small job pool whose first consumer is `decode_image` (and whose seam is sized to absorb future bursty workloads like world-gen, pathfinding, save-file decode).

---

## Architecture

Hybrid two-primitive design, locked by the spike. **Sockets and bursty jobs have different shapes; one abstraction would be the wrong shape for both.**

- **Network worker** = dedicated long-lived thread. Owns the auth/account/combat sockets. Uses BLOCKING `socket:select`/`socket:receive` inside the worker (correct there — can't stall rendering anymore). Plain-table messages cross a pair of channels: main pushes outbound requests, worker pushes inbound messages. Lifecycle = full game lifetime; sentinel-message graceful shutdown on quit.
- **Job pool** = small worker pool (size 2 initially, configurable). Generic dispatch: `Jobs.submit(kind, data, onDone)` → returns id. Workers register named handlers from `services/jobs/handlers/<kind>.lua` at boot and dispatch by name. First handler: `decode_image`. The pool absorbs anything else later via "drop a handler file."

The two primitives don't share infrastructure beyond the same Channel API. They live in `services/io/` and `services/jobs/` respectively. Each is independently shippable; cutover order is `prelude → P5a → P5b`, but P5a and P5b can land in either order once the prelude is done.

---

## Tech stack

- **`love.thread`** + **`love.thread.Channel`** — LÖVE 11.x native threading. Channels carry primitives + LÖVE userdata (by reference) + flat-ish tables. Functions/closures and foreign userdata (LuaSocket, file handles) CANNOT cross.
- **LuaSocket** — already in use via `network_tcp.lua`. Moves WHOLLY into the worker (sockets cannot cross channels).
- **`love.image.newImageData`** — worker-safe (no GL). Builds an `ImageData` userdata that crosses back by reference.
- **`love.graphics.newImage(imageData)`** — main-thread, called by `Jobs.update(dt)` in the result callback to do the GL upload.
- **CP-3 `--profile-capture`** — the validation gate. `update.network` and `update.assets` scopes drop to ~0 ms after Phase 5.

---

## Levers (sequenced)

### P5-prelude — Split `Assets.lua` into worker-safe + main-only halves

**Files:**
- Refactor: `GachaClient/services/Assets.lua` — move every `love.graphics.*` call out of the module's public surface
- Create: `GachaClient/services/assets/gpu.lua` — main-thread GL operations (currently inline in Assets.lua)
- Keep: `GachaClient/services/assets/{cache,manifest,image,font,data,sound,shader}.lua` as-is (already worker-safe except where they take an `Image` for filter setup)

**The problem:** today `Assets.dummyImage()` calls `love.graphics.newImage` inside the module. Any worker that `require`s `services.Assets` will crash when it hits this — workers don't have `love.graphics`. The disk-decode handler needs to use `Manifest.resolve` + `loaders.image.load`, which means it needs Assets — but it can't take all of Assets along.

**The fix:** the `loaders.*` modules already do `love.graphics.newImage` themselves (the existing `image.lua` loader returns a fully-uploaded Image). Split that: the **worker** loads file → `ImageData` only; the **main thread** uploads `ImageData` → `Image`. Same for fonts and sounds.

Specifically:
- `loaders/image.lua` gets a new `load_data(path, opts)` that returns `(ImageData, bytes, err)` — pure load, no GL.
- The existing `load(path, opts)` becomes `load_data(...)` + `love.graphics.newImage(...)` on top, for backwards-compatible main-thread synchronous calls.
- `Assets.dummyImage` moves to a new file `services/assets/gpu.lua` that ONLY workers don't require. Or stays in Assets.lua but gated behind `if love.graphics then` so workers can still load the module.

**Validation:** writing a smoke `require "services.Assets"` from a worker context (a tiny test harness in `tests/threading_harness/`) should succeed without crashing on missing `love.graphics`. Two-line test, headless-ish.

**Estimated cost:** 1-2 days.

### P5a — Network worker thread

**Files:**
- Create: `GachaClient/services/io/network_worker.lua` — the worker's entry point. Pulls outbound requests off `Channel<"net_out">`, pushes inbound messages onto `Channel<"net_in">`. Owns the LuaSocket TCP connections internally.
- Create: `GachaClient/services/io/IOClient.lua` — main-thread shim. Same callback API the existing `Network` module exposes; internally pushes outbound to `net_out` and drains inbound from `net_in` in `Network.update(dt)`.
- Refactor: `GachaClient/network.lua` — becomes a thin facade. Whatever publicly-exposed API exists stays unchanged; internals route through `IOClient`.
- Refactor: `GachaClient/network_tcp.lua` — most of this moves INTO `network_worker.lua` since sockets can't cross channels. The module shrinks dramatically or is deleted in favor of the worker file.

**Wire format unchanged.** `LENGTH:TYPE|TOKEN|PAYLOAD\n` per the existing protocol. Worker parses + assembles; only plain tables cross the channel.

**Reconnect logic** stays in the worker (sockets can't bounce across channels safely). Main-thread `IOClient` exposes the same `reconnecting`/`reconnected`/`reconnectFailed`/`reconnectNeedsAuth`/`sessionExpired` event hooks the current `Network` does — those become channel-message kinds the main thread dispatches on receipt.

**Request correlation:** every outbound request gets a `reqId` (numeric, monotonic, generated by the main-thread shim). Worker echoes `reqId` back in the response so callbacks fire correctly. Existing callback-keyed-by-reqId machinery in `network.lua` carries forward.

**Cancellation / supersession:** main can push `{kind="cancel", reqId=N}` to abort a pending request. The worker honors it pre-send; mid-flight requests complete naturally.

**Lifecycle:** worker starts in `love.load`; main pushes `{kind="quit"}` in `love.quit` and `thread:wait()`s. Worker errors land in `thread:getError()` and also get pushed to a third `net_err` channel for live diagnosis.

**Validation:** CP-3 capture before/after on the login → click-to-play → game-world boot sequence. `update.network` profiler scope drops from current ~0.5-1.5 ms (varies with packet activity) to ~0 ms. The `update.network` scope becomes purely "drain the inbound channel + dispatch callbacks" — no socket work.

**Estimated cost:** 4-5 days. The blocking-select-vs-`pcall` interaction on Windows is the main thing to test carefully; everything else is mechanical message-passing.

### P5b — Job pool + first consumer (`decode_image`)

**Files:**
- Create: `GachaClient/services/jobs/JobSystem.lua` — main-thread submit/dispatch/poll API. Owns the worker pool, the job channel, the result channel, the in-flight map, the callback fire loop.
- Create: `GachaClient/services/jobs/worker.lua` — worker entry point. At boot, scans `services/jobs/handlers/` and `require`s each handler into a name → fn map. Loops: `demand()` job → `pcall` the handler → `push` result. Honors `_quit` sentinel.
- Create: `GachaClient/services/jobs/handlers/decode_image.lua` — the first handler. Calls `loaders.image.load_data(path)` (from P5-prelude), returns `{imageData, bytes}`.
- Modify: `GachaClient/services/Assets.lua` — `Assets.preload` becomes async: submits one `decode_image` job per item, GL-uploads in the callback. `Assets.image(key)` stays synchronous for first-time-on-main calls (the cold path); subsequent calls hit the cache.

**API:**

```lua
-- Main thread.
local Jobs = require "services.jobs.JobSystem"

-- One-shot submit. data must be Channel-compatible.
local id = Jobs.submit("decode_image", { path = "data/image/foo.png" }, function(ok, result)
    if not ok then log.warn("decode_image failed: %s", result); return end
    -- result.imageData is an ImageData shared by reference. Upload on main.
    local img = love.graphics.newImage(result.imageData)
    -- ... store in cache, fire app-level callback, etc.
end)

-- Optional supersession: later submits with the same key cancel earlier in-flight ones.
Jobs.submit("decode_image", { path = "..." }, onDone, { supersedeKey = "atlas_ui" })

-- Cancel pre-execution. Mid-flight jobs complete naturally.
Jobs.cancel(id)

-- Main loop drains result channel + fires callbacks. Called from love.update.
Jobs.update(dt)
```

**Handler convention:**

```lua
-- services/jobs/handlers/decode_image.lua
return function(data)
    local loader = require "services.assets.image"
    local imageData, bytes, err = loader.load_data(data.path)
    if err then error(err) end
    return { imageData = imageData, bytes = bytes }
end
```

Handlers are **pure functions** of their `data` arg. No global state, no closures. The worker's `pcall` catches errors → ` { id, ok = false, value = errMessage } ` on the result channel.

**Pool sizing:** 2 workers initially. Each carries `love.image`/`love.filesystem`/`love.data` + the handler files (~2-3 MB Lua memory per worker). The pool size is a constant; dial it up later if the empirical job-queue depth justifies it.

**Backpressure:** `Jobs.submit` uses `push` (non-blocking). For load-storm scenarios (e.g. boot atlas-bake of 200 images), a `Jobs.submitSync(kind, data)` variant uses `supply()` so the caller blocks until a worker accepts. Default = async.

**Cancellation / supersession:** `supersedeKey` cancels earlier in-flight jobs with the same key BEFORE they execute. The worker checks an `_superseded` set (delivered on a side channel or via job-message lookup) at the start of each iteration. Mid-execution jobs run to completion.

**Validation:** profile-capture during a heavy preload sequence (boot, or screen transition that loads new portraits). `update.assets` scope stays under 0.5 ms even when 50+ image decodes are in flight. First-time `Assets.image(key)` cold-path latency moves from "frame hitch" to "1-2 frames delay" (the time to bubble through the job channel + GL upload).

**Estimated cost:** 4-5 days. The handler dispatch + pool lifecycle is the bulk of it; one handler is small.

---

## Validation strategy

Phase 5 ships two measurable wins:

1. **`update.network` scope → ~0 ms** (was ~0.5-1.5 ms typical, higher during reconnect storms). Captured by CP-3.
2. **First-time asset hitch eliminated.** The worst-case 80-200ms PNG decode hitch on first request becomes a 1-2 frame delay (~16-32 ms) while the worker decodes off-thread. Visible in the frame-time graph as the disappearance of a spike, not a number.

Both pre-Phase-5 baselines captured ahead of time + post-Phase-5 captures committed alongside.

**Important capture caveat (discovered 2026-05-28 at T3 baseline; RESOLVED at T3.5):** the `update.network` profiler scope was originally gated to `PLAYING` gameState in `main.lua`. At login/landing, `Network.update` was driven by the screen's own update method, NOT scoped, so the metric read 0.000 even though `socket:receive` was happening. T3.5 (commit `32cb39a`) moved the scope INTO `Network.update` itself so every call site is captured uniformly. New baseline: `update.network.ms = 0.0235` at idle login.

**P5a (T5-T7) DEFERRED (2026-05-28):** `network_tcp.lua` is 1960 lines, 41 public functions, 73 reconnect/circuit-breaker references. The full cutover — moving LuaSocket connection lifecycle, multi-service state, reconnect with exponential backoff, circuit breaker, and event dispatch into a worker thread, marshaling everything back through channels — is genuinely the 4-5 day estimate originally given. It needs interactive login/play/combat validation (each flow exercises a different code path that headless captures can't trigger). Not a single-session task. The IOClient shim (T4, commit `9573542`) locks the contract; the universal `update.network` profiler scope (T3.5, commit `32cb39a`) locks the validation gate. When P5a resumes (dedicated session), the work is: build `services/io/network_worker.lua` replicating NetworkTCP's behavior in a thread, swap IOClient's internals from passthrough to channel dispatch, visual-gate the full network flow. T8-T11 (P5b job pool) are independent and ship now.

**Reference scenes:**
- Cold boot → login (`--scene login --profile-capture 5`) — captures the login network roundtrip
- Inventory cold open (`--scene inventory --profile-capture 5`) — captures first-time portrait + item-icon decode
- Reconnect storm — simulate a forced disconnect (set `Settings.netForceDisconnect = true` or similar) and capture during the retry burst

**Headless test additions:**
- `tests/threading_harness/main.lua` (new) — boots a worker, submits a synthetic job, asserts the result comes back. Validates the JobSystem API headlessly.
- The existing `assets_harness` syntax-checks every new file via the `touched` list (P4 pattern).

---

## Scope guards

- **No protocol changes.** Wire format, message IDs (loaded from `data/protocol.json`), session tokens, reconnect policy — all unchanged. Phase 5 is a thread-cutover, not a protocol refactor.
- **No combat ABILITIES changes** (`[[feedback_abilities_system_ownership]]`).
- **No async Network API changes.** Callbacks fire on the main thread, same shapes as today. The shim makes the cutover invisible to existing callers (Login screen, GachaActions, CombatScreen, etc.).
- **F2/F3 menus** are slated for full rework (`[[project_inventory_party_rework]]`). Phase 5 touches `Assets.image` (used everywhere) but doesn't refactor F2/F3 internals — just makes their image loads async-capable.
- **Pool size = 2.** Anything more is YAGNI until empirical job-queue depth says otherwise.
- **No mid-execution job cancellation.** Pre-start supersession only. Anything bigger requires changing the message protocol later.
- **No `bitser` or other serialization library.** The spike noted bitser is a community recommendation for complex payloads; for our use case (image paths, request blobs of plain tables), the LÖVE Channel native marshaling is sufficient. Add bitser only if a future workload genuinely needs nested-with-functions support.

---

## Open risks

1. **Blocking `socket:select` under `pcall` on Windows.** The Windows scheduler quirk that necessitated `RECV_TIMEOUT = 0` may also bite blocking `select` calls inside the worker. Mitigation: keep a short max-block timeout (e.g. 50 ms) inside the worker's select loop so the worker can also drain its outbound channel responsively. If that still hangs, fall back to non-blocking + a small sleep.

2. **Cyclic-reference table args.** Workers will infinite-loop or error on a table containing itself. `Jobs.submit` should sanity-check (a simple depth-bounded scan) and reject with a clear error. Cheap to add at API surface.

3. **Worker boot ordering.** Workers `require "services.assets.image"` and the manifest. If those modules transitively pull in `love.graphics` somewhere we didn't catch in P5-prelude, the worker crashes at boot — and the error is in `thread:getError()`, which we read only after `Jobs.update` runs. Add a boot-time worker readiness handshake: the worker pushes `{kind="ready"}` on boot, the main thread waits up to 2s for it before allowing submits.

4. **Memory cost.** Two workers at ~2-3 MB Lua state each + their loaded modules + the in-flight `ImageData` queue could spike to ~10-15 MB peak. Acceptable for a desktop game; flag it.

5. **GL upload bottleneck.** If many decode jobs complete in one frame, the main-thread `Jobs.update(dt)` callback dispatch could itself become a hitch (each `newImage` from an `ImageData` is ~1-3 ms). Cap the per-frame GL-upload count at N (e.g. 4) in `Jobs.update`; remaining results stay in the result channel for next frame.

6. **Atlas-bake T8 hang doesn't auto-resolve.** The T8 hang (216-key atlas bake locking boot) lived in the blit/canvas path, NOT the disk-read path. Moving disk reads off-thread doesn't fix it. Phase 5 makes the disk reads non-blocking; the blit-on-canvas issue still needs separate isolation. Document this so we don't expect Phase 5 to "fix" T8.

---

## Cross-references

- Master spec: `docs/superpowers/specs/2026-05-27-aaa-rendering-design.md` § Phase 5 (lines 142-149)
- Audit: `docs/audits/2026-05-28-aaa-gap-audit-perf-batching.md` G-CAP-2
- Synthesis: `docs/audits/2026-05-28-aaa-gap-audit-synthesis.md` SD-2
- Spike: `docs/research/2026-05-28-love2d-threading-spike.md`
- Phase 4 outcome: `docs/audits/p4/summary.md` (atlas bake hang noted as separate concern)
- Memory: `[[aaa-rendering-initiative]]`, `[[feedback_abilities_system_ownership]]`, `[[project_inventory_party_rework]]`, `[[asset_manager]]`, `[[ffi_optimization]]`

---

## Self-review

- [x] Hybrid design (Option C from spike) — dedicated network thread + small job pool. No forced unification.
- [x] P5-prelude flagged as a hard prereq before either worker spawns.
- [x] Validation hooks are mechanical (CP-3 captures + scope drops), not visual.
- [x] No protocol changes; cutover invisible to existing callers.
- [x] Pool size + cancellation semantics + error policy explicit in the API sketch.
- [x] Risks include mitigations.
- [x] T8 atlas-bake hang explicitly NOT in scope; documented so expectations are correct.
- [x] Scope guards reference the relevant memories.
