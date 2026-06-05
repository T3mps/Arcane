# V5 Follow-up Audit: Networking + Service Lifetime

**Date:** 2026-06-03 (v5, same day as the v4 remediation arc)
**Dimension:** Networking + service lifetime (split out in v4)
**Scope:** `Server/Common/src/Net/*` (TcpServerBase, TcpSocket, ServiceClient, ServiceEndpoint, InternalRpcAuth, RpcFraming, Protocol, RateLimiter, MessageDispatcher, SessionCache) plus the three TcpServerBase subclasses (AuthServer, AccountServer, CombatServer) — service-lifetime aspects: ctor/dtor ordering, OnStarted / OnStopped / OnBeforeShutdown sequence, ProbeStartupPeers Release-fatal `Stop()` branch.
**Method:** Read v4 synthesis + v4 networking follow-up + v3 distribution (concurrency / security / cross-cutting); verified each v4 networking remediation commit (`25a81c1`, `534233c`, `992fd06`, `388c0dd`, `d8fd1c7`) against current HEAD; walked the Probe / SessionCache classification chain end-to-end; sanity-checked the H-V4-10 cap path for leak / log-disclosure; chased the Release-fatal `Stop()`-from-inside-`OnStarted()` flow through `Start()`.

---

## Verdict

**Concerning.** Five of the v4 networking items are verifiably closed in code (M-V4-5 stoull-strict, M-V4-6 parser-clear on Send fail, M-V4-9 SessionCache envelope-error classify, H-V4-10 connection caps, H-V4-4 Debug timeout split, M-V4-4 widened probe budget, C-V4-2 Probe classification). The big-shape headlines hold up. But the v4 cluster shipped one new **High-class** defect with the same flavor as v4's C-V4-1 ("class is correct, integration is missing") — only this time at the lifecycle seam: **`Start()` returns `true` after the Release-fatal `ProbeStartupPeers` calls `Stop()` from inside `OnStarted()`, and then spawns a cleanup thread that is never joined**. The destructor of `m_cleanupThread` in `~TcpServerBase` runs in the joinable state with a finished OS thread, and `std::thread`'s destructor calls `std::terminate`. The v4 audit's M-V4-4 cross-cutting flagged the "Release-fatal turns probe race into a service crash" pattern but only the budget side was widened; the lifecycle ordering inside `Start()` was not. The detached-thread / 10s drain hazard from H-V4-2 carries forward unchanged. M-V4-3 (no connect timeout), M-V4-8 (no keepalive), and the M-V4-1 (Probe holds mutex) family are all unchanged. Three peer-ServiceClient.Call sites in `AuthServer` (`HandleLogin`, `HandleRegister`) still don't do the M-V4-9 envelope-error classification that `SessionCache::Validate` got — same-shape defect, three more occurrences.

---

## CRITICAL

(None this pass.)

---

## HIGH

### H-V5-1. `Stop()`-from-`OnStarted()` (Release-fatal probe branch) leaks an unjoined cleanup thread → `~TcpServerBase` terminates the process
**Files:**
- `Server/Common/src/Net/TcpServerBase.hpp:80-91` (`Start()`: m_running=true → m_internalEndpoint.Start → OnStarted → unconditionally spawns m_cleanupThread at L91)
- `Server/Common/src/Net/TcpServerBase.hpp:154-202` (`Stop()`: compare_exchange + join m_cleanupThread only if joinable; idempotent after first call)
- `Server/Account/src/AccountServer.hpp:182-201` (ProbeStartupPeers, Release-fatal `Stop()` at L197)
- `Server/Auth/src/AuthServer.hpp:110-128` (same shape, L124)
- `Server/Combat/src/CombatServer.hpp:73-91` (same shape, L87)
**Flagged by:** Service-lifetime sequencing trace (v5 focus area #10)
**Status:** NEW — introduced by the v3 H-V3-14(b) commit `bfbcf2c`, widened-budget but not fixed by v4 M-V4-4 / `d8fd1c7`.

The intended flow is: `Start()` sets `m_running=true` → starts internal endpoint → calls virtual `OnStarted()` → spawns cleanup thread → enters accept loop. In the Release-fatal probe failure case the override `OnStarted()` calls `ProbeStartupPeers` → `m_authClient.Probe(...)` returns non-Ok → `LOG_SERVER_CRITICAL` → `Stop()`.

At that point in the call stack:
- `Stop()` at L157 `compare_exchange_strong(true → false)` succeeds.
- `m_internalEndpoint.Stop()` at L162 drains the internal endpoint.
- `m_serverSocket` closed at L165-169.
- L172-182 walks `m_clients` (empty — accept loop hasn't run).
- L184 `m_cleanupCv.notify_all()` (no-op — no waiter).
- L186-187 `if (m_cleanupThread.joinable())` — **false**, because `m_cleanupThread` is a default-constructed `std::thread` (line 585 default member-init); not yet started.
- L189-198 walks empty `m_clients`, clears `m_clientsByIp`.
- `OnStopped()` runs (AccountServer's saves accounts, all safe to call here).
- `CleanupSockets()` runs.

Then control returns to `Start()` at L90, runs through L91:
```cpp
m_cleanupThread = std::thread(&TcpServerBase::CleanupThread, this);
```
**The cleanup thread is now spawned even though we just called `Stop()`.** It immediately observes `m_running.load() == false` at L536, falls out of the `while` loop, and exits cleanly. But the `std::thread` object is now joinable with a finished OS thread.

`Start()` then enters its accept-loop at L93. `m_running == false`, loop body never runs, `Start()` returns `true` at L150 — operator sees no failure indication.

Now the process is in this state:
- `m_running == false`
- `m_cleanupThread` is joinable but the OS thread is gone
- No further `Stop()` will join it: the compare_exchange at L157 fails on the second call (m_running already false), so L186-187 never runs again.

Eventually `~AccountServer()` (or whatever derived class) runs. It calls `Stop()` — short-circuits at L158. `~TcpServerBase()` calls `Stop()` again — short-circuits. Then `~TcpServerBase` proceeds to destruct member-by-member; `m_cleanupThread` destructs in the joinable state. **`std::thread`'s destructor calls `std::terminate`.** The process aborts with no useful log.

The bug exists for any path that calls `Stop()` from inside `OnStarted()`, but production code in three TcpServerBase subclasses does exactly that on Release-fatal probe failure. Trigger condition: any of (a) sibling service down for >30s of the widened-by-M-V4-4 probe budget, (b) `APHELYON_INTERNAL_SECRET` mismatched between two services in Release, (c) a future operator running with `APHELYON_ENVELOPE_VERSION` skew.

**Fix.** Three reasonable shapes:
1. **Hoist the cleanup thread above `OnStarted()`** — easiest, but means cleanup thread runs against an empty `m_clients` for a moment before any work exists, which is harmless (just a 5-second `wait_for` cycle).
2. **Re-check `m_running` after `OnStarted()` returns** — `if (!m_running.load()) return false;` before L91. Communicates failure to the operator by Start()'s return value too, which today is silently lost.
3. **Make `Stop()` join the cleanup thread idempotently** — track a separate `m_cleanupThreadStarted` flag; if set after the first `Stop()`, the second `Stop()` still joins. Larger refactor.

Recommend (1)+(2): hoist cleanup thread spawn above `OnStarted()`, and return false from `Start()` when `OnStarted` flipped m_running. Three-line change at TcpServerBase.hpp:88-91.

```cpp
m_cleanupThread = std::thread(&TcpServerBase::CleanupThread, this);
m_internalEndpoint.Start();
OnStarted();
if (!m_running.load(std::memory_order_acquire)) return false;
```

### H-V5-2. `ServiceEndpoint::Stop()` 10s drain remains bounded; detached threads still survive overrun
**Files:**
- `Server/Common/src/Net/ServiceEndpoint.hpp:116-121` (detached thread spawn, L121)
- `Server/Common/src/Net/ServiceEndpoint.hpp:143-148` (`wait_for(10s)`)
**Source:** v4 H-V4-2 carry-forward
**Status:** OPEN — not addressed in v4 remediation arc.

The v4 audit flagged this as a window narrowed by H-V3-7 explicit destructors but not closed. The v4 remediation arc didn't touch it. With H-V5-1 above, a handler running >10s during a Stop()-from-OnStarted() scenario opens a UAF window: cleanup proceeds, derived members destruct, the detached connection thread keeps running with a captured `this` pointer to destroyed `InternalRpcHandlers`/`SetupInternalEndpoint` lambda state.

The narrowed-window analysis from v4 still holds — today's handlers are unlikely to run >10s — but with the H-V4-4 timeout bump to 30s on the client side, the symmetric server-side handler timeout (`SetSocketTimeout(clientSocket, 5000)` at ServiceEndpoint.hpp:111) was NOT bumped. So a Debug-build Auth-side PBKDF2 of 18s now exceeds the server's recv timeout window, and the handler-runtime upper bound becomes whatever the handler chooses to compute — there's no hard upper bound.

**Fix.** Same options as v4 H-V4-2 (unbounded wait + watchdog; or `join` not `detach`).

### H-V5-3. `AuthServer::HandleLogin` and `HandleRegister` still treat any `rpc.ok()` from Account as authoritative — same defect as M-V4-9 in two more sites
**Files:**
- `Server/Auth/src/AuthServer.hpp:266-297` (HandleRegister calls m_accountClient.Call("CreateAccount", ...))
- `Server/Auth/src/AuthServer.hpp:323-343` (HandleLogin calls m_accountClient.Call("VerifyCredentials", ...))
**Source:** M-V4-9 same-shape sweep
**Status:** NEW — M-V4-9 caught the SessionCache instance and `388c0dd` fixed it; the two AuthServer sites use the same `rpc.ok()` → `result.value("...", default)` idiom and were left.

```cpp
auto rpc = m_accountClient.Call("VerifyCredentials", ...);
if (!rpc.ok()) { /* WireError or Unreachable */ return ...; }

auto result = rpc.value.value("result", rpc.value);
bool valid = result.value("valid", false);
```

If Account responds with `{"error":"Authentication failed"}` (MAC mismatch) or `{"error":"unsupported_envelope_version"}` (envelope skew):
- `rpc.ok()` is true (round-trip succeeded).
- `result = rpc.value.value("result", rpc.value)` falls through to `rpc.value` because there's no `result` field → result = `{"error":"..."}`.
- `valid = result.value("valid", false)` defaults to `false`.
- Outcome by coincidence: HandleLogin returns "Invalid username or password" / HandleRegister returns "Failed to create account."

Operationally this is the same trap M-V4-9 closed for SessionCache: a wholesale Account-MAC-rejection masquerades as "credentials wrong" to every operator and user, with no log line distinguishing the cause. Login fails for everyone but the dashboard shows "auth failed" not "internal-secret mismatch."

**Fix.** Same shape as M-V4-9 in SessionCache.hpp:148-156 — inspect `rpc.value` for an `error` key before extracting `result`, log loudly, and return a user-facing message that doesn't lie about "wrong password."

```cpp
if (rpc.value.contains("error"))
{
    LOG_AUTH_ERROR("Login: Account RPC returned envelope error: {} (IP={})",
                   rpc.value.value("error", std::string{}), clientIP);
    return CreateAuthResponse(false, "Account service unavailable", "", "", "", 0);
}
auto result = rpc.value.value("result", rpc.value);
```

The mirror change in HandleRegister.

---

## MEDIUM

### M-V5-1. `ServiceEndpoint::HandleConnection` server-side recv timeout still hard-coded 5000 — diverged from H-V4-4's Debug=30s contract on the symmetric client side
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:111` (`SetSocketTimeout(clientSocket, 5000)`)
**Source:** H-V4-4 same-shape sweep

H-V4-4's commit message: "eliminating the magic-number drift surface" — implemented as `kSocketTimeoutMs` on `ServiceClient`. But the server-side timeout in `ServiceEndpoint::Start()`'s connection-accept block at L111 is still the original 5000 literal. So while a client (Auth) now waits up to 30s in Debug for a slow PBKDF2 handler, the server side (Account) still drops the recv at 5s — and the recv-timeout polling at `HandleConnection` L163 just `continue`s on timeout, so the connection survives. Not a functional bug today, but the magic-number drift surface H-V4-4 explicitly aimed to eliminate is half-fixed: the constant is consumed in two ClientCa sites and zero EndpointS sites.

**Fix.** Use the same `kSocketTimeoutMs` constant on the server side, OR document the asymmetry in a comment at L111 (server-side polling timeout intentionally smaller than client-side recv budget because the server checks m_running every cycle).

### M-V5-2. `SessionCache::Validate`'s M-V4-9 fix doesn't distinguish "Unknown method" or "Internal error" from "Authentication failed"
**File:** `Server/Common/src/Net/SessionCache.hpp:148-156`
**Source:** M-V4-9 classification completeness

The M-V4-9 fix at L148 catches any `error` field and logs the literal string, then calls `NoteAuthLost()`. But `NoteAuthLost()` is semantically "Auth service is unreachable" — it sets the state flag that triggers H6's auth-down-extension path on subsequent Validate calls. An "Unknown method: AuthorizeToken" error (which would happen on a deployment skew where Auth's binary doesn't yet have the AuthorizeToken handler registered) is NOT a network outage — it's a deployment integrity failure that should be louder. Similarly "Internal error: ..." from a server-side exception is not a network outage.

The current behavior is fail-safe in the practical sense (no successful session validation), but ops monitoring `NoteAuthLost` as a network-outage signal would see spurious "Auth unreachable" alerts during a partial-deploy window.

**Fix.** Classify by error string. `"Authentication failed"` and `"unsupported_envelope_version"` → `NoteAuthLost()` (integrity loss, treat as channel-down). `"Unknown method: ..."` and `"Internal error: ..."` → log at ERROR, don't toggle `NoteAuthLost` (Auth is up but malfunctioning). Or factor out a `ClassifyEnvelopeError(rpc.value)` helper shared between Probe, SessionCache, HandleLogin, HandleRegister.

### M-V5-3. Probe's response-body inspection misses the two non-headline error strings ("Unknown method", "Internal error")
**File:** `Server/Common/src/Net/ServiceClient.hpp:294-305`
**Source:** C-V4-2 classification completeness

The fix in `25a81c1` correctly routes `unsupported_envelope_version` and `Authentication failed`. Any other non-empty `error` (the two strings emitted by `ServiceEndpoint::HandleConnection`: `"Unknown method: <method>"` at L246 and `"Internal error: <e.what()>"` at L279) goes to `UnknownError`. That's fine for the Ping path (Ping is auto-registered, won't hit Unknown method; Ping's handler doesn't throw, won't hit Internal error) but the comment at ServiceClient.hpp:297-299 names "the source-of-truth line numbers in ServiceEndpoint.hpp" while only covering two of the four. A future Probe call to a non-Ping method (e.g., a "GetServiceInfo" probe expansion) would silently degrade to `UnknownError` for both unknown-method and internal-error responses — losing the deployment-integrity vs. handler-panic distinction.

**Fix.** Extend the comment to enumerate all four error strings and explain why Probe only acts on two. Or, if a future Probe targets non-Ping methods, add a hex-prefix log line of the raw error text before returning UnknownError.

### M-V5-4. `ServiceEndpoint::InvokeForTest` is a public method with no compile-time gate — only a doc comment keeps it from production callers
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:66-77`
**Source:** v5 focus question #9 (test backdoor leak)

The comment at L59-65 says "test-only direct dispatch... Production paths never call this." Verified: today's grep shows only `Server/Account/tests/Integration/InternalRpcHandlersTest.cpp` calls it. But the method is `public`, has no `#ifndef NDEBUG` gate, no friend-class restriction, no naming convention beyond `ForTest` suffix. A handler refactor that needs to "directly invoke the in-process handler bypassing the wire" could land in production source and pass review without lighting up anything except a careful reader of the doc comment.

The risk: `InvokeForTest` bypasses MAC verification + envelope-version check + ts replay window. If a future production caller invokes a registered handler via this path believing they're saving a serialize round-trip, every security control on the internal-RPC channel is bypassed for that one call.

**Fix.** One of:
1. `#ifndef NDEBUG` gate so it doesn't even exist in Release builds.
2. Make `InvokeForTest` private and friend the test class (requires forward-declaring the test class in a Release-friendly way).
3. Rename to `UnsafeDirectInvokeForTestsOnly` so a production caller is obviously self-incriminating.

Recommend (1). The integration tests already require Debug-build collaborators (the test fixture, ConnectionPool, etc.); they don't need this method in Release binaries.

### M-V5-5. `Start()` returns `true` on Release-fatal probe failure → operator and supervisor process can't observe the failure via return code
**File:** `Server/Common/src/Net/TcpServerBase.hpp:80-150`
**Source:** Service-lifetime sequencing trace (related to H-V5-1)

Even ignoring H-V5-1's cleanup-thread leak, `Start()`'s return value carries no signal that `ProbeStartupPeers` flipped m_running. Operator code in `main.cpp` ([typical pattern](Server/Account/src/main.cpp)) reads `Start()`'s return code to decide whether to log "started" or "failed to start". On Release-fatal probe failure, the operator sees:
- LOG_SERVER_CRITICAL line ("Account startup probe of Auth FAILED")
- Start() returns true
- main() proceeds as if normal

The LOG_SERVER_CRITICAL is correct but the return-code lie obscures the failure for any external supervisor that watches exit codes (systemd, Docker healthcheck, k8s liveness probe). Today the process exits via H-V5-1's std::terminate, which Docker sees as exit code 134 (SIGABRT) — but that's an unhealthy way to communicate "probe failed."

**Fix.** Same as H-V5-1 option (2): return `false` from `Start()` if `m_running` is false after `OnStarted()`. main.cpp then exits non-zero on its own.

### M-V5-6. `m_clientsByIp` is correctly bounded to active IPs but the constant `MAX_CONNECTIONS_PER_IP=16` collides with shared NAT egress
**File:** `Server/Common/src/Net/TcpSocket.hpp:43-49`
**Source:** H-V4-10 design review

The cap is 16 concurrent connections per IP. In the small-DAU launch window this is fine, but it conflicts with:
- A shared university or office NAT where dozens of legit users share one egress IP.
- Mobile carriers' CGNAT (Carrier-Grade NAT) where thousands of users can share one IP.

The current behavior rejects the 17th connection with `LOG_NET_WARN("Accept refused: per-IP cap 16 reached for <IP>")`. A legit shared-NAT user sees a confusing connection failure that's indistinguishable from any other "can't connect."

**Fix options:**
1. Document 16 as the pre-launch operational default; raise to 64 or 128 at scale and revisit per region/CGNAT analysis post-launch.
2. Make `MAX_CONNECTIONS_PER_IP` configurable via `protocol.json`'s settings block, like `max_message_size` already is.
3. Trust an X-Forwarded-For header from an upstream LB (out of scope for current architecture; would require a TLS-terminating ingress).

Recommend (1) for now — file a launch-readiness item with the cap-tuning rationale. The current 16 is more about slow-loris defense than legitimate scaling. Document.

### M-V5-7. `MAX_CONNECTIONS_TOTAL=2048` cap does not apply to `ServiceEndpoint`'s loopback accept loop
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:90-122`
**Source:** H-V4-10 scope review

The v4 H-V4-10 fix correctly bounded the public-facing `TcpServerBase` accept loop. But the internal `ServiceEndpoint` is still uncapped — it spawns one detached thread per accepted connection with no per-IP or total ceiling. The v4 followup noted the loopback-only mitigation: "practical attack surface is 'local process with no internal-RPC secret'." That's still true. But a Debug-build flow where a developer runs a tool that connects-and-drops to 127.0.0.1:<internal_port> in a tight loop will spawn unbounded threads, each holding ~1.1MB of stack.

**Fix.** Apply the same per-IP-or-total cap pattern to `ServiceEndpoint::HandleConnection`'s accept loop. The per-IP map is degenerate (every IP is 127.0.0.1), so a single counter on `m_activeConnections` with a constant `MAX_INTERNAL_CONNECTIONS=64` is sufficient. Or document this as accepted risk given the loopback-only threat model.

### M-V5-8. `m_clientsByIp` not measured via `LOG_NET_TRACE` cleanup tick — silent monotonic counter drift would go unnoticed
**File:** `Server/Common/src/Net/TcpServerBase.hpp:578-581`
**Source:** H-V4-10 observability

A cleanup-thread bug or a missed disconnect path (today: there's none I can see) would leave `m_clientsByIp[ip]` permanently above zero. The H-V4-10 implementation correctly erases entries at zero, but there's no periodic invariant check that `sum(m_clientsByIp values) == m_clients.size()`. A debug-build assertion at the end of CleanupThread's tick (`assert(sum == m_clients.size())`) would catch a counter-leak the day it lands.

**Fix.** Add a debug-only invariant check at the end of CleanupThread iteration. Optional; the leak surface is small.

### M-V5-9. M-V4-3 carry-forward: `ConnectSocket` has no connect-timeout — peer's saturated accept queue blocks ServiceClient::Call for 21s on Windows
**File:** `Server/Common/src/Net/TcpSocket.hpp:163-181`
**Source:** v4 M-V4-3 carry-forward
**Status:** OPEN — unchanged in v4 remediation arc.

The v4 followup at M-V4-3 enumerated the fix. Today's behavior unchanged: `connect()` blocks until OS-level connect timeout (Windows ~21s). During this window the calling thread holds `m_mutex`. With H-V4-4's Debug bump to `kSocketTimeoutMs=30000`, the worst-case Debug Call() now blocks for 21s connect + 30s recv = 51s while holding the mutex — over a minute of ServiceClient unavailable to concurrent callers.

**Fix.** As v4: non-blocking connect + `select` with timeout.

### M-V5-10. M-V4-8 carry-forward: still no TCP keepalive on accepted client sockets
**File:** `Server/Common/src/Net/TcpServerBase.hpp:142` (only RCVTIMEO set), `Server/Common/src/Net/TcpSocket.hpp:125-160` (CreateListenSocket sets REUSEADDR only)
**Source:** v4 M-V4-8 carry-forward
**Status:** OPEN — unchanged in v4 remediation arc.

With H-V4-10's connection cap at 2048 total and 16 per-IP, half-open connections from crashed clients still survive up to 2 hours (Windows default keepalive idle). A client crash that doesn't send RST can hold a connection-cap slot for that duration. Compounds with the per-IP cap on shared NAT: 16 stale half-open connections from one CGNAT IP shut out new legit users for hours.

**Fix.** As v4: `SO_KEEPALIVE` + `SIO_KEEPALIVE_VALS` on Windows.

---

## LOW / OBSERVATION

### L-V5-1. Probe-failure-poisons-backoff (v4 L-V4-15 carry-forward)
**File:** `Server/Common/src/Net/ServiceClient.hpp:289-325`
The 15-retry Probe now drives `m_backoffMs` to the 30000 cap after a full retry exhaustion. Today only the Release-fatal Stop() consumes that state. If a future "graceful degradation on probe-fail in Release" pattern lands, the first non-probe `Call` for the next 30s will fast-fail Unreachable. Document near the Probe definition.

### L-V5-2. v4 L-V4-7 carry-forward: InternalRpcAuth `MAX_AGE_SECONDS = 30` symmetric replay window
**File:** `Server/Common/src/Net/InternalRpcAuth.hpp:58`
Clock-skewed peer can replay up to 60s of envelope. Runbook note.

### L-V5-3. M-V4-1 carry-forward: Probe holds `m_mutex` for the entire retry budget
**File:** `Server/Common/src/Net/ServiceClient.hpp:289-325`
With M-V4-4's widened budget (15×2s=30s minimum), the practical mutex-hold lengthens. Still single-threaded at startup so harmless today. Document.

### L-V5-4. v4 L-V4-3 carry-forward: `IsAvailable()` doesn't reset `m_backoffMs` after window elapses
**File:** `Server/Common/src/Net/ServiceClient.hpp:128-137`
Tiny overhead; cosmetic.

### L-V5-5. v4 L-V4-4 carry-forward: SessionCache `m_cache` map has no max-size cap
**File:** `Server/Common/src/Net/SessionCache.hpp:267`
Unbounded growth at scale.

### L-V5-6. v4 L-V4-5 carry-forward closed by `25a81c1`'s comment update
**File:** `Server/Common/src/Net/ServiceClient.hpp:280-292`
The Probe doc-comment now correctly states what the implementation does. **CLOSED.**

### L-V5-7. v4 L-V4-6 carry-forward: timeout asymmetry between ServiceClient (Debug=30s/Release=5s), ServiceEndpoint (5s hard-coded), TcpServerBase (100ms)
**Files:** `ServiceClient.hpp:69-72`, `ServiceEndpoint.hpp:111`, `TcpSocket.hpp:41`
Worth a comment block in `TcpSocket.hpp` documenting the intent of each timeout's value. See M-V5-1.

### L-V5-8. v4 L-V4-8 carry-forward: `FormatRpcMessage` trusts well-formed JSON from caller
**File:** `Server/Common/src/Net/RpcFraming.hpp:55-58`
Internal-only path, no caller injects raw strings.

### L-V5-9. v4 L-V4-9 carry-forward: ServiceEndpoint::Stop doesn't close in-flight connection sockets, relies on 5s recv-timeout polling
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:126-149`
Intentional trade-off; documented.

### L-V5-10. v4 L-V4-10 carry-forward: Probe's per-retry log doesn't distinguish "first retry" from "final retry"
**File:** `Server/Common/src/Net/ServiceClient.hpp:311-312`
Cosmetic. The widened 15-retry budget makes this noisier.

### L-V5-11. v4 L-V4-11 closed via `d65dbdf` commit (HexEquals length-mismatch documented)
**Status:** Verified closed.

### L-V5-12. v4 L-V4-13 carry-forward: `m_activeConnections` memory-order asymmetry on fetch_add/fetch_sub
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:115,118`
Cosmetic; works due to CV lock providing cross-thread ordering at the only consumer.

### L-V5-13. v4 L-V4-14 carry-forward: outbound `ConnectSocket` doesn't set SO_KEEPALIVE either
**File:** `Server/Common/src/Net/TcpSocket.hpp:163-181`
Lower urgency — reconnect logic mitigates.

### L-V5-14. v4 L-V4-1 carry-forward: RpcParser and MessageParser duplicated framing logic
**Files:** `RpcFraming.hpp:15-52`, `TcpServerBase.hpp:292-344`
Both now correctly use the shared `ExtractLengthFramed` helper (which M-V4-5 hardened). The remaining duplication is cosmetic.

### L-V5-15. M-V4-5's stoull-strict fix correctly catches the partial-parse case; symmetric path in `MessageParser::TryExtractMessage` goes through the same `ExtractLengthFramed` and inherits the fix
**File:** `Server/Common/src/Net/TcpSocket.hpp:213-224`
Verified: both `RpcParser::TryExtract` (RpcFraming.hpp:34-46) and `MessageParser::TryExtractMessage` (TcpServerBase.hpp:311-337) delegate to the same `ExtractLengthFramed`. **No symmetric site missed.** The fix is single-point of correction.

### L-V5-16. M-V4-6's parser-clear placement at the Send-failure site is correct
**File:** `Server/Common/src/Net/ServiceClient.hpp:201-214`
Symmetric receive-side failure paths (L222-244) all close the socket and return WireError — the parser is local to `Call()` and isn't re-used between calls except via the `m_parser.Clear()` reset at L170 / L218. **No symmetric site missed.**

### L-V5-17. Cap-refusal log only fires `LOG_NET_WARN`, not `Logger::LogConnectionEvent` — refused connects don't appear in the connection event stream
**File:** `Server/Common/src/Net/TcpServerBase.hpp:122-138`
Cosmetic asymmetry. If ops greps the connection event log for IP X's pattern, the cap-refusal won't show. The WARN log is sufficient for immediate ops signal.

### L-V5-18. H-V4-4's `kSocketTimeoutMs` constant correctly consumed at both Connect() and Call() reconnect sites
**File:** `Server/Common/src/Net/ServiceClient.hpp:103, 169`
Verified — both sites use the constant. No drift surface in ClientCa side. **CLOSED.**

### L-V5-19. M-V4-4 widened probe budget consistently across all three call sites
**Files:** `AccountServer.hpp:188`, `AuthServer.hpp:115`, `CombatServer.hpp:78`
All three call sites use `retries=15, delayMs=2000`. Verified consistent.

### L-V5-20. H-V4-10's cap-refusal log lines safe to expose to a public attacker — only the cap value (public constant) and the attacker's own IP appear
**File:** `Server/Common/src/Net/TcpServerBase.hpp:123-124, 131-132`
No internal state leak. Attacker sees what they sent. Safe.

### L-V5-21. v4 introduced `ServiceEndpoint::InvokeForTest` (H-V4-5 test enablement) — flagged in M-V5-4 above for gating
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:66-77`
See M-V5-4.

---

## Verified Closed from v4 (networking-specific)

| v4 item | Status | Evidence |
|---|---|---|
| C-V4-2 ServiceClient::Probe inspects response body | ✅ Closed | `ServiceClient.hpp:294-305` routes the two known error strings, treats other non-empty error as UnknownError. Commit `25a81c1`. See M-V5-3 for completeness note. |
| H-V4-4 Debug-only longer ServiceClient timeout | ✅ Closed | `kSocketTimeoutMs` constant defined `ServiceClient.hpp:68-72`, used at L103 (Connect) and L169 (Call reconnect). Both sites verified. Commit `992fd06`. |
| H-V4-10 Connection caps on TcpServerBase accept loop | ✅ Closed | `MAX_CONNECTIONS_TOTAL=2048` and `MAX_CONNECTIONS_PER_IP=16` at `TcpSocket.hpp:48-49`. Accept loop checks both at `TcpServerBase.hpp:121-138`. Cleanup decrements + erase-when-zero at L556-561. Stop() clears at L197. No leak path. Log safe. Commit `534233c`. See M-V5-7 for the ServiceEndpoint gap. |
| M-V4-4 Probe budget widen | ✅ Closed | All three sites use `retries=15, delayMs=2000`. Verified consistent. Commit `d8fd1c7`. |
| M-V4-5 ExtractLengthFramed strict parse | ✅ Closed | `TcpSocket.hpp:220-224` captures `parsedChars` via `stoull`'s out-param, requires equality with `colonPos`. Inherited by both RpcParser and MessageParser. Commit `388c0dd`. |
| M-V4-6 ServiceClient parser-clear on Send fail | ✅ Closed | `ServiceClient.hpp:212` `m_parser.Clear()` immediately after `CloseSocket` on Send fail. Symmetric recv paths confirmed clean. Commit `388c0dd`. |
| M-V4-9 SessionCache envelope-error classify | ✅ Closed for SessionCache; **same shape OPEN in AuthServer::HandleLogin and ::HandleRegister** — see H-V5-3. |
| H-V4-1/2/3/4/5/6/7/8/9 + M/L items outside networking dimension | Out of scope here |

### v4 networking items NOT addressed in v4 remediation arc (carry-forward to v5)
| v4 item | v5 status | v5 ref |
|---|---|---|
| H-V4-2 ServiceEndpoint::Stop 10s drain bounded; detached threads survive | Still OPEN | H-V5-2 |
| M-V4-1 Probe holds m_mutex through retries | Still OPEN | L-V5-3 |
| M-V4-2 Probe sleep-outside-lock semantics | Acceptable as-is | (no v5 item) |
| M-V4-3 No connect timeout on ConnectSocket | Still OPEN | M-V5-9 |
| M-V4-4 buffer compaction / shrink_to_fit (NOTE: same M-V4-4 number used in v4 networking for buffer; cross-cutting M-V4-4 was the probe budget — distinct) | Still OPEN | (carries forward unchanged) |
| M-V4-7 m_questTokenSecret init-order documentation | Out of scope (Account-specific) | — |
| M-V4-8 No TCP keepalive | Still OPEN | M-V5-10 |
| M-V4-10 Detached connection thread + lambda capture UAF | Same window as H-V5-2 | H-V5-2 |
| L-V4-* items | Mix of closed (L-V4-5/11) and carry-forward | L-V5-* |

---

## v3 distribution checks

v3-concurrency `H-V3-7` (explicit destructors) holds for all three servers (`AccountServer.hpp:138`, `AuthServer.hpp:56`, `CombatServer.hpp:37`). v3-security `H-V3-11` (LruCache Peek/Touch) holds at `RateLimiter.hpp:77`. v3-cross-cutting `H-V3-14(a)` envelope-version holds at `InternalRpcAuth.hpp:53`; `H-V3-14(b)` Probe + Ping verified at `ServiceEndpoint.hpp:40-45` and `ServiceClient.hpp:289`; classification was closed by v4 C-V4-2.

---

## Test coverage (networking-specific)

**Still no regression test for the Probe classification fix (C-V4-2 / `25a81c1`).** The v4 networking followup recommended this as the first test-coverage gap and it remains unwritten. A stub `ServiceEndpoint` that returns `{"error":"unsupported_envelope_version"}` and asserts `Probe()` returns `EnvelopeVersionMismatch` would close the gap. Existing tests: `InternalRpcAuthRoundTripTest`, `EnvelopeShapeTest`, `RateLimiterSmokeTest`, `test_main.cpp`. No connection-cap test, no drain-timeout regression test.

Adding `ProbeOutcomeClassificationTest.cpp` to CommonTests would be ~150 LOC and would have caught H-V5-1's `Start()`-returns-true bug as a side-effect of an end-to-end "Start succeeds, then Stop succeeds, then second Stop is a no-op, then destructor doesn't terminate" assertion.

---

## Suggested triage order

**Today / immediate (before next deploy):**
1. **H-V5-1** — Hoist `m_cleanupThread = std::thread(...)` above `OnStarted()` in TcpServerBase::Start, and re-check `m_running` after OnStarted returns. ~3 LOC. Without this fix, every Release-fatal probe failure crashes the process with `std::terminate`.
2. **H-V5-3** — Apply M-V4-9's classification fix to AuthServer::HandleLogin and HandleRegister. ~12 LOC across two functions.
3. **H-V5-2** — Unbounded drain (or join-not-detach) on ServiceEndpoint::Stop. v4 carry-forward.

**This week:**
4. **M-V5-1** — Use kSocketTimeoutMs on ServiceEndpoint server-side too, OR document the asymmetry.
5. **M-V5-2** — Factor a `ClassifyEnvelopeError(json)` helper shared between Probe, SessionCache, HandleLogin, HandleRegister (closes H-V5-3 + M-V5-2 + M-V5-3 cleanly).
6. **M-V5-4** — Gate `InvokeForTest` behind `#ifndef NDEBUG`.
7. **M-V5-5** — Return false from Start() on Release-fatal probe (companion to H-V5-1).

**Before launch (operational):**
8. **M-V5-9** — non-blocking connect with timeout (v4 carry-forward).
9. **M-V5-10** — TCP keepalive (v4 carry-forward).
10. **M-V5-6** — Document MAX_CONNECTIONS_PER_IP=16 vs CGNAT trade-off; revisit at scale.
11. **M-V5-7** — Apply connection cap to ServiceEndpoint accept loop (or document the loopback acceptance).

**Test backlog:**
12. ProbeOutcome classification test (v4 networking followup recommendation, still unwritten).
13. Start-Stop lifecycle regression (would catch H-V5-1 mechanically).
14. Connection-cap acceptance test (does opening MAX_TOTAL+1 yield a graceful reject?).

---

## System-level Verdict (networking dimension only)

**B−.** v4 closed 7 of 7 networking items it set out to close, all verified in code. But the v4 cluster introduced one new High-class lifecycle defect (H-V5-1) that crashes the process on Release-fatal probe failure — the same path v4's M-V4-4 widened the budget on — and missed three same-shape AuthServer call sites for M-V4-9 (H-V5-3). The carry-forward backlog from v4 networking (H-V4-2 / M-V4-3 / M-V4-8 / M-V4-10) is entirely untouched. The big-shape headlines (probe classification, connection caps, framing strictness) are correct; the cleanup is incomplete in two specific directions.

The cluster fix for H-V5-1 + H-V5-3 + H-V5-2 is the recommended scope for the v5 networking remediation arc. The whole cluster is ~25 LOC across four files.
