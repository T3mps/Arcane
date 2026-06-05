# V4 Follow-up Audit: Networking + Service Lifetime

**Date:** 2026-06-03 (v4, same day as the v3 remediation arc)
**Dimension:** Networking + service lifetime (new top-level dimension; previously distributed across concurrency, security, cross-cutting)
**Scope:** `Server/Common/src/Net/*` (TcpServerBase, TcpSocket, ServiceClient, ServiceEndpoint, InternalRpcAuth, RpcFraming, Protocol, RateLimiter, SessionCache) plus the three service-derived classes (AuthServer, AccountServer, CombatServer) at the seam where they own per-service connection lifetimes.
**Method:** Read v3 synthesis + v3-concurrency + v3-security + v3-cross-cutting follow-ups; verified the v3 remediation commits (`f46d496..9129a68`) against current code; walked each focus area (framing, recv timeout, detached threads, probe classification, envelope layout, wire surface, isolation, backoff, reconnect, pool pressure) end-to-end.

---

## Verdict

**Concerning.** The v3 networking-adjacent remediations (H-V3-7 explicit destructors, H-V3-11 LruCache Peek/Touch split, H-V3-14(a) envelope version + MAC-over-v + golden file) all hold up in code. Per-connection state is correctly isolated (each `HandleConnection` owns its parser stack-local; `MessageParser` lives in the per-client `ClientConnection`). Backoff is exponential and capped at 30s. The framing layer (`ExtractLengthFramed`) rejects oversized lengths against `MAX_RECEIVE_BUFFER_SIZE`, handles partial frames correctly, and `ParseBody` defensively fails closed on malformed integers. **But H-V3-14(b)'s startup canary is structurally broken**: the server's error responses (`{"error":"unsupported_envelope_version"}` / `{"error":"Authentication failed"}`) are delivered to `ServiceClient::Call` as a parseable JSON payload, which sets `RpcCallStatus::Success` — so `Probe` returns `ProbeOutcome::Ok` for the exact two failure modes the canary was built to detect. The `AuthFailed` and `EnvelopeVersionMismatch` enum members are unreachable code. Beyond that, **the entire TCP surface has no per-IP connection cap and no idle-client kick** — one of the larger pre-launch operational gaps in the tree. Three High-class items and several Mediums round out the surface.

---

## CRITICAL

### C-V4-1. `ServiceClient::Probe` cannot detect `AuthFailed` or `EnvelopeVersionMismatch` — the headline v3 fix is dead code on those paths
**Files:**
- `Server/Common/src/Net/ServiceClient.hpp:258-294` (Probe)
- `Server/Common/src/Net/ServiceClient.hpp:129-246` (Call)
- `Server/Common/src/Net/ServiceEndpoint.hpp:162-264` (HandleConnection MAC + version handling)
**Flagged by:** v4 focus question #4 (Probe failure-mode classification)
**Status:** NEW — undermines H-V3-14(b), shipped in commit `bfbcf2c`.

When `ServiceEndpoint::HandleConnection` rejects an envelope:

```cpp
// v mismatch — line 179
response = {{"error", "unsupported_envelope_version"}, {"expected_v", APHELYON_ENVELOPE_VERSION}};
// MAC mismatch — line 199
response = {{"error", "Authentication failed"}};
```

In both cases the rejection response is serialized via `FormatRpcMessage(response.dump())` and sent over the same socket via `SendAll(...)`. The peer's `ServiceClient::Call` receives the bytes, the parser frames them cleanly, `Json::parse` succeeds, and `Call` returns:

```cpp
return RpcCallResult{ RpcCallStatus::Success, Json::parse(extractResult.rawJson) };  // L235
```

So `Probe` at L263 sees `rpc.ok() == true` and returns `ProbeOutcome::Ok` — claiming the peer is reachable, the MAC matches, and the envelope version agrees. **None of those are true**. The 4 enum members `ProbeOutcome::AuthFailed` / `EnvelopeVersionMismatch` are never returned by `Probe`'s code (`UnknownError` is the only error-class return). Dead code.

**Concrete consequence.** A field deploy where Auth ships with a bumped `APHELYON_ENVELOPE_VERSION` but Account still has the old binary:
- Account's startup `ProbeStartupPeers()` calls `m_authClient.Probe()`.
- Account sends an envelope with the old `v`.
- Auth's ServiceEndpoint rejects with `{"error":"unsupported_envelope_version"}`.
- Account's Probe sees a parseable response → `ProbeOutcome::Ok` → LOG_NET_INFO `"Account startup probe: Auth reachable, MAC + envelope-v OK"` — **a confident lie**.
- Account proceeds to enter its accept loop and serve traffic.
- Every subsequent `SessionCache::Validate` issues an `AuthorizeToken` RPC that comes back as `{"error":"..."}` — same parseable shape, same `rpc.ok()==true`.
- `SessionCache::Validate` at L141 does `result = rpc.value.value("result", rpc.value)` then `info.valid = result.value("valid", false)`. The response object has no `result` field (only `error`), so the outer `value()` returns the whole `rpc.value` (`{"error":"unsupported_envelope_version"}`). Then `value("valid", false)` defaults to false. Every login attempt fails silently with no diagnostic distinguishing it from a real expired session.

The same shape covers MAC mismatch (operator forgot to set `APHELYON_INTERNAL_SECRET` on Auth but set it on Account, or set different values across services).

**Fix.** Inspect the parsed response inside `Probe` before declaring `Ok`:
```cpp
if (rpc.ok()) {
    const auto& v = rpc.value;
    if (v.contains("error")) {
        const std::string err = v.value("error", std::string{});
        if (err == "unsupported_envelope_version") return ProbeOutcome::EnvelopeVersionMismatch;
        if (err == "Authentication failed")        return ProbeOutcome::AuthFailed;
        if (err.rfind("Unknown method", 0) == 0)   return ProbeOutcome::UnknownError;
        return ProbeOutcome::UnknownError;
    }
    if (v.contains("result")) return ProbeOutcome::Ok;
    return ProbeOutcome::UnknownError;  // neither error nor result
}
```
Plus: surface an end-to-end Probe regression test (the existing `EnvelopeShapeTest` and `InternalRpcAuthRoundTripTest` cover the MAC layer but not the wire-level Probe classification). And the same bug shape recurs anywhere else that reads `rpc.ok()` without inspecting the payload — `SessionCache::Validate` for example treats every parseable response as authoritative, so its failure-mode signal is also conflated.

---

## HIGH

### H-V4-1. `ServiceClient::Call` recv timeout (5s) is shorter than Debug-build PBKDF2 (18s on the slow machines noted in commit `0a92bd5`)
**Files:**
- `Server/Common/src/Net/ServiceClient.hpp:152` (`SetSocketTimeout(m_socket, 5000)`)
- `Server/Common/src/Net/ServiceClient.hpp:198-211` (recv loop, no re-attempt on partial)
- `Server/Common/src/Crypto/Crypto.hpp` (`DEFAULT_ITERATIONS = 200000`)
**Flagged by:** v4 focus question #2 (recv timeout behavior)
**Status:** NEW — known issue per commit message but not closed.

`SetSocketTimeout(m_socket, 5000)` sets a 5-second recv timeout. When `SessionCache::Validate` issues `AuthorizeToken` via `m_authClient.Call(...)`, the receive blocks waiting for Auth's response. If Auth's handler runs `Crypto::VerifyPassword` (HandleVerifyCredentials path during a login chain) or just hits a slow Postgres query, the response can take >5s on Debug builds — the commit explicitly notes 18s PBKDF2 on the dev hardware. `recv` returns `<0` with `IsSocketTimeoutError() == true`, the code drops into the catch-all at L203-211, closes the socket, returns `RpcCallStatus::WireError`.

**Impact today.** `SessionCache::Validate` at L125-135 treats `WireError` as fail-closed: the session is rejected, the player sees "Invalid or expired session". A retry recreates the cycle. In Debug this is the dominant pre-launch login-failure-mode the dev team has hit.

**Why it's High.** The 5s is the only timeout — there is no retry-on-timeout, no recv-extending heartbeat, no streaming. It's also not aligned with the Account-side handler budget (handlers can run >5s while holding a stripe lock).

**Fix options.**
1. Bump `ServiceClient` recv to 30s — aligns with the 30s envelope replay window. PBKDF2 200k iters at production hardware (~150-400ms) leaves a 100× safety margin; Debug-machine 18s leaves a 1.7× margin.
2. Make the timeout configurable per ServiceClient instance — Account→Auth needs longer (sees PBKDF2), Auth→Account is bounded by DB query latency.
3. Heartbeat protocol — receiver sends a `{"keepalive":true}` byte every 4s while a handler runs. Adds protocol surface; recommend (1) instead.

### H-V4-2. `ServiceEndpoint::Stop()` 10s drain is bounded but detached threads persist on overrun → UAF window remains
**Files:**
- `Server/Common/src/Net/ServiceEndpoint.hpp:96-101` (detached thread spawn)
- `Server/Common/src/Net/ServiceEndpoint.hpp:121-128` (`wait_for(10s)`)
- `Server/Common/src/Net/TcpServerBase.hpp:38-43,58-61` (base destructor sequencing)
- `Server/Account/src/AccountServer.hpp:115-126`, `Server/Auth/src/AuthServer.hpp:49-56`, `Server/Combat/src/CombatServer.hpp:33-37` (explicit derived destructors)
**Flagged by:** v4 focus question #3 (detached thread lifetime; H-V3-7 verification)
**Status:** NEW — H-V3-7 narrowed the UAF window but did not close it.

The H-V3-7 fix (explicit `~AccountServer/~AuthServer/~CombatServer` calling `Stop()` before derived-member destruction) is the right shape and verifiably eliminates the **common** UAF path. However:

1. `ServiceEndpoint::Stop()` still uses `m_connDrainCv.wait_for(std::chrono::seconds(10), ...)` — bounded. If `m_activeConnections > 0` after 10s, Stop returns and the destructor sequence proceeds.
2. Connection threads are `detach()`ed (L101), so the framework has no way to actually wait for them after the 10s expires.
3. The 5s recv timeout means a handler that's mid-recv exits within 5s of `m_running` going false. A handler that's mid-RPC dispatch (PBKDF2, DB query, Postgres advisory-lock wait) exits whenever it returns, regardless of `m_running`. Handler runtime is unbounded from `Stop()`'s perspective.
4. After Stop returns, derived members (m_cache, m_repository, m_pool, m_authClient) destruct. Then base `~TcpServerBase` runs, which calls `Stop()` again (short-circuit, m_running already false), then base members destruct (m_internalEndpoint).
5. **A detached connection thread still running at this point** holds:
   - A captured `this` ref to a destroyed `InternalRpcHandlers` / `AuthServer::SetupInternalEndpoint` lambda capture.
   - Through that ref, a destroyed `m_cache` / `m_sessionManager` / `m_pool`.
   - The lambda's `[this]` closure becomes a dangling pointer.

**Trigger condition.** A handler running >10s during shutdown. Realistic triggers: Postgres advisory-lock contention behind a stripe lock; the integration test session that prompted db-reset noted by v3; a Debug-build PBKDF2 verify that hits the 18s slow path; a `CleanupExpired` that walks 10k LRU entries.

**Fix.**
1. **Replace `wait_for(10s)` with `wait` (unbounded)** + a watchdog log every 30s (`LOG_NET_WARN("ServiceEndpoint: Stop drain still waiting for {} connections after {}s", count, elapsed)`). Trades shutdown latency for safety.
2. **OR `std::thread::join` instead of `detach`** — track per-connection threads in a vector under `m_methodsMutex` (or a new mutex), have `Stop()` join them all. This forces correctness at the cost of unbounded `Stop()` wait — same trade as (1).
3. **OR move `m_internalEndpoint` ownership to derived classes** — V3-M5 in the concurrency follow-up suggested this. Larger refactor; defer until the simpler fixes are exhausted.

### H-V4-3. No connection cap on `TcpServerBase` accept loop or `ServiceEndpoint` accept loop → resource-exhaustion DoS
**Files:**
- `Server/Common/src/Net/TcpServerBase.hpp:93-122` (accept loop spawns a thread per accepted socket; no limit)
- `Server/Common/src/Net/ServiceEndpoint.hpp:73-103` (same pattern, detached)
**Flagged by:** v4 focus questions #1, #10 (TCP framing robustness, connection pool pressure)
**Status:** NEW — never flagged in v1/v2/v3 because the dimension was distributed.

Both TCP entry points spawn one OS thread per accepted connection with no cap, no per-IP rate limit on `accept`, and no slow-loris defense:

- **TcpServerBase**: each thread allocates a `std::thread` (typically 1MB stack on Windows), a `MessageParser` with `std::string m_buffer` (up to `MAX_RECEIVE_BUFFER_SIZE=64KB`), and a `RECV_CHUNK_SIZE=4KB` recv buffer. Total per-connection footprint: ~1.1MB minimum, ~1.1MB + parser fill peak.
- **ServiceEndpoint**: same shape, detached, loopback-only — so the practical attack surface is "local process with no internal-RPC secret" (low for prod, but means a Debug exploit/crash via a malicious local process can OOM the service).

A 10k-connection flood (achievable with `nmap`, `hping3`, or `ab` on the loopback path) is ~11 GB of address space + 10k OS threads + 10k file descriptors. Windows default per-process FD limit is high enough to not save us; default thread limit is bounded by virtual address space which a 64-bit process won't hit at 10k threads but the OS thread scheduler will degrade badly.

The downstream **rate limiter does NOT mitigate this** — `RateLimiter::Allow` runs INSIDE `OnProcessMessage`, which runs INSIDE `HandleClient`, which has already accepted the connection and allocated everything. By the time the limiter sees the IP, all the per-connection memory is committed.

**Pre-launch a non-issue** — this is the kind of bug that bites the first time a botnet finds you. **It is the largest single networking-dimension finding** in this audit.

**Fix.**
1. **Per-IP connection limit at accept time** — track `std::unordered_map<std::string, int>` of active connections by client IP under `m_clientsMutex`. Reject (close immediately) at accept if `count[ip] >= MAX_PER_IP` (suggest 8 for client-facing, 32 for internal-RPC). Logs as `LOG_NET_WARN`.
2. **Total connection cap** — reject at accept if `m_clients.size() >= MAX_TOTAL` (suggest 5000 client-facing, 256 internal).
3. **Slow-loris timeout** — track `lastActivityAt` per ClientConnection, set in `HandleClient` after every successful frame parse. CleanupTick disconnects clients with no completed frames in N seconds (suggest 30s).
4. **(Optional) backlog limit on `listen()`** — currently `listen(sock, 10)` which is already conservative. Document the 10-deep backlog as part of the layered defense.

---

## MEDIUM

### M-V4-1. `Probe` holds `ServiceClient::m_mutex` for the entire retry budget (up to ~10s) → blocks all concurrent RPC during startup canary
**File:** `Server/Common/src/Net/ServiceClient.hpp:258-294`

`Probe` calls `Call(...)` inside the for-loop. `Call` takes `m_mutex` (L131). On `Unreachable`, `Probe` sleeps `delayMs` (default 2000) while NOT holding the mutex (`Call` returned), then loops. But `Call` itself takes the mutex; so during the actual probe round-trip the mutex IS held. For 5 retries × (delayMs + 5s recv timeout) ≈ 35s worst case, no other RPC on this ServiceClient can dispatch.

In Account's startup sequence the Probe runs in `OnStarted()` which is on the accept-loop thread BEFORE the accept loop starts. So no client-RPC traffic is competing. Today this is harmless. Risk: any future code that launches a background thread that uses the same `m_authClient` during startup (e.g., a metrics reporter) would block.

**Fix.** Document the contract ("Probe must run during single-threaded startup; do not call concurrently with Call"). Or split Probe to use a separate dedicated socket so it doesn't share state with the production ServiceClient.

### M-V4-2. `Probe` retry sleep happens AFTER releasing the mutex but BEFORE re-acquiring → during sleep another thread can fully consume `Call` budgets
**File:** `Server/Common/src/Net/ServiceClient.hpp:271-279`

`Probe`'s sleep is at L277 `std::this_thread::sleep_for(std::chrono::milliseconds(delayMs))` — happens outside any lock (m_mutex was released when `Call` returned). On the next iteration `Call` re-takes the mutex. If another thread snuck in between and modified `m_backoffMs` (e.g., recorded a different failure), the backoff window may now be longer than the retry's planned next-attempt time, causing `Call` to immediately return `Unreachable` again without an actual connect. Bounded loss (one extra Unreachable return) but inverts the retry semantic from "retry every delayMs" to "retry after the current backoff settles."

**Fix.** Acceptable as-is given the constraint Probe runs single-threaded during startup; document in the function comment.

### M-V4-3. No connect timeout on `ConnectSocket` — blocking `connect()` can hang indefinitely if peer's accept queue is saturated
**File:** `Server/Common/src/Net/TcpSocket.hpp:155-173`

`ConnectSocket` does a blocking `connect()` with no `SO_SNDTIMEO` setup. On Windows the OS-level connect timeout is 21 seconds (3 retries × 7s each); on Linux similar. During this window the calling thread is fully blocked and the mutex is held. If a peer's listen backlog is full or stuck (slow-loris on the listen socket), every ServiceClient::Call that needs to reconnect stalls for 21s.

**Fix.** Use non-blocking connect + `select` with timeout, or `WSAEventSelect` on Windows. ~30 LOC; replaces the current naive `connect`.

### M-V4-4. `MessageParser` buffer can grow to 64KB per connection before overflow rejection — no compaction on slow consumers
**File:** `Server/Common/src/Net/TcpServerBase.hpp:277-318`

`AppendData` rejects if total > `MAX_RECEIVE_BUFFER_SIZE=65536`. But on every successful extract, `erase(0, frame.consumed)` is O(n) on `std::string` — moves the tail. A pathological client sending many small messages back-to-back triggers repeated O(n) copies. With 10k connections each holding 32KB on average, the steady-state working set is 320MB just for in-flight parser buffers.

Plus the parser `m_buffer` is a `std::string`, so even after `erase` returns size to 0, the internal capacity stays at the high-water mark — `shrink_to_fit` is never called. A connection that briefly hit 50KB stays at 50KB-capacity for its lifetime.

**Fix.**
- Use a deque or ring buffer instead of `std::string`.
- Or periodically `shrink_to_fit` (`if (m_buffer.empty() && m_buffer.capacity() > 4096) m_buffer.shrink_to_fit()`).
- Lower priority; magnitude is small for current scale.

### M-V4-5. `ExtractLengthFramed` accepts non-numeric tail in length prefix via `std::stoull` partial parse
**File:** `Server/Common/src/Net/TcpSocket.hpp:206`

`std::stoull("12abc:body")` returns 12 — partial parse on the colon position substring. The colon position search at L197 happens first, then the substring is `"12abc"`, then `stoull` returns 12 leaving "abc" unconsumed. Body length 12 is then required for the frame. Garbage in the length prefix is silently accepted as the leading digits.

**Not a security issue** (the max-body check caps at 64KB) but unexpected. A future log audit might be confused by frames "successfully parsed" with a prefix like `"99garbage:..."`.

**Fix.** Validate that `colonPos - 0` == `pos` returned by `stoull` (the `idx` out-param). If not, treat as malformed. 2 LOC.

### M-V4-6. `Reconnect` path in `ServiceClient::Call` correctly closes m_socket but does not flush parser state on Send failure
**File:** `Server/Common/src/Net/ServiceClient.hpp:184-190`

When `SendAll` fails:
```cpp
CloseSocket(m_socket);
m_socket = INVALID_SOCK;
return RpcCallResult{ RpcCallStatus::WireError, Json() };
```

But `m_parser` is NOT cleared. The next `Call` enters the auto-reconnect path at L134, calls `ConnectSocket`, succeeds, then `m_parser.Clear()` at L153 — so parser state is cleaned on reconnect. **However**, between the failed Send and the next Call, `IsConnected()` returns false (socket invalid) but `m_parser` retains the previous frame's leftover bytes (if Send failed mid-write). Race: another thread observing `IsConnected() == false` could conceivably read stale parser state, but `m_parser` is only touched under `m_mutex` so this is a sequencing concern not a thread-safety one.

**Fix.** Call `m_parser.Clear()` immediately after `CloseSocket` on Send failure. 1 LOC. Defense-in-depth.

### M-V4-7. `m_questTokenSecret` is loaded by `InitializeQuests` AFTER `m_internalRpcHandlers` is constructed, but Register() runs before InitializeQuests
**Files:**
- `Server/Account/src/AccountServer.hpp:96-112` (constructor body)
- `Server/Account/src/AccountServer.hpp:282-296` (InitializeQuests sets m_questTokenSecret)

Wait — re-reading: `InitializeQuests()` runs BEFORE `m_internalRpcHandlers.Register()` at L111. So m_questTokenSecret IS populated when Register runs. Verified safe; documenting the cross-cutting init-order dependency because H-V3-10 closed the analogous dummy-hash issue but didn't extract the general principle. Future refactor that moves Register() above InitializeQuests would break quest-token mint inside RPC handlers.

**Fix.** Either extract a `BuildInitOrder()` static_assert-able dependency graph (overkill) or add comments to both Register() and InitializeQuests linking the ordering invariant. 4 LOC of comment.

### M-V4-8. No TCP keepalive on accepted client sockets — half-open connections survive until the next send
**Files:**
- `Server/Common/src/Net/TcpServerBase.hpp:114` (only sets RCVTIMEO)
- `Server/Common/src/Net/TcpSocket.hpp:117-152` (CreateListenSocket sets REUSEADDR but no keepalive)

A client that crashes / NATs away leaves the accepted socket in CLOSE_WAIT or ESTABLISHED on the server side until the OS keepalive (default 2 hours on Windows) fires. During that window the server holds the connection thread, the parser buffer, and the FD. Combined with H-V4-3's no-cap, this is a slow-leak DoS surface.

**Fix.** `setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, ...)` after accept; on Windows additionally configure `SIO_KEEPALIVE_VALS` to a shorter idle (60s) + interval (10s) + count (3). ~15 LOC platform-specific.

### M-V4-9. `SessionCache::Validate` treats every `rpc.ok()==true` as authoritative — inherits C-V4-1's blind spot
**File:** `Server/Common/src/Net/SessionCache.hpp:117-160`

After `m_authClient.Call("AuthorizeToken", ...)` returns `rpc.ok()==true`, the code reads `result.value("valid", false)` and trusts the answer. If the server actually returned `{"error":"Authentication failed"}` (envelope MAC mismatch — i.e., the Account-side internal secret diverged from Auth's), `info.valid=false` is the right outcome by coincidence — the user's session fails. But the cause is misdiagnosed: the log shows `LOG_AUTH_DEBUG("SessionCache: miss, calling Auth RPC")` then a clean failed-session response, **not** the envelope-version-skew that's actually killing every login.

**Fix.** Same as C-V4-1's fix shape — inspect the response for an `error` field and surface it distinctly:
```cpp
if (rpc.ok()) {
    if (rpc.value.contains("error")) {
        LOG_AUTH_WARN("SessionCache: Auth returned RPC-level error: {}", rpc.value.value("error", ""));
        NoteAuthLost();  // treat as integrity failure, not a normal "session invalid"
        return SessionInfo{};
    }
    // ... existing happy path ...
}
```

### M-V4-10. Detached connection thread can outlive its bound `Handler` lambda even when destruction order is correct
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:53-57,210-216`

`RegisterMethod` stores `Handler` (a `std::function`) in `m_methods` under `m_methodsMutex`. During `HandleConnection` L210-216, a copy of the handler is taken under the mutex (`Handler handler = it->second`), then released. The handler copy lives on the thread's stack. So far so good.

But the `Handler` lambda captures `this` (the InternalRpcHandlers instance, AuthServer's lambda capture, etc.). The COPY of the function object still holds a `this` pointer. After H-V3-7's fix, derived destructors call `Stop()` first which drains for 10s; after 10s any still-running handler thread holds a dangling `this` even though the captured `Handler` was copied into its stack — because the **closure's body** dereferences `this` (e.g., `m_cache.LockFor(...)`).

So H-V3-7 narrowed the window to "handlers running >10s during shutdown" but the lambda-capture UAF class is structurally still present. The audit reading correctly identified this (H-V3-1 concurrency). Closing it requires either:
- Lambdas that capture by value (impractical for cache/repository refs).
- Unbounded drain (H-V4-2 fix #1).
- Architectural extraction (H-V4-2 fix #3).

Tracking here as a Medium because the v3 fix narrowed the window enough for current usage; v4 surfacing it ensures the next "let's add a slow RPC handler" doesn't reopen it silently.

---

## LOW / OBSERVATION

### L-V4-1. `RpcParser` and `MessageParser` are functionally identical except for the body parsing step — one shared base would dedupe ~40 LOC
`RpcParser` (in `RpcFraming.hpp`) and `MessageParser::TryExtractMessage` (in `TcpServerBase.hpp`) both call `ExtractLengthFramed` then conditionally parse the body. Could share a CRTP base or a function-pointer template. Low value; cosmetic.

### L-V4-2. `ServiceClient::Disconnect` lock is taken twice when called from the destructor
`~ServiceClient()` calls `Disconnect()` which takes `m_mutex`. After destructor returns, mutex destructs. If a thread was blocked on the mutex when destructor started, behavior is undefined — but `~ServiceClient` is only called from member-destruction in `AuthServer`/`AccountServer`/`CombatServer`, by which time Stop() has joined all internal-endpoint threads. So no concurrent caller exists. Safe.

### L-V4-3. `IsAvailable()` does not update `m_backoffMs` when the backoff window elapses → reports `false` for one more call after backoff expires
**File:** `Server/Common/src/Net/ServiceClient.hpp:111-120`
The check `elapsed >= std::chrono::milliseconds(m_backoffMs)` returns true after the window — at which point `IsAvailable()` returns true correctly. But `m_backoffMs` is NOT reset to 0 until the next successful Connect/Call. So a second `IsAvailable()` call still sees `m_backoffMs > 0` and re-runs the elapsed check. Tiny overhead, no functional issue.

### L-V4-4. `SessionCache` per-token `unordered_map` has no max-size cap
**File:** `Server/Common/src/Net/SessionCache.hpp:246`
`m_cache` grows unbounded with the number of distinct tokens seen. Today's idle-cleanup pattern (RateLimiter has one; SessionCache does not) leaves stale entries forever. At 1k DAU with daily session rotation, that's 365k entries/year. Probably fine for years; flagging because the dimension was distributed in v3 and might be missed.

### L-V4-5. `Probe`'s comment claims it inspects the response body but the implementation doesn't
**File:** `Server/Common/src/Net/ServiceClient.hpp:254-291`
The function comment at L254 says `"WireError → checks the response body for the distinctive 'unsupported_envelope_version' or 'Authentication failed' markers"` but the implementation at L282-291 explicitly chose not to ("Walk the socket directly for one more byte to see if a 'result' came back is overkill"). Comment-implementation drift. The C-V4-1 fix should make the comment true.

### L-V4-6. `ServiceClient`'s 5s recv timeout matches `ServiceEndpoint`'s 5s — symmetric, but both diverge from `TcpServerBase`'s 100ms client recv
**Files:** `ServiceClient.hpp:152`, `ServiceEndpoint.hpp:91`, `TcpSocket.hpp:41`
The asymmetry is intentional (internal RPC has fewer connections, longer handler work; client TCP has many connections, polled often). Worth a comment block in `TcpSocket.hpp`'s `RECV_TIMEOUT_MS` declaration documenting WHY the 100ms is appropriate for client TCP but not internal RPC.

### L-V4-7. `InternalRpcAuth::MAX_AGE_SECONDS = 30` symmetric — see v3-L2; carried forward
A clock-skewed peer can replay up to 60s of envelope window. Loopback-only, low attacker capability, but worth runbook note.

### L-V4-8. `FormatRpcMessage` doesn't escape the JSON body — relies on `nlohmann::json::dump()` correctness
**File:** `Server/Common/src/Net/RpcFraming.hpp:55-58`
The function trusts that `jsonStr` is well-formed JSON with no embedded `\n`. nlohmann's `dump()` escapes newlines as `\n` (literal backslash-n), so this holds. But a caller passing a non-dumped string directly could break the frame. Internal-only code path; documenting because the function signature accepts arbitrary `std::string`.

### L-V4-9. `ServiceEndpoint::Stop` does NOT close in-flight connection sockets — relies on the recv-timeout polling loop
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:106-129`
`Stop()` closes the listen socket but leaves each handler's `clientSocket` alone. Handler exits on next recv timeout (5s) when it sees `m_running == false`. This is intentional (avoids forcing a torn write mid-response) but means shutdown latency is at least 5s even for an idle endpoint. Acceptable trade-off.

### L-V4-10. Probe's `Unreachable` return after retry exhaustion logs nothing distinguishable from a normal Unreachable
**File:** `Server/Common/src/Net/ServiceClient.hpp:280`
A startup probe that retries 5× and gives up logs the same `LOG_NET_DEBUG` lines as a single transient Unreachable. The caller (ProbeStartupPeers in AuthServer/AccountServer/CombatServer) logs its own `LOG_SERVER_CRITICAL` so the operator sees the eventual fatal, but the per-retry trail is harder to grep. Cosmetic.

### L-V4-11. The `mac` field in the envelope accepts hex strings of any length — only HexEquals on the full-length expected MAC catches mismatches
Constant-time comparison only matters when both inputs are equal length. Today `Crypto::HexEquals` short-circuits on length-mismatch. A 0-length received mac vs a 64-hex expected mac early-outs in microseconds. Acceptable (the early-out leaks "wrong length" but not which bytes differ); flagging for completeness.

### L-V4-12. `Ping` handler returns `NowSeconds()` — exposes server clock to any peer with the shared secret
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:40-45`
Loopback-only + secret-gated, so the attacker who can call Ping already has the secret and could trivially forge their own timestamps. No new information disclosure.

### L-V4-13. `m_activeConnections` uses `memory_order_relaxed` on fetch_add but `acq_rel` on fetch_sub
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:95,98`
Asymmetric. The relaxed add followed by acq_rel sub means a thread observing the count post-decrement sees a value that may not include the corresponding increment. In practice the wait_for/notify_one mechanism relies on the count reaching 0, and the final decrement does the notify under the load. The CV wait predicate uses `m_activeConnections.load(acquire)`. Verified to work in the common path because Stop only observes the count under the CV's lock; the lock provides the cross-thread ordering. Cosmetic.

### L-V4-14. `ConnectSocket` doesn't set `SO_KEEPALIVE` on the client-side socket either
Same as M-V4-8 but for the outbound ServiceClient socket. ServiceClient's reconnect-on-fail logic mitigates this (a dropped connection triggers reconnect on next Call). Less urgent than the server side.

### L-V4-15. The "two backoff state machines" gap — `ServiceClient::Probe` retries N times with delayMs, but ServiceClient's exponential backoff (`m_backoffMs`) is also progressing
**File:** `Server/Common/src/Net/ServiceClient.hpp:258-294, 311-325`
During Probe's 5 retries, every Unreachable Call advances `m_backoffMs` via `RecordFailure`. After 5 failures `m_backoffMs = min(1000*2^5, 30000) = 30000`. So after the Probe returns Unreachable, the very next non-probe Call from the same ServiceClient enters a 30s backoff window — Probe's failure poisons the future fast-fail timer. Today's startup ordering means this is OK (Probe failure ⇒ Stop() called ⇒ no subsequent Calls). But if a future refactor allows Probe failure to be non-fatal in Release (the Debug branch already is), the production backoff becomes 30s for the first half-minute of service uptime. Worth a comment.

---

## Verified Closed from v3 (networking-specific)

| v3 item | v3 dimension | Status | Evidence |
|---|---|---|---|
| H-V3-7 (detached threads outlive AccountCache) | Concurrency | ✅ Closed (narrowed; see H-V4-2 for residual) | Explicit `~AccountServer/~AuthServer/~CombatServer` calling `Stop()` first; verified at `AccountServer.hpp:126`, `AuthServer.hpp:56`, `CombatServer.hpp:37` |
| H-V3-11 (LruCache `Get` touches on rejected probes) | Security | ✅ Closed | `LruCache.hpp` Peek (mutable + const) + Touch split; `RateLimiter.hpp:77` uses Peek; explicit Touch only on accept branches L96/L104/L118 |
| H-V3-14(a) (envelope version + MAC over v) | Cross-cutting | ✅ Closed | `APHELYON_ENVELOPE_VERSION` constant `InternalRpcAuth.hpp:53`; MAC input includes `v` `InternalRpcAuth.hpp:106-115`; golden file `EnvelopeShapeTest.cpp` |
| H-V3-14(b) (Probe + Ping + Debug-warn / Release-fatal) | Cross-cutting | ⚠️ Partial — see C-V4-1 | Ping auto-registered at `ServiceEndpoint.hpp:40-45`; Probe at `ServiceClient.hpp:258`; ProbeStartupPeers in all 3 servers. **Classification of AuthFailed / EnvelopeVersionMismatch is dead code.** |
| M-V3-4 (`ServiceClient::Call` collapses failure modes) | Cross-cutting | ✅ Closed (substantially) | `RpcCallResult` with `Success/Unreachable/WireError`; SessionCache routes WireError to fail-closed (`SessionCache.hpp:125-135`). **But SessionCache still treats Success as authoritative without checking for `error` field** — see M-V4-9 |
| M-V3-1 cross-cutting (event_type unconstrained) | Cross-cutting | Out of scope (DB, not networking) | — |
| V3-H1 concurrency (detached thread / 10s drain hazard) | Concurrency | ⚠️ Partial — same as H-V3-7 | Window narrowed by explicit destructors; 10s bound on drain is still there |
| V3-H2 security (LruCache spray pivot) | Security | ✅ Closed | Same fix as H-V3-11 |
| L-V3-2 (outer parse-failure surfaces method="") | Cross-cutting | ✅ Closed | `ServiceEndpoint.hpp:239-254` hex-prefix logging |

---

## Suggested triage order

**Today / immediate (before next deploy):**
1. **C-V4-1** — Probe classification fix (~20 LOC in `ServiceClient::Probe` + a test). The H-V3-14(b) headline only works once this lands.
2. **M-V4-9** — SessionCache should distinguish RPC-level errors from session-invalid. Pairs with C-V4-1 — same fix shape.

**This week:**
3. **H-V4-1** — Bump `ServiceClient` recv timeout to 30s (or make configurable). 1 LOC.
4. **H-V4-3** — Per-IP and total connection caps in both accept loops. ~50 LOC.
5. **H-V4-2** — Unbounded drain (or join-not-detach) in `ServiceEndpoint::Stop`. ~15 LOC.

**Before launch (operational):**
6. **M-V4-3** — Non-blocking connect with timeout. ~30 LOC.
7. **M-V4-8** — TCP keepalive on accepted client sockets. ~15 LOC.
8. **M-V4-4** — Parser buffer compaction / shrink. ~10 LOC.

**Pre-launch quality bar:**
9. M-V4-5 (length-prefix strict parse), M-V4-6 (parser flush on Send fail), L-V4 items, runbook entries for L-V4-7 / L-V4-15 (clock skew + backoff poison).

---

## Test coverage gap (networking-specific)

Recommended additions to CommonTests (already wired in commit `492255a`):
1. **ProbeOutcome classification test** — boot a stub ServiceEndpoint that returns `{"error":"unsupported_envelope_version"}`, call Probe, assert `EnvelopeVersionMismatch`. Same shape for AuthFailed and UnknownError. Closes the gap C-V4-1 exposes.
2. **Wire-level round-trip test** — boot a real ServiceEndpoint, call a method via real ServiceClient, assert success and payload bytes. Currently only the MAC layer is unit-tested; the framing + envelope + dispatch chain has zero end-to-end coverage.
3. **MessageParser fuzz** — feed `ExtractLengthFramed` 100k random byte strings, assert no exception escapes, no infinite loop, no buffer overrun. Property-test shape via rapidcheck.
4. **Connection-cap test** — assert that opening MAX_TOTAL+1 connections to TcpServerBase yields a graceful reject on the +1, not OOM (depends on H-V4-3 landing first).
5. **Drain-timeout regression** — a handler that sleeps 11s; assert that `Stop()` either waits for it (unbounded fix) or that the explicit destructor doesn't UAF (current narrowed window).

---

## System-level Verdict (networking dimension only)

**B−.** The framing layer is correct. The envelope layer is correct now that H-V3-14(a) landed. The destructor lifetime is mostly correct after H-V3-7. But:
- The startup canary (H-V3-14(b)) is structurally broken on its two key failure modes (C-V4-1).
- There is no DoS defense at the accept layer (H-V4-3).
- The 5s recv timeout is shorter than the longest legitimate handler runtime in Debug (H-V4-1).
- The detached-thread / 10s drain hazard is narrower than v3 but not closed (H-V4-2).

These are pre-launch-bounded but ship-blocking for a public-facing service. The cluster fixes for C-V4-1 + H-V4-1 + H-V4-3 + H-V4-2 is the recommended scope for the v4 networking remediation arc.
