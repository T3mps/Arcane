# Phase 5 — IO Worker Thread Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move main-thread-blocking I/O off the render path. Network sockets get a dedicated long-lived worker thread; disk reads + ImageData decode flow through a small job pool. After P5, the main thread does GL submission, game logic, and channel drains — nothing else.

**Architecture:** Hybrid two-primitive design. `services/io/` houses the dedicated network worker + thin `IOClient` shim that preserves the existing `Network` callback API. `services/jobs/` houses a 2-worker pool with filesystem-scanned handler convention (`services/jobs/handlers/<kind>.lua`). First handler: `decode_image`. The two primitives don't share infrastructure — sockets and one-shot jobs have different shapes.

**Tech Stack:** LÖVE 11.x `love.thread` + `Channel`, LuaSocket (moves wholly into the network worker), `love.image.newImageData` (worker-safe), `love.graphics.newImage` (main-thread, called from `Jobs.update` callback after decode). CP-3 `--profile-capture` is the validation gate.

**Spec:** `docs/superpowers/specs/2026-05-28-rendering-p5-io-worker-design.md` (commit `40177cb`)
**Spike:** `docs/research/2026-05-28-love2d-threading-spike.md` (the threading-feasibility research)

**Standing constraints:**
- Working tree dirty: targeted `git add` only — NEVER `git add -A` or `git add .`.
- Never skip hooks (`--no-verify`) or bypass signing.
- Combat ABILITIES gameplay off-limits.
- F2/F3 inventory + party menus slated for full rework — cross-cutting Assets contract changes are OK; internals stay deferred.
- No protocol changes. Wire format (`LENGTH:TYPE|TOKEN|PAYLOAD\n`) + message IDs in `data/protocol.json` + session handling unchanged.

---

## Task 1: `image.load_data` split (worker-safe image loader)

The image loader currently does `love.image.newImageData` + `love.graphics.newImage` in one call. Workers can't do the second; split them.

**Files:**
- Modify: `GachaClient/services/assets/image.lua` (add `load_data`, keep `load` as compat wrapper)
- Modify: `GachaClient/tests/assets_harness/main.lua` (new test block + ensure file stays in `touched`)

- [ ] **Step 1: Add failing test for `load_data`**

In `tests/assets_harness/main.lua`, AFTER the existing `print("== BlobBatch.profile ==")` block, add:

```lua
print("== image.load_data (P5 prelude) ==")
do
    local image = assert(loadfile(CLIENT .. "services/assets/image.lua"))()
    -- load_data should return (ImageData, bytes, err) without calling love.graphics.
    -- Headless: we can't actually load a PNG without love.image being real, so verify
    -- the function exists + has the right signature shape, and that the legacy load()
    -- wrapper still exists for back-compat.
    assertEq(type(image.load_data), "function", "load_data exists")
    assertEq(type(image.load),      "function", "load (legacy) still exists")
end
```

- [ ] **Step 2: Verify failing**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/assets_harness
```
Expected: FAIL on `load_data` not being a function.

- [ ] **Step 3: Refactor image.lua**

Replace the current body of `GachaClient/services/assets/image.lua`:

```lua
-- services/assets/image.lua
-- Loader for textures. Two entry points:
--   load_data(path, opts) — worker-safe. Returns (ImageData, bytes, err). No GL.
--   load(path, opts)      — main-thread. Returns (Image, bytes, err). GL upload + filter.
-- Workers that decode images off-thread call load_data and push the ImageData over a
-- Channel; the main thread then runs love.graphics.newImage on the result.
local M = {}

--- Worker-safe PNG decode. Returns (ImageData, bytes_estimate, errMessage).
function M.load_data(path, opts)
    local ok, imageData = pcall(love.image.newImageData, path)
    if not ok then return nil, 0, imageData end
    local w, h = imageData:getDimensions()
    return imageData, w * h * 4   -- rough RGBA8 byte estimate
end

--- Main-thread Image load. Returns (Image, bytes, errMessage).
function M.load(path, opts)
    opts = opts or {}
    local mipmaps = opts.mipmaps ~= false
    local imageData, bytes, err = M.load_data(path, opts)
    if not imageData then return nil, 0, err end
    local ok, img = pcall(love.graphics.newImage, imageData, { mipmaps = mipmaps })
    if not ok then return nil, 0, img end
    if mipmaps and img.setMipmapFilter then pcall(img.setMipmapFilter, img, "linear") end
    if opts.filter then img:setFilter(opts.filter, opts.filter) end
    return img, bytes
end

return M
```

- [ ] **Step 4: Verify tests pass**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/assets_harness
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
```

Both green. The legacy `Assets.image(key)` path still works because it calls `loaders.image.load(...)` which now layers `load_data` + GL upload.

- [ ] **Step 5: Engine smoke**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 1 --output /tmp/p5-t1-smoke.csv --scene inventory
```
Expected: ~200 frames captured, no crash, inventory portraits still load.

- [ ] **Step 6: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/assets/image.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "refactor(assets): split image loader into load_data (worker-safe) + load (main) (P5 task 1)"
```

---

## Task 2: Move `Assets.dummyImage` + verify worker-safety

`Assets.dummyImage()` calls `love.graphics.newImage`. Move it to a main-only sidecar so the rest of `Assets.lua` can be `require`d from a worker.

**Files:**
- Modify: `GachaClient/services/Assets.lua` (gate `dummyImage` behind a guard)
- Create: `GachaClient/tests/threading_harness/main.lua` (verifies Assets-from-worker)
- Create: `GachaClient/tests/threading_harness/conf.lua` (LÖVE conf disabling everything except thread/filesystem)

- [ ] **Step 1: Gate `Assets.dummyImage` behind a `love.graphics` check**

In `services/Assets.lua`, find `function Assets.dummyImage()` and update:

```lua
local _dummy
function Assets.dummyImage()
    if not (love and love.graphics and love.graphics.newImage) then
        return nil   -- worker context: no GL surface available, callers must guard
    end
    if not _dummy then
        local d = love.image.newImageData(1, 1)
        d:setPixel(0, 0, 1, 1, 1, 1)
        _dummy = love.graphics.newImage(d)
    end
    return _dummy
end
```

The guarded nil-return is the worker-safe shape; existing main-thread callers that never enter that branch keep working.

- [ ] **Step 2: Create the threading harness**

`GachaClient/tests/threading_harness/conf.lua`:

```lua
function love.conf(t)
    t.window = false
    t.modules.graphics = false
    t.modules.audio    = false
    t.modules.sound    = false
    t.modules.video    = false
    t.modules.touch    = false
    t.modules.joystick = false
    t.modules.keyboard = false
    t.modules.mouse    = false
    t.modules.window   = false
end
```

`GachaClient/tests/threading_harness/main.lua`:

```lua
-- Headless harness validating that worker-targeted modules require cleanly without
-- love.graphics. We don't actually spawn a worker (LÖVE's love.thread API isn't
-- meaningful here); we simulate worker context by disabling love.graphics in conf.lua
-- and requiring the modules.
local CLIENT = "D:/dev/starworks/Gacha/GachaClient/"
package.path = CLIENT .. "?.lua;" .. CLIENT .. "?/init.lua;" .. package.path

local fails = 0
local function fail(m) fails = fails + 1; print("  FAIL: " .. m) end
local function assertEq(a, b, what) if a ~= b then fail((what or "eq") .. " expected " .. tostring(b) .. " got " .. tostring(a)) end end

print("== love.graphics absence ==")
assertEq(love.graphics, nil, "graphics module disabled by conf.lua")

print("== worker-safe modules require cleanly ==")
local mods = {
    "services.assets.cache",
    "services.assets.manifest",
    "services.assets.image",      -- load_data path is worker-safe; load() guarded
    "services.assets.data",
    "services.assets.skyline",
    "services.assets.atlas",
}
for _, name in ipairs(mods) do
    local ok, err = pcall(require, name)
    assertEq(ok, true, name .. " require: " .. tostring(err))
end

print("== Assets module loads (graphics-gated) ==")
local ok, err = pcall(require, "services.Assets")
assertEq(ok, true, "services.Assets require under no-graphics: " .. tostring(err))

print("== image.load_data signature ==")
local image = require "services.assets.image"
assertEq(type(image.load_data), "function", "load_data exists in worker context")

if fails == 0 then print("\nALL THREADING HARNESS CHECKS PASSED") else print(("\n%d FAILED"):format(fails)) end
os.exit(fails == 0 and 0 or 1)
```

- [ ] **Step 3: Run the threading harness**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/threading_harness
```

Expected: ALL THREADING HARNESS CHECKS PASSED.

If `services.Assets` fails to require, find the OTHER `love.graphics.*` call site in the module + apply the same guard. Common suspects: any place that calls `love.graphics.newCanvas` at module scope (probably none — most calls are inside functions).

- [ ] **Step 4: Run regular harnesses too**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/assets_harness
```

Both still green.

- [ ] **Step 5: Engine smoke**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 1 --output /tmp/p5-t2-smoke.csv --scene inventory
```
Expected: 200+ frames, no crash, inventory still works (dummyImage's nil-return path doesn't trigger in main-thread context).

- [ ] **Step 6: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/Assets.lua GachaClient/tests/threading_harness/main.lua GachaClient/tests/threading_harness/conf.lua
git -C D:/dev/starworks/Gacha commit -m "feat(assets): worker-safe Assets module + threading harness (P5 task 2)"
```

---

## Task 3: Network baseline capture

Capture pre-Phase-5 `update.network` metrics so we have a measurable cutover gate.

**Files:**
- Capture only: `docs/audits/p5/network-before.csv`

- [ ] **Step 1: Create the directory**

```bash
mkdir -p D:/dev/starworks/Gacha/docs/audits/p5
```

- [ ] **Step 2: Capture login + click-to-play + game-world boot sequence**

The headless `--profile-capture` boots straight to login. Capture a full 5-second window to span the initial connect + reconnect retries + session loading.

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 5 --output D:/dev/starworks/Gacha/docs/audits/p5/network-before.csv --scene login
```

Expected: ~1000 frames, CSV written.

- [ ] **Step 3: Record steady-state `update.network` average**

Inspect the CSV:

```bash
awk -F, 'NR==3{for(i=1;i<=NF;i++) if($i=="update.network.ms") col=i} NR>62 && NR<400 { sum+=$col; n++ } END { printf "update.network.ms avg: %.3f (n=%d)\n", sum/n, n }' D:/dev/starworks/Gacha/docs/audits/p5/network-before.csv
```

Record the number — that's the baseline P5a needs to drop to ~0.

- [ ] **Step 4: Commit baseline**

```bash
git -C D:/dev/starworks/Gacha add docs/audits/p5/network-before.csv
git -C D:/dev/starworks/Gacha commit -m "docs(render): P5 network baseline capture (task 3)"
```

---

## Task 4: `IOClient` shim — main-thread placeholder

Build the shim BEFORE the worker so the contract is locked. Initially `IOClient` just wraps the existing synchronous `network_tcp.lua` calls — same callback shape the rest of the codebase consumes. Task 6 swaps the internals to the worker; consumers never notice.

**Files:**
- Create: `GachaClient/services/io/IOClient.lua`

- [ ] **Step 1: Inspect the existing `Network` module API**

Read `GachaClient/network.lua` + `GachaClient/network_tcp.lua` to understand:
- Public functions called by app code (`connect`, `disconnect`, `send`, callback registration via `Network.on(event, fn)`, `resumeSession`, `cancelReconnect`, `logout`, `resetCircuitBreaker`, `update(dt)`)
- Callback events emitted (`reconnecting`, `reconnected`, `reconnectNeedsAuth`, `reconnectFailed`, `sessionExpired`)

The shim must expose the SAME callable shape. Trim wherever the legacy API isn't actually used (run `grep -rn "Network\." GachaClient/` to enumerate call sites).

- [ ] **Step 2: Implement IOClient as a passthrough**

Create `GachaClient/services/io/IOClient.lua`:

```lua
-- services/io/IOClient.lua
-- Main-thread shim over the network stack. P5a Task 4 ships this as a transparent
-- passthrough to network_tcp; Task 6 swaps the internals to a worker thread without
-- changing the consumer-facing API.
--
-- All callbacks fire on the main thread. Events: reconnecting, reconnected,
-- reconnectNeedsAuth, reconnectFailed, sessionExpired.
local TCP = require "network_tcp"   -- existing synchronous network code

local IOClient = {}

-- Event subscribers: { eventName -> { fn1, fn2, ... } }
local _subs = {}

function IOClient.on(event, fn)
    _subs[event] = _subs[event] or {}
    table.insert(_subs[event], fn)
end

local function emit(event, ...)
    local fns = _subs[event]
    if not fns then return end
    for _, fn in ipairs(fns) do pcall(fn, ...) end
end

-- Delegate the existing synchronous API for now. Task 6 replaces these implementations
-- with channel-based dispatch into the worker.
function IOClient.connect(host, port, cb)              return TCP.connect(host, port, cb) end
function IOClient.disconnect()                          return TCP.disconnect() end
function IOClient.send(typeName, payload, cb)           return TCP.send(typeName, payload, cb) end
function IOClient.update(dt)                            return TCP.update(dt) end
function IOClient.cancelReconnect()                     return TCP.cancelReconnect() end
function IOClient.resetCircuitBreaker()                 return TCP.resetCircuitBreaker() end
function IOClient.resumeSession(token, cb)              return TCP.resumeSession(token, cb) end
function IOClient.logout(cb)                            return TCP.logout(cb) end

-- Wire the existing TCP event hooks through emit() so subscribers get them.
-- (TCP module currently fires hooks directly; this shim absorbs that.)
-- If network_tcp has a different event API, adapt accordingly.

return IOClient
```

The shim's purpose at this task is to be transparent. Adapt the actual delegation pattern to whatever `network_tcp.lua` actually exposes (function names may not match exactly — read first, write second).

- [ ] **Step 3: Confirm engine still boots**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 1 --output /tmp/p5-t4-smoke.csv --scene inventory
```

Nothing should change behaviorally — the shim isn't consumed yet, just available.

- [ ] **Step 4: Add to assets harness syntax-check**

In `tests/assets_harness/main.lua` `touched` list:

```lua
    "services/io/IOClient.lua",
```

- [ ] **Step 5: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/io/IOClient.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(io): IOClient main-thread shim (P5 task 4, passthrough only)"
```

---

## Task 5: Network worker entry point

Build the worker file. NOT YET spawned. Standalone, testable via the threading harness.

**Files:**
- Create: `GachaClient/services/io/network_worker.lua` (worker entry point)

- [ ] **Step 1: Implement the worker**

Create `GachaClient/services/io/network_worker.lua`:

```lua
-- services/io/network_worker.lua
-- Worker-thread entry point. Owns LuaSocket connections to auth/account/combat.
-- Receives outbound work over Channel "p5_net_out": flat-table messages
-- { kind, reqId, host, port, typeName, payload, ... }.
-- Pushes inbound work onto Channel "p5_net_in":
-- { kind, reqId, ok, payload, errMessage }
-- Lifecycle: spawned in love.load; quits on { kind = "_quit" } sentinel.
--
-- Wire format: LENGTH:TYPE|TOKEN|PAYLOAD\n (unchanged from network_tcp.lua).
-- Reconnect: lives in the worker; emits {kind="event", event="reconnecting", ...} etc.
require "love.timer"
require "love.event"
local socket = require "socket"

local outChan = love.thread.getChannel("p5_net_out")
local inChan  = love.thread.getChannel("p5_net_in")
local errChan = love.thread.getChannel("p5_net_err")

-- Single connection state. Multiple services would use a table keyed by service name;
-- start with one and grow if combat/account need their own connection slots.
local conn = nil          -- LuaSocket TCP object
local recvBuf = ""        -- partial-line buffer

local function emitEvent(name, ...)
    inChan:push({ kind = "event", event = name, args = { ... } })
end

local function connect(host, port)
    local sock = socket.tcp()
    sock:settimeout(0)    -- non-blocking after setup
    local ok, err = sock:connect(host, port)
    if not ok and err ~= "timeout" and err ~= "Operation already in progress" then
        return nil, err
    end
    return sock
end

-- Drain any complete \n-terminated frames from recvBuf and emit them as inbound messages.
local function drainFrames()
    while true do
        local lineEnd = recvBuf:find("\n", 1, true)
        if not lineEnd then return end
        local line = recvBuf:sub(1, lineEnd - 1)
        recvBuf = recvBuf:sub(lineEnd + 1)
        -- Parse LENGTH:TYPE|TOKEN|PAYLOAD
        local lengthStr, rest = line:match("^(%d+):(.+)$")
        if rest then
            local typeName, token, payload = rest:match("^([^|]+)|([^|]*)|(.*)$")
            inChan:push({
                kind     = "message",
                typeName = typeName,
                token    = token,
                payload  = payload,
            })
        end
    end
end

-- Main loop. Blocking-with-short-timeout select so we can also drain outbound work
-- responsively. The 50ms cap is the "blocking select on Windows" mitigation noted
-- in the spec's risks.
local running = true
while running do
    -- Drain outbound channel (non-blocking).
    while true do
        local msg = outChan:pop()
        if not msg then break end
        if msg.kind == "_quit" then
            running = false
            break
        elseif msg.kind == "connect" then
            local sock, err = connect(msg.host, msg.port)
            if sock then
                conn = sock
                emitEvent("connected")
            else
                emitEvent("connect_failed", err)
            end
        elseif msg.kind == "disconnect" then
            if conn then conn:close(); conn = nil end
            emitEvent("disconnected")
        elseif msg.kind == "send" then
            if conn then
                local body = string.format("%s|%s|%s", msg.typeName, msg.token or "", msg.payload or "")
                local frame = #body .. ":" .. body .. "\n"
                local _, err = conn:send(frame)
                if err and err ~= "timeout" then
                    emitEvent("send_failed", err)
                    conn:close(); conn = nil
                end
            else
                emitEvent("send_failed", "no connection")
            end
        end
    end

    -- Receive whatever's available without blocking longer than 50ms.
    if conn then
        local data, err, partial = conn:receive("*a")
        if data then
            recvBuf = recvBuf .. data
        elseif partial and #partial > 0 then
            recvBuf = recvBuf .. partial
        end
        if err == "closed" then
            emitEvent("disconnected", "closed by peer")
            conn:close(); conn = nil
        end
        drainFrames()
    end

    -- Short sleep to avoid pegging a CPU core when idle. 5ms = 200Hz worker tick.
    love.timer.sleep(0.005)
end

-- Graceful shutdown.
if conn then conn:close() end
inChan:push({ kind = "_quit_ack" })
```

This is a sketch. Adapt to:
- Whatever the existing `network_tcp.lua` actually does (reconnect logic, circuit breaker, multiple-service connections — if any). The spec says "reconnect logic stays in the worker"; preserve all existing reconnect/backoff/circuit-breaker behavior by porting it in.
- The exact event names the existing `Network` module emits (`reconnecting`, `reconnected`, `reconnectNeedsAuth`, `reconnectFailed`, `sessionExpired`) so the IOClient → Network event surface stays the same.

- [ ] **Step 2: Syntax-check via the harness**

The worker file is `require`d normally on the main thread for syntax purposes; it only RUNS as a thread when `love.thread.newThread(...)` spawns it. Add it to the `touched` list:

```lua
    "services/io/network_worker.lua",
```

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/assets_harness
```

Expected: file count +1, all checks pass.

- [ ] **Step 3: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/io/network_worker.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(io): network_worker.lua entry point (P5 task 5, not yet spawned)"
```

---

## Task 6: Wire `IOClient` → worker; cutover network.lua

Now flip the IOClient internals from synchronous-passthrough to channel-based dispatch into the worker. Spawn the worker in `love.load`. Drain inbound in `IOClient.update`. Wire reconnect events back to subscribers.

**Files:**
- Modify: `GachaClient/services/io/IOClient.lua` (replace passthrough with channel dispatch + worker spawn)
- Modify: `GachaClient/network.lua` (route through `IOClient` instead of `network_tcp` directly)
- Modify: `GachaClient/main.lua` (no changes ideally; if `Network.update(dt)` is called from `love.update`, that path keeps working)

- [ ] **Step 1: Rewrite IOClient to dispatch via channels**

In `services/io/IOClient.lua`, replace the body:

```lua
local IOClient = {}

local _subs       = {}
local _outChan    = nil
local _inChan     = nil
local _errChan    = nil
local _workerHnd  = nil   -- love.Thread handle
local _ready      = false
local _reqIdSeq   = 0
local _pendingCbs = {}    -- reqId -> callback fn

function IOClient.on(event, fn)
    _subs[event] = _subs[event] or {}
    table.insert(_subs[event], fn)
end

local function emit(event, ...)
    local fns = _subs[event]
    if not fns then return end
    for _, fn in ipairs(fns) do pcall(fn, ...) end
end

-- Spawn the worker (idempotent). Called by Network.connect or first IOClient.update.
local function ensureWorker()
    if _workerHnd then return end
    _outChan   = love.thread.getChannel("p5_net_out")
    _inChan    = love.thread.getChannel("p5_net_in")
    _errChan   = love.thread.getChannel("p5_net_err")
    _workerHnd = love.thread.newThread("services/io/network_worker.lua")
    _workerHnd:start()
end

local function nextReqId()
    _reqIdSeq = _reqIdSeq + 1
    return _reqIdSeq
end

function IOClient.connect(host, port, cb)
    ensureWorker()
    local id = nextReqId()
    if cb then _pendingCbs[id] = cb end
    _outChan:push({ kind = "connect", reqId = id, host = host, port = port })
end

function IOClient.disconnect()
    if not _outChan then return end
    _outChan:push({ kind = "disconnect" })
end

function IOClient.send(typeName, payload, cb)
    ensureWorker()
    local id = nextReqId()
    if cb then _pendingCbs[id] = cb end
    _outChan:push({ kind = "send", reqId = id, typeName = typeName, payload = payload })
end

-- Called by Network.update each frame. Drains the inbound channel + fires callbacks/events.
function IOClient.update(dt)
    if not _inChan then return end
    while true do
        local msg = _inChan:pop()
        if not msg then break end
        if msg.kind == "message" then
            -- Inbound protocol message. Existing Network module dispatches by typeName.
            emit("message", msg.typeName, msg.token, msg.payload)
        elseif msg.kind == "event" then
            emit(msg.event, table.unpack(msg.args or {}))
        elseif msg.kind == "_quit_ack" then
            _workerHnd = nil
        end
    end
    -- Worker errors surface in thread:getError(). Check + log + emit as a session_error.
    if _workerHnd and _workerHnd:getError() then
        local err = _workerHnd:getError()
        emit("session_error", err)
        _workerHnd = nil
    end
end

-- Lifecycle helpers (called by network.lua's facade).
function IOClient.shutdown()
    if not _outChan then return end
    _outChan:push({ kind = "_quit" })
    if _workerHnd then _workerHnd:wait(); _workerHnd = nil end
end

-- Stubs for the legacy API surface the rest of the app expects. Adapt to network.lua's
-- actual signatures; some of these may be no-ops in the worker model (reconnect lives
-- in the worker now), others may delegate via additional channel messages.
function IOClient.cancelReconnect()
    if not _outChan then return end
    _outChan:push({ kind = "cancel_reconnect" })
end

function IOClient.resetCircuitBreaker()
    if not _outChan then return end
    _outChan:push({ kind = "reset_circuit_breaker" })
end

function IOClient.resumeSession(token, cb)
    -- Resume = a specific "send" with the resume protocol message. Wire as send().
    return IOClient.send("ResumeSession", token, cb)   -- adapt to actual protocol name
end

function IOClient.logout(cb)
    return IOClient.send("Logout", "", cb)              -- adapt to actual protocol name
end

return IOClient
```

- [ ] **Step 2: Adapt `network.lua` to use `IOClient`**

`GachaClient/network.lua` is the public-facing facade most app code consumes (`Network.connect`, `Network.send`, `Network.on(...)`, `Network.update(dt)`). Make it a thin wrapper over `IOClient`:

```lua
-- network.lua
local IOClient = require "services.io.IOClient"

local Network = {}

-- Forward the public API one-to-one.
Network.on                  = IOClient.on
Network.connect             = IOClient.connect
Network.disconnect          = IOClient.disconnect
Network.send                = IOClient.send
Network.update              = IOClient.update
Network.cancelReconnect     = IOClient.cancelReconnect
Network.resetCircuitBreaker = IOClient.resetCircuitBreaker
Network.resumeSession       = IOClient.resumeSession
Network.logout              = IOClient.logout
Network.shutdown            = IOClient.shutdown

return Network
```

Adapt to whatever the existing `network.lua` actually exposed (read it first). Existing message-dispatch hooks (e.g., `Network.on("message", function(typeName, token, payload) ... end)`) keep working because `IOClient.update` emits a `"message"` event that subscribers handle.

- [ ] **Step 3: Shut down the worker on quit**

In `main.lua`, find the existing `function love.quit() Network.disconnect() end` and update:

```lua
function love.quit()
    Network.shutdown()   -- pushes _quit + waits for the worker
end
```

- [ ] **Step 4: Engine smoke — full network flow**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 5 --output /tmp/p5-t6-smoke.csv --scene login
```

Expected: 1000+ frames, the [NET] log lines appear (login attempt visible), the worker logs its own activity, `ProfileCapture wrote N frames` at the end. The CSV `update.network.ms` column should now be ~0 since the actual network work happens on the worker.

If the engine hangs or crashes, the most likely culprit is the worker's pop-from-empty-buffer loop or the LuaSocket semantics in non-blocking mode. Add `_errChan` reads to surface worker errors:

```lua
while true do
    local err = _errChan:pop()
    if not err then break end
    log.warn("network worker err: %s", tostring(err))
end
```

- [ ] **Step 5: USER VISUAL GATE — full network workflow**

User runs the game normally:
1. Engine boots → login screen appears → enter credentials → click login → confirm reaches the click-to-play landing
2. Click play → world loads
3. F1 (gacha) → pull → reveal works
4. F5 (combat) → combat runs
5. Quit → engine shuts down cleanly (no hung worker)

Any reconnect flow (force-disconnect from server, restart server while client is up) should still reconnect-with-backoff.

- [ ] **Step 6: Commit on visual GO**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/io/IOClient.lua GachaClient/network.lua GachaClient/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(io): cutover Network to dedicated worker thread (P5 task 6)"
```

---

## Task 7: P5a validation capture

Capture the post-cutover network metrics + verify the win.

**Files:**
- Capture: `docs/audits/p5/network-after.csv`
- Modify: `docs/superpowers/specs/2026-05-28-rendering-p5-io-worker-design.md` (append P5a outcome block)

- [ ] **Step 1: Capture post-cutover**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 5 --output D:/dev/starworks/Gacha/docs/audits/p5/network-after.csv --scene login
```

- [ ] **Step 2: Compare before vs after**

```bash
for f in before after; do
    awk -F, -v label=$f 'NR==3{for(i=1;i<=NF;i++) if($i=="update.network.ms") col=i} NR>62 && NR<400 { sum+=$col; n++ } END { printf "%s: update.network.ms avg=%.3f (n=%d)\n", label, sum/n, n }' D:/dev/starworks/Gacha/docs/audits/p5/network-$f.csv
done
```

Target: `after < before / 4` (heuristic) — ideally `after ≈ 0`.

If `after >= before / 2`, something's wrong: maybe the worker isn't actually doing the network calls (IOClient is still routing synchronously) or the network-thread costs are being attributed to the wrong scope. Diagnose before declaring victory.

- [ ] **Step 3: Append P5a outcome to spec**

Add an "Outcome" subsection to the spec (or update the existing one if T2 already touched it). Record the before/after numbers + commit SHA.

- [ ] **Step 4: Commit**

```bash
git -C D:/dev/starworks/Gacha add docs/audits/p5/network-after.csv docs/superpowers/specs/2026-05-28-rendering-p5-io-worker-design.md
git -C D:/dev/starworks/Gacha commit -m "docs(render): P5a validation capture + outcome (task 7)"
```

---

## Task 8: `JobSystem` skeleton + headless tests

Pure-Lua dispatch logic (the worker-spawn side comes in Task 9). Headless TDD with factory injection like SpriteBatch/MeshBatch.

**Files:**
- Create: `GachaClient/services/jobs/JobSystem.lua`
- Modify: `GachaClient/tests/render_harness/main.lua` (new test block)
- Modify: `GachaClient/tests/assets_harness/main.lua` (syntax-check inclusion)

- [ ] **Step 1: Write failing tests**

In `tests/render_harness/main.lua` after the existing Widget-culling block, add:

```lua
print("== JobSystem (P5) ==")
do
    local Jobs = require "services.jobs.JobSystem"
    -- Inject a fake channel pair + a fake worker. The test exercises submit + update + cancel
    -- + supersession without spawning a real thread.
    local outChan = { _q = {}, push = function(self, v) self._q[#self._q+1] = v end,
                      pop  = function(self) return table.remove(self._q, 1) end }
    local inChan  = { _q = {}, push = function(self, v) self._q[#self._q+1] = v end,
                      pop  = function(self) return table.remove(self._q, 1) end }
    Jobs._reset({ outChan = outChan, inChan = inChan })

    -- Submit a job, expect it on the outbound channel.
    local fired = {}
    local id = Jobs.submit("decode_image", { path = "x.png" }, function(ok, result)
        fired[#fired+1] = { ok = ok, result = result }
    end)
    eq(type(id), "number", "submit returns numeric id")
    eq(#outChan._q, 1, "one job on outbound channel")
    eq(outChan._q[1].kind, "decode_image", "kind set on outbound message")
    eq(outChan._q[1].id, id, "id matches")

    -- Simulate the worker pushing a result.
    inChan:push({ id = id, ok = true, result = { magic = 42 } })
    Jobs.update(0)
    eq(#fired, 1, "callback fired after update")
    eq(fired[1].ok, true, "ok=true forwarded")
    eq(fired[1].result.magic, 42, "result forwarded")

    -- Cancel before update consumes — cancellation should drop the in-flight tracker.
    local id2 = Jobs.submit("decode_image", { path = "y.png" }, function() fail("cancelled job fired") end)
    Jobs.cancel(id2)
    inChan:push({ id = id2, ok = true, result = {} })
    Jobs.update(0)
    -- Cancelled jobs are dropped silently; fail() above won't have been called.

    -- Supersession: same supersedeKey replaces the earlier in-flight.
    local idA = Jobs.submit("decode_image", { path = "a" }, function() fail("superseded job fired") end, { supersedeKey = "atlas" })
    local idB = Jobs.submit("decode_image", { path = "b" }, function(ok, r) fired[#fired+1] = { id = "B", ok = ok } end, { supersedeKey = "atlas" })
    inChan:push({ id = idA, ok = true, result = {} })
    inChan:push({ id = idB, ok = true, result = {} })
    Jobs.update(0)
    eq(#fired, 2, "only the non-superseded job fired (1 from earlier + 1 from B)")
    eq(fired[2].id, "B", "B is the second fire")
end
```

- [ ] **Step 2: Verify failing**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
```
Expected: FAIL on `module 'services.jobs.JobSystem' not found`.

- [ ] **Step 3: Implement JobSystem**

Create `GachaClient/services/jobs/JobSystem.lua`:

```lua
-- services/jobs/JobSystem.lua
-- Main-thread job dispatch + result fan-out. Workers (see services/jobs/worker.lua)
-- pull jobs from a shared outbound channel and push results to an inbound channel.
-- Jobs.update(dt) drains the inbound channel each frame and fires callbacks.
--
-- Job format (outbound):
--   { id, kind, data }
-- Result format (inbound):
--   { id, ok, result }                       -- ok=true:  result is handler's return value
--   { id, ok = false, result = errMessage }  -- ok=false: pcall error from the worker
--
-- Supersession (via opts.supersedeKey): later submits with the same key cancel earlier
-- in-flight jobs at the dispatcher; the worker may still complete them, but the result
-- is dropped on arrival.
local JobSystem = {}

local _outChan = nil
local _inChan  = nil
local _idSeq   = 0
local _pending = {}    -- id -> { cb, supersedeKey, cancelled = false }
local _superseded = {} -- id -> true (jobs to drop when their result arrives)

local function nextId()
    _idSeq = _idSeq + 1
    return _idSeq
end

-- Internal test seam.
function JobSystem._reset(opts)
    opts = opts or {}
    _outChan, _inChan = opts.outChan, opts.inChan
    _idSeq, _pending, _superseded = 0, {}, {}
end

function JobSystem.boot()
    if _outChan then return end   -- already booted (or test-injected)
    _outChan = love.thread.getChannel("p5_jobs_out")
    _inChan  = love.thread.getChannel("p5_jobs_in")
end

function JobSystem.submit(kind, data, onDone, opts)
    JobSystem.boot()
    local id = nextId()
    _pending[id] = { cb = onDone, supersedeKey = opts and opts.supersedeKey }
    -- Supersession: mark any earlier pending with the same key as superseded.
    if opts and opts.supersedeKey then
        for pid, p in pairs(_pending) do
            if pid ~= id and p.supersedeKey == opts.supersedeKey then
                _superseded[pid] = true
                p.cancelled = true
            end
        end
    end
    _outChan:push({ id = id, kind = kind, data = data })
    return id
end

function JobSystem.cancel(id)
    local p = _pending[id]
    if p then p.cancelled = true; _superseded[id] = true end
end

-- Drain inbound channel + fire callbacks. Called from love.update.
function JobSystem.update(dt)
    if not _inChan then return end
    while true do
        local msg = _inChan:pop()
        if not msg then break end
        local p = _pending[msg.id]
        _pending[msg.id]    = nil
        _superseded[msg.id] = nil
        if p and not p.cancelled then
            if p.cb then pcall(p.cb, msg.ok, msg.result) end
        end
        -- Cancelled / superseded jobs: result discarded.
    end
end

return JobSystem
```

- [ ] **Step 4: Verify tests pass**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient/tests/render_harness
```

Expected: ALL RENDER HARNESS CHECKS PASSED, new "JobSystem (P5)" block green.

- [ ] **Step 5: Add to assets harness syntax-check**

```lua
    "services/jobs/JobSystem.lua",
```

- [ ] **Step 6: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/jobs/JobSystem.lua GachaClient/tests/render_harness/main.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(jobs): JobSystem dispatch + supersession + headless tests (P5 task 8)"
```

---

## Task 9: `jobs/worker.lua` + first handler (`decode_image`)

The worker side. Scans `services/jobs/handlers/`, builds name → fn map, drains job channel, pushes results. Spawns the workers at boot via `JobSystem.boot()`.

**Files:**
- Create: `GachaClient/services/jobs/worker.lua` (worker entry)
- Create: `GachaClient/services/jobs/handlers/decode_image.lua` (first handler)
- Modify: `GachaClient/services/jobs/JobSystem.lua` (spawn the pool in `boot`)
- Modify: `GachaClient/main.lua` (call `Jobs.boot()` in `love.load` + `Jobs.update(dt)` in `love.update`)

- [ ] **Step 1: Implement the worker**

Create `GachaClient/services/jobs/worker.lua`:

```lua
-- services/jobs/worker.lua
-- Worker-thread entry point. On boot:
--   1. require love.image / love.filesystem / love.data
--   2. scan services/jobs/handlers/ for handler files, build name -> fn map
--   3. push a ready handshake onto the inbound channel
-- Loop: demand a job from p5_jobs_out, pcall the handler, push the result.
require "love.image"
require "love.data"
require "love.filesystem"
require "love.timer"

local outChan = love.thread.getChannel("p5_jobs_out")
local inChan  = love.thread.getChannel("p5_jobs_in")

-- Build the handler map.
local handlers = {}
local handlerDir = "services/jobs/handlers"
for _, name in ipairs(love.filesystem.getDirectoryItems(handlerDir)) do
    if name:sub(-4) == ".lua" then
        local kind = name:sub(1, -5)
        local ok, fnOrErr = pcall(require, "services.jobs.handlers." .. kind)
        if ok and type(fnOrErr) == "function" then
            handlers[kind] = fnOrErr
        end
    end
end

-- Handshake.
inChan:push({ kind = "_ready", handlers = (function()
    local list = {}
    for k in pairs(handlers) do list[#list+1] = k end
    return list
end)() })

-- Job loop.
while true do
    local msg = outChan:demand()
    if msg.kind == "_quit" then break end
    local handler = handlers[msg.kind]
    if not handler then
        inChan:push({ id = msg.id, ok = false, result = "no handler for kind: " .. tostring(msg.kind) })
    else
        local ok, result = pcall(handler, msg.data)
        inChan:push({ id = msg.id, ok = ok, result = result })
    end
end
```

- [ ] **Step 2: Implement decode_image handler**

Create `GachaClient/services/jobs/handlers/decode_image.lua`:

```lua
-- services/jobs/handlers/decode_image.lua
-- Worker-side image decode. Returns { imageData, bytes } or throws on error
-- (the worker pcall-wraps; the throw becomes ok=false in the result).
local image = require "services.assets.image"

return function(data)
    if type(data) ~= "table" or type(data.path) ~= "string" then
        error("decode_image: data must be { path = <string> }")
    end
    local imageData, bytes, err = image.load_data(data.path)
    if not imageData then error("decode_image: " .. tostring(err)) end
    return { imageData = imageData, bytes = bytes }
end
```

- [ ] **Step 3: Spawn the pool in JobSystem.boot**

Update `JobSystem.boot()`:

```lua
local _workers = nil       -- array of love.Thread handles

function JobSystem.boot(opts)
    if _outChan then return end
    _outChan = love.thread.getChannel("p5_jobs_out")
    _inChan  = love.thread.getChannel("p5_jobs_in")
    -- Spawn pool. Default size 2; configurable via opts.poolSize.
    local poolSize = (opts and opts.poolSize) or 2
    _workers = {}
    for i = 1, poolSize do
        local t = love.thread.newThread("services/jobs/worker.lua")
        t:start()
        _workers[#_workers + 1] = t
    end
end

function JobSystem.shutdown()
    if not _outChan or not _workers then return end
    for _ = 1, #_workers do _outChan:push({ kind = "_quit" }) end
    for _, t in ipairs(_workers) do t:wait() end
    _workers = nil
end
```

- [ ] **Step 4: Wire boot + update + shutdown into main.lua**

In `main.lua` `love.load`, after the existing render-graph setup:

```lua
-- P5b: spawn the job pool. Default 2 workers; first consumer is decode_image.
require("services.jobs.JobSystem").boot()
```

In `love.update`, after the existing scopes:

```lua
do local s = Profiler:scope("update.jobs"); guarded("jobs.update", function() require("services.jobs.JobSystem").update(dt) end); s() end
```

In `love.quit`:

```lua
require("services.jobs.JobSystem").shutdown()
Network.shutdown()
```

- [ ] **Step 5: Engine smoke — workers boot + first decode**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 2 --output /tmp/p5-t9-smoke.csv --scene inventory
```

Expected: 200+ frames, the `[Jobs] worker ready ...` log line (you may want to add a print in JobSystem.update when the _ready message arrives), no crash. Inventory still renders normally — Assets.image's main-thread cold path is unchanged at this task; the job pool just exists.

If a worker crashes at boot (handler require failure, missing love module), the error lands in `thread:getError()`. Add a check in `JobSystem.update`:

```lua
for _, t in ipairs(_workers or {}) do
    if t:getError() then log.warn("job worker err: %s", t:getError()) end
end
```

- [ ] **Step 6: Commit**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/jobs/worker.lua GachaClient/services/jobs/handlers/decode_image.lua GachaClient/services/jobs/JobSystem.lua GachaClient/main.lua GachaClient/tests/assets_harness/main.lua
git -C D:/dev/starworks/Gacha commit -m "feat(jobs): worker pool + decode_image handler (P5 task 9)"
```

Don't forget to add the new files to `assets_harness`'s `touched`:

```lua
    "services/jobs/JobSystem.lua",
    "services/jobs/worker.lua",
    "services/jobs/handlers/decode_image.lua",
```

---

## Task 10: `Assets.preload` async path

Make `Assets.preload(items)` use the job pool instead of the synchronous loader. Synchronous `Assets.image(key)` cold path stays unchanged (consumers that need an Image NOW still block).

**Files:**
- Modify: `GachaClient/services/Assets.lua` (rewrite `Assets.preload`)

- [ ] **Step 1: Rewrite preload**

In `services/Assets.lua`, replace `Assets.preload`:

```lua
--- Async preload via the job pool. Schedules a decode_image job per image item; the
--- GL upload happens in the result callback on the main thread. Items of other types
--- (shader/font/data/sound) still synchronously load on this call — they're cheap or
--- main-thread-bound and don't benefit from the worker pipe.
function Assets.preload(items)
    local Jobs = require "services.jobs.JobSystem"
    for _, it in ipairs(items or {}) do
        local atype = it.type or it.atype or it[1]
        local key   = it.key or it[2]
        if atype == "image" then
            -- Skip if already cached.
            local id = "image:" .. key
            if cache:has(id) then goto continue end
            local path = Manifest.resolve("image", key)
            Jobs.submit("decode_image", { path = path }, function(ok, result)
                if not ok then
                    logOnce("image", key, result)
                    cache:put(id, "image", false, 0)
                    return
                end
                local img = love.graphics.newImage(result.imageData)
                if it.opts and it.opts.filter then img:setFilter(it.opts.filter, it.opts.filter) end
                if img.setMipmapFilter then pcall(img.setMipmapFilter, img, "linear") end
                cache:put(id, "image", img, result.bytes)
            end)
        elseif atype == "shader" then Assets.shader(key)
        elseif atype == "font"   then Assets.font(key, it.size or 16, it.opts)
        elseif atype == "data"   then Assets.data(key, it.opts)
        elseif atype == "sound"  then Assets.sound(key, it.mode)
        end
        ::continue::
    end
end
```

- [ ] **Step 2: Engine smoke + asset-heavy scene**

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 3 --output /tmp/p5-t10-smoke.csv --scene inventory
```

If anything in the code currently calls `Assets.preload(...)` with image items, those images now load async. Verify no visible regression (images appear at most 1-2 frames late instead of synchronously).

- [ ] **Step 3: USER VISUAL GATE — preload-heavy flows**

User runs the game:
1. Boot → login: ANY portraits / icons that load via `Assets.preload` (search the code for usages) should appear normally, maybe a tiny pop-in on first frame
2. F2 / F3 / F1: same — visible behavior unchanged

No frame hitches on first-time loads. If a hitch is still visible, the cold path (`Assets.image(key)` direct call) is still synchronous — that's intentional for now; we'd need a callback-style API for true async-everywhere.

- [ ] **Step 4: Commit on visual GO**

```bash
git -C D:/dev/starworks/Gacha add GachaClient/services/Assets.lua
git -C D:/dev/starworks/Gacha commit -m "feat(assets): preload uses job pool for image decode (P5 task 10)"
```

---

## Task 11: P5b validation capture

Verify the asset hitch is gone.

**Files:**
- Capture: `docs/audits/p5/assets-before.csv` (cold-boot inventory cap; take EARLIER if not already captured)
- Capture: `docs/audits/p5/assets-after.csv`
- Modify: `docs/superpowers/specs/2026-05-28-rendering-p5-io-worker-design.md` (P5b outcome block)

- [ ] **Step 1: Capture cold-boot inventory**

Delete LÖVE's save dir cache first so first-time loads actually fire:

```bash
rm -rf "C:/Users/Ethan Temprovich/AppData/Roaming/LOVE/GachaClient/game_cache.json"
```

Then capture:

```bash
D:/dev/starworks/Gacha/ThirdParty/love2d/lovec.exe D:/dev/starworks/Gacha/GachaClient --profile-capture 5 --output D:/dev/starworks/Gacha/docs/audits/p5/assets-after.csv --scene inventory
```

- [ ] **Step 2: Inspect frame-time graph for hitches**

```bash
awk -F, 'NR==3{for(i=1;i<=NF;i++) if($i=="frameMs") col=i} NR>3 && NR<60 { if ($col > 33) printf "frame %d: %.2f ms\n", NR-3, $col }' D:/dev/starworks/Gacha/docs/audits/p5/assets-after.csv
```

Target: in the first ~60 frames (where first-time image loads would normally hitch), no frame > 33ms (i.e., no dropped 30fps frame).

If there's no equivalent "before" capture, the comparison is qualitative: the absence of frame spikes is the win.

- [ ] **Step 3: Append P5b outcome to spec**

Same pattern as Task 7 — record commit SHA + the measurable improvement.

- [ ] **Step 4: Commit**

```bash
git -C D:/dev/starworks/Gacha add docs/audits/p5/assets-after.csv docs/superpowers/specs/2026-05-28-rendering-p5-io-worker-design.md
git -C D:/dev/starworks/Gacha commit -m "docs(render): P5b validation capture + outcome (task 11)"
```

---

## Task 12: Phase 5 outcome summary + memory update

**Files:**
- Create: `docs/audits/p5/summary.md` (per-workstream outcome)
- Modify: `C:/Users/Ethan Temprovich/.claude/projects/D--dev-starworks-Gacha/memory/project_aaa_rendering.md` (append P5 entry)

- [ ] **Step 1: Write summary**

`docs/audits/p5/summary.md`:

```markdown
# Phase 5 — Outcome Summary

## What shipped
[per-task SHA + status]

## Per-workstream metrics
[before/after numbers + captures]

## Durable lessons
[what we learned about LÖVE threading, what bit us, what to do differently next time]

## What's deferred
[atlas-bake hang from T8 of P4 — still open; mid-execution job cancellation; bitser integration if/when a workload needs nested-with-functions]
```

- [ ] **Step 2: Append memory entry**

In `project_aaa_rendering.md`, after the P4 block, add a P5 block with the same structure (commits, shipped components, durable lessons, deferred items). Reference the summary file.

- [ ] **Step 3: Commit**

```bash
git -C D:/dev/starworks/Gacha add docs/audits/p5/summary.md
git -C D:/dev/starworks/Gacha commit -m "docs(render): Phase 5 outcome summary"
```

(The memory file is outside the repo; no `git add` needed.)

---

## Self-review

- [x] Three workstreams clearly separated (prelude / P5a / P5b).
- [x] Each task ends in a commit.
- [x] Visual gates explicit on T6 (network cutover) and T10 (preload async path) — both touch live gameplay.
- [x] Headless TDD where applicable: T1 (image.load_data shape), T2 (worker-safety), T8 (JobSystem dispatch + supersession).
- [x] No protocol changes; existing Network callback API preserved.
- [x] No combat ABILITIES touched.
- [x] F2/F3 internals NOT refactored; only the cross-cutting Assets contract changes.
- [x] Worker error paths handled (thread:getError checked in JobSystem.update + IOClient.update).
- [x] T8 atlas-bake hang explicitly NOT addressed (different code path).
- [x] No `bitser` or third-party threading lib introduced.
- [x] Code shown for every new module + every meaningful refactor.
- [x] Captures committed alongside the code for regression baselines.
