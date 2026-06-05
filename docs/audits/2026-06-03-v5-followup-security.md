# V5 Follow-up Audit: Security

**Date:** 2026-06-03 (same-day successor to v4, after the 20-commit v4 remediation arc)
**Dimension:** Security
**Scope:** Verify the v4 remediation arc (commits `f24b3ca..HEAD`, ~20 fix commits) for the security-relevant items and surface any new defects introduced or exposed. Same surface as v3/v4: Auth handlers, SessionManager, SessionCache, InternalRpcAuth, ServiceEndpoint/Client, Crypto primitives, secrets handling, login/resume flow timing, rate-limiter eviction semantics, protocol.json `requires_auth:false` surface, env-var enforcement matrix, token logging, plus the new H-V4-10 connection caps and the C-V4-2 Probe body-inspection.
**Method:** Single security-lens pass over each v4-touched file end-to-end, walk the call graphs the v4 fixes claim to close, and exercise the failure-mode trees on paper.

---

## Verdict

The v4 remediation pass is **substantively clean**. C-V4-2 (Probe body inspection), M-V4-2 (LogSessionEvent on invalidate / cleanup with reason classifier), M-V4-6 (Crypto::ParseStoredHash odd-length guard), M-V4-9 (SessionCache envelope-error classification), H-V4-4 (Debug-only longer ServiceClient timeout), and H-V4-10 (TcpServerBase connection caps) all hold up under direct examination. Every named security item from v4 lands correctly in code: the Probe enum now reaches `AuthFailed` / `EnvelopeVersionMismatch` / `UnknownError` on response-body inspection; `InvalidateByPlayerIdInternal` and `CleanupExpiredInternal` emit structured events with the new optional `reason` field; `ParseStoredHash` rejects odd-length salt/hash hex before the PBKDF2 burn; `SessionCache::Validate` surfaces envelope-errors loudly and flips into the auth-lost state.

Two new **High** items surface, both of the same shape as C-V4-2 — they are the cross-cutting completion of the Probe response-body classification work. `AuthServer::HandleLogin` and `AuthServer::HandleRegister` use `rpc.value.value("result", rpc.value)` to extract the result; on an envelope-error response (secret mismatch, version skew) the fallback collapses the error envelope into a result-object with `success=false` / `valid=false`, producing the correct user-facing outcome but a misdiagnosed ops signal — exactly the misleading-log surface that motivated M-V4-1 ("MAC OK" log line during secret mismatch). One **Medium** sits on the H-V4-10 connection-cap fix: per-IP counter is keyed by the raw `inet_ntop` peer address, which under a future NAT-collapsed / CDN-collapsed deployment would treat thousands of legitimate clients sharing one egress IP as a single attacker. Six Lows are restatements of v4 carry-forwards.

Verdict: **clean** (zero Critical, two High, one Medium, six Low; no broken v4 fixes; the High items below are not regressions of v4 fixes but completion gaps on the same finding shape).

C/H/M/L: **0 / 2 / 1 / 6**

---

## CRITICAL

*(None.)*

The v4 remediation closed both v4 Criticals (C-V4-1 OutboxRelay wiring at commit `f24b3ca`, C-V4-2 Probe response-body inspection at commit `25a81c1`) in adjacent / this dimension. C-V4-2 is verified below in **Verified Closed**. Nothing in the current security surface has Critical impact.

---

## HIGH

### H-V5-1. `HandleLogin` / `HandleRegister` collapse envelope errors into "result" via fallback default

**Files:** `Server/Auth/src/AuthServer.hpp:279` (`HandleRegister`), `Server/Auth/src/AuthServer.hpp:336` (`HandleLogin`)
**Source:** This audit — completion gap on C-V4-2's pattern
**Status:** NEW

`Server/Auth/src/AuthServer.hpp:279`:
```cpp
auto result = rpc.value.value("result", rpc.value);
bool success = result.value("success", false);
std::string error = result.value("error", "");
```

`Server/Auth/src/AuthServer.hpp:336`:
```cpp
auto result = rpc.value.value("result", rpc.value);
bool valid = result.value("valid", false);
```

Both sites follow the exact pattern that C-V4-2 fixed for `ServiceClient::Probe` and M-V4-9 fixed for `SessionCache::Validate`. `ServiceEndpoint`'s response to a MAC-rejected or envelope-version-skewed request is a **top-level** JSON envelope of shape `{"error": "Authentication failed"}` or `{"error": "unsupported_envelope_version", "expected_v": 1}`. The round-trip succeeds, so `rpc.ok()` is true. The Login/Register handlers then call `rpc.value.value("result", rpc.value)` — and when "result" is absent, the fallback **returns the entire `rpc.value`** (the envelope error). The subsequent `result.value("success", false)` / `result.value("valid", false)` returns false (the error envelope has no such field), and the user-facing path returns "Failed to create account" / "Invalid username or password".

The user-facing outcome is **correct by coincidence** (deny the login). The operational signal is **wrong**:
- `HandleRegister` logs `"Register: account creation failed, IP={} reason={}"` with `reason="Authentication failed"` — telling ops the user failed to register, not that the Auth↔Account internal channel is broken.
- `HandleLogin` logs only `LOG_AUTH_DEBUG("Login failed for user {} from {}")` at Debug level — meaning a Release deploy with mismatched `APHELYON_INTERNAL_SECRET` between Auth and Account would log every player as failing login with no WARN/ERROR signal at all. Same misdiagnosis as M-V4-1 ("MAC OK" startup log under secret mismatch) — startup probe is now correct (C-V4-2 closed), but the steady-state RPC path still hides the cause.

**Operational impact:** A production deploy with envelope skew (mid-rollout, where Auth has been bumped to envelope v2 but Account is still on v1) would show every login attempt failing with no signal pointing at the version mismatch. Combined with C-V4-2's fix the startup probe catches the case at deploy time — but a mid-flight `APHELYON_INTERNAL_SECRET` rotation that landed on Auth before Account would not retrigger startup probes, and Login/Register would silently fail until ops noticed the elevated login-failure rate.

**Fix:** apply the same pattern C-V4-2 and M-V4-9 already use. After `rpc.ok()` and BEFORE the `value("result", rpc.value)` fallback, inspect `rpc.value` for a top-level `"error"` field and surface it loudly:

```cpp
if (!rpc.ok()) { ... }
if (rpc.value.contains("error"))
{
    const auto err = rpc.value.value("error", std::string{});
    LOG_AUTH_ERROR("Login: account RPC envelope error: {} (likely secret mismatch / version skew), IP={}", err, clientIP);
    return CreateAuthResponse(false, "Account service unavailable", "", "", "", 0);
}
auto result = rpc.value.value("result", rpc.value);
```

Same shape for `HandleRegister`. ~6 LOC each. A regression test that wires up a ServiceClient with a deliberately-wrong secret and asserts `HandleLogin` returns the "service unavailable" path with an ERROR log entry would close this for good.

### H-V5-2. `HandleResumeSession` rate-limited by `clientIP` but per-IP cap on accept could mask the rate-limit signal

**Files:** `Server/Auth/src/AuthServer.hpp:473-478` (`HandleResumeSession` rate-limit), `Server/Common/src/Net/TcpServerBase.hpp:128-137` (H-V4-10 per-IP cap)
**Source:** This audit — interaction between H-V4-10 and H-V2-4
**Status:** NEW (interaction defect, not a regression)

H-V4-10 set `MAX_CONNECTIONS_PER_IP = 16`. A token-guessing attacker who opens 16 TCP connections from one IP exhausts the per-IP cap; the 17th connection is refused at accept with a `LOG_NET_WARN`. So far so good — except this **interacts adversely with the H-V2-4 ResumeSession rate-limiter** (`10/min/IP, 30s cooldown`):

1. Attacker opens 16 long-lived connections from IP X.
2. Attacker sends 10 ResumeSession requests on each connection, all with random 64-hex tokens.
3. Per-IP cap is full so no NEW connections from X can attempt resume. **But the existing 16 connections are not blocked from sending more probes.** Each connection can spam ResumeSession on its already-accepted socket, hitting the per-IP rate-limit only at the 10/min mark.
4. With 16 sockets × ~10/min × 60min = ~9,600 attempts/hr per IP — still far below 2^256 brute-force, but ~16× what the rate-limit ostensibly grants.

This is not a serious threat (256-bit token entropy makes any number of attempts negligible), but it documents that the per-IP rate-limit's denominator is "TCP connect attempts" while the per-IP cap's denominator is "concurrent sockets" — they don't compose to a single per-IP budget. Worth a comment in the rate-limit config, and an explicit decision on whether the per-IP cap should default lower for public-facing services (16 is generous for a real user; a legitimate user opens 1 TCP connection per service to a single Auth, then 1 per service after auth — total ~4).

**Fix options:**
- (a) Lower `MAX_CONNECTIONS_PER_IP` to 4 for `AuthServer` only (override in derived). Sufficient for legit clients (1 socket per service), refuses a multiplexed-spam attacker.
- (b) Count per-connection attempts toward the per-IP rate-limit budget — i.e., the rate limiter checks the IP and the connection ID; the connection's first ResumeSession after accept consumes one slot.
- (c) Mark as observation; the 256-bit token entropy makes even 9,600/hr negligible. The Probe-side rate-limit-bypass via TCP-connection-multiplexing remains a documented design tradeoff (similar to V3-M7 ResumeSession+Login sharing the same limiter).

Tracking as High because the interaction surface is non-obvious from reading either piece of code in isolation; future ops should not be surprised by it.

---

## MEDIUM

### M-V5-1. H-V4-10 per-IP cap is keyed by raw peer IP; NAT-collapsed deployments will lock out legit users

**Files:** `Server/Common/src/Net/TcpServerBase.hpp:110-139` (accept loop with cap check)
**Source:** This audit — H-V4-10 design tradeoff
**Status:** NEW (pre-launch operational concern)

`m_clientsByIp` is keyed by the literal `inet_ntop(AF_INET, &clientAddr.sin_addr, ...)` from accept — the immediate peer address. Two future deployment shapes turn this into a Foot-Gun:

1. **NAT-collapsed user populations.** A 1000-player school / dorm / corporate office shares one egress IP. At `MAX_CONNECTIONS_PER_IP=16` only 16 players from that NAT can connect at a time; the 17th is refused with `"per-IP cap 16 reached for {NAT IP}"` — a real-user denial of service, not an attacker.
2. **CDN / reverse-proxy fronted services.** If Aphelyon ever sits behind a TLS-terminating proxy (Nginx, Envoy, CloudFront), the per-IP cap becomes "max connections from the proxy" — which is 1 IP carrying 100% of traffic. The cap immediately misfires.

Today's deployment is bare-socket loopback / direct-internet so neither case applies. But the cap is hardcoded in `ServerConfig::MAX_CONNECTIONS_PER_IP` and isn't service-overridable; a future PR adding a reverse-proxy / load-balancer would have to find this constant and override it. Best handled now with:

- Document the assumption ("no NAT, no CDN") in `TcpSocket.hpp` next to the constant.
- Plumb `MAX_CONNECTIONS_PER_IP` as a constructor arg to `TcpServerBase` so the per-service default is overridable without a recompile.
- Add a `LOG_NET_WARN` rate-limit on the refusal log line (currently every refused-at-cap connection logs a fresh WARN — at 1000 NAT users behind one IP, the log volume on refusal would be one WARN per refused TCP SYN, possibly hundreds/sec).

Not a security defect today; documenting so the H-V4-10 fix carries its trade-off forward.

---

## LOW / OBSERVATION

### L-V5-1. `Crypto::DEFAULT_ITERATIONS = 200000` is unchanged

**File:** `Server/Common/src/Crypto/Crypto.hpp:41`

Carry-forward from V3-L9 / L-V4-1. OWASP 2023 PBKDF2-SHA256 floor is 600k; 200k is the documented lower bound. Pre-launch fine; bump before 2027 launch. Same item three audit rounds in a row.

### L-V5-2. `BCryptGenRandom` failure still silently falls back to `std::random_device`

**File:** `Server/Common/src/Crypto/Crypto.hpp:143-180`

Carry-forward from V3-M8 / L-V4-2. The `LOG_AUTH_WARN` fires once per failed call but the process continues. MSVC+BCrypt path is reliable in practice; the future-Linux risk is documented. Startup self-test still recommended — three audit rounds, still open.

### L-V5-3. `InternalRpcAuth::MAX_AGE_SECONDS = 30` is symmetric

**File:** `Server/Common/src/Net/InternalRpcAuth.hpp:58, 131-136`

Carry-forward from V3-L2 / L-V4-3. `|now - ts| > MAX_AGE_SECONDS` accepts a 30s-future-dated envelope. Loopback-only attack surface; runbook note: NTP drift > 30s breaks internal RPC silently.

### L-V5-4. `SeedAccounts` body compiles into Release binary

**File:** `Server/Account/src/main.cpp:51-86`

Carry-forward from L-V4-7. Body is gated only by `--seed-accounts` CLI flag, not by `#ifndef NDEBUG`. A Release operator who fat-fingers the flag would create test accounts with `password == username`. Flag itself is documented; gate the body with `#ifndef NDEBUG` so Release physically can't seed. ~3 LOC.

### L-V5-5. `m_questTokenSecret` heap zeroization unchanged

**File:** `Server/Account/src/AccountServer.hpp:351`

Carry-forward from V3-L6 / L-V4-8. `std::string` destructor frees but doesn't zero. Same "rotate-on-timer + zero" suggestion as v3/v4.

### L-V5-6. `Probe`'s `rpc.value.value("error", std::string{})` assumes `rpc.value` is an object

**File:** `Server/Common/src/Net/ServiceClient.hpp:300`
**Status:** NEW

`nlohmann::json::value(key, default)` requires the receiver to be an object — calling it on an array / scalar / null throws `type_error`. `RpcCallStatus::Success` guarantees Json::parse succeeded, but a peer (or a future intermediary) could legitimately respond with a non-object JSON value (e.g., `42`, `[1,2,3]`, `null`). The throw would propagate out of `Probe` to the startup-canary site where it is currently uncaught. Today's `ServiceEndpoint` only ever sends objects, so this is theoretical — but the same pattern at `SessionCache.hpp:148` (`rpc.value.contains("error")`) is `is_object`-safe (`contains` on non-objects returns false rather than throwing).

**Fix (defense-in-depth):** wrap the introspection in an `is_object()` guard:
```cpp
if (rpc.value.is_object())
{
    const auto err = rpc.value.value("error", std::string{});
    // ... existing classification ...
}
return ProbeOutcome::UnknownError;
```
~3 LOC.

---

## Verified Closed (from v4)

### C-V4-2 — `ServiceClient::Probe` inspects response body for envelope error
**Commit `25a81c1`** — verified at `Server/Common/src/Net/ServiceClient.hpp:289-325`. The Probe now:
- Reads `rpc.value.value("error", std::string{})` after `rpc.ok()`.
- Returns `EnvelopeVersionMismatch` on `"unsupported_envelope_version"`.
- Returns `AuthFailed` on `"Authentication failed"`.
- Returns `UnknownError` on any other non-empty `"error"` string OR when neither `"error"` nor `"result"` is present.
- Maps `WireError` → `UnknownError` (rpc.value is empty for frame/parse failures so no introspection possible).

The two error strings are exactly the ones `ServiceEndpoint` emits at `ServiceEndpoint.hpp:203-204` (envelope version) and `:218-219` (MAC failure). The five-outcome enum (`Ok`, `Unreachable`, `AuthFailed`, `EnvelopeVersionMismatch`, `UnknownError`) is now reachable for every value. The dead-code class of v4 closes. **Test gap:** no unit test exercises `Probe()` returning `AuthFailed` / `EnvelopeVersionMismatch` (Common/tests has `EnvelopeShapeTest`, `InternalRpcAuthRoundTripTest`, `RateLimiterSmokeTest`); v4 audit recommended this — still open as a backlog item.

### M-V4-2 — `LogSessionEvent` reason field; invalidate + cleanup emit structured events
**Commit `e5b02f4`** — verified at `Server/Common/src/Util/Logger.hpp:235-263` and `Server/Auth/src/SessionManager.hpp:447-512`.

- `LogSessionEvent` grew an optional 4th param `reason`. Empty reason → no `"reason"` field in the JSON (backwards-compat for InvalidateByToken / created / validated callers). Non-empty reason → `"reason":"<value>"` appended. Both paths still hash the token per M-V3-1 (8 hex chars of SHA-256(token)).
- `InvalidateByPlayerIdInternal` (forced-logout via re-login with `allowConcurrentSessions=false`) emits `Logger::LogSessionEvent("invalidated", oldSession.playerId, tok)` BEFORE the erase — visible to ops watching the structured stream. No `reason` since the eviction cause is well-defined ("displaced by re-login").
- `CleanupExpiredInternal` per-iteration drop emits `"invalidated"` with a classifier `reason` of `"invalid"` (session.valid was false), `"expired"` (lifetime past), or `"idle"` (no activity within `idleTimeoutSeconds`). The classifier preserves which sweep arm fired so post-mortem queries can distinguish.

**JSON escaping note:** the `reason` strings are compile-time string literals (`"invalid"`, `"expired"`, `"idle"`) so the fmt-spec embedding can't be broken by user-supplied content. If a future caller passes a runtime-derived reason (e.g., admin-supplied logout reason), the `R"({{..."reason":"{}"}})"` format string would need an explicit JSON-escape — today's call sites are safe.

### M-V4-6 — `Crypto::ParseStoredHash` rejects odd-length salt/hash hex
**Commit `e5b02f4`** — verified at `Server/Common/src/Crypto/Crypto.hpp:359-392`. The audit comment is correct that `FromHex`'s `i + 1 < hex.length()` loop guard silently truncates odd-length input — the fix adds an explicit `(saltHex.size() % 2) != 0 || (hashHex.size() % 2) != 0` check BEFORE `FromHex` runs. Returns false on rejection, which causes `VerifyPassword` to fall through to its M-V3-3 dummy-hash PBKDF2 burn (`Crypto.hpp:113-128`) — timing-oracle mitigation still triggers on parse failure. The two M-V3-3 + M-V4-6 defenses now compose:
- Malformed `iter:salt:hash` overall (no colon, bad iteration count, empty salt/hash, OR odd-length hex) → ParseStoredHash returns false → VerifyPassword burns one PBKDF2 derivation against dummySalt(0xA5) → returns false. Wall time matches the legitimate path.

The odd-length case used to consume a wasted PBKDF2 (the V4-original behavior was to feed the truncated bytes through PBKDF2, get a hash of the wrong length, fail `ConstantTimeCompare` on size). With M-V4-6 the rejection happens BEFORE the legitimate-path PBKDF2, but the M-V3-3 dummy-hash compute STILL fires, so the legitimate-path wall time is preserved.

### H-V4-4 — Debug ServiceClient timeout split via NDEBUG-gated constant
**Commit `992fd06`** — verified at `Server/Common/src/Net/ServiceClient.hpp:64-72`, `:103`, `:169`. `kSocketTimeoutMs` constant is 5000 in Release (`#ifdef NDEBUG`), 30000 in Debug. Both call sites (`Connect()` at L103, `Call()` mid-reconnect at L169) use the constant — no magic-number drift surface. Release path keeps the 5s SLO (200k iters compile to ~2s under -O2, well under). Debug-build Auth→Account `VerifyCredentials` no longer trips on the 5s timeout. **Release behavior unchanged.**

### M-V4-9 — `SessionCache::Validate` classifies envelope error and flips to AuthLost
**Commit `388c0dd`** — verified at `Server/Common/src/Net/SessionCache.hpp:137-156`. The new branch fires AFTER `rpc.ok()` and BEFORE the `rpc.value.value("result", rpc.value)` fallback. On `rpc.value.contains("error")`:
- Emits `LOG_AUTH_WARN` with the envelope error string ("Authentication failed" or "unsupported_envelope_version").
- Calls `NoteAuthLost()` (flips the atomic auth-down flag, emits a one-shot WARN).
- Returns `SessionInfo{valid=false}` — same outcome as the legacy code, but the cause is now correctly logged.

Note: this means a deployed-with-mismatched-secret SessionCache will keep firing NoteAuthLost on every Validate (since IsAvailable still returns true at the socket level — the wireError check is what gates the cache's TTL-extension path, but envelope-error isn't a wireError). The auth-down extension path checks `m_authClient.IsAvailable()` which stays true throughout, so extensions DON'T kick in — every cache miss revalidates against Auth, every revalidation returns envelope-error, every session becomes invalid. The user-facing outcome is correct ("every session invalid until ops fix the secret"); ops see a sustained WARN stream identifying the cause.

### H-V4-10 — TcpServerBase per-IP and total connection caps
**Commit `534233c`** — verified at `Server/Common/src/Net/TcpSocket.hpp:42-49` (constants) and `Server/Common/src/Net/TcpServerBase.hpp:113-147, 197, 551-561, 581` (accept loop + cleanup).

- `MAX_CONNECTIONS_TOTAL = 2048`, `MAX_CONNECTIONS_PER_IP = 16`.
- Accept loop checks BOTH caps BEFORE `LogConnectionEvent("connected")` and BEFORE the `std::thread` spawn. Refused connections: socket closed, `LOG_NET_WARN` identifies which cap fired and the IP, `continue`s. Per-IP counter is NOT incremented on refusal — prevents accumulation of dead counters from ephemeral attacker IPs.
- Cleanup path (CleanupThread at line 534+) decrements `m_clientsByIp[ip]` after the per-client thread joins, and erases the map entry when the counter hits zero. Working set is bounded to active IPs.
- Stop() clears `m_clientsByIp` alongside `m_clients` at line 197.

**Subtle behaviors:**
- The cap check and the thread spawn happen UNDER `m_clientsMutex` (line 120-147). If `std::thread(...)` throws on OS-thread-limit (rare), the per-IP counter is already incremented but no `HandleClient` ever runs — orphaned counter. The exception would propagate out of the accept loop, killing the accept thread. Recovery from this is "service restart"; the leaked counter doesn't matter since the process is dead. Minor robustness concern; not exploitable.
- The pre-increment + log inside the lock means a high-burst attack still spends one mutex lock per refused connection. With the listen-socket's backlog cap (`SOMAXCONN`) absorbing the initial burst, this should be bounded. Not flagging as a defect.
- Refusal log at WARN every time gets noisy under sustained attack — see M-V5-1 for the NAT-related lock-out concern. Rate-limited logging recommended.

### v3 carry-forwards still closed
All v3-confirmed-closed items (H-V3-10 dummy hash function-local static, H-V3-11 LruCache Peek/Touch + RateLimiter touch-iff-allow, H-V3-14(a) envelope version + golden-file pin, M-V3-1 token-hash log, M-V3-2 DB connection env var, M-V3-3 PBKDF2 burn on parse failure, M-V3-4 legacy ValidateSession removed, V3-L7 bindToIP=true default, V3-L10 token generation retry cap, V3-L5 global snapshot doc) remain correct. Spot-verified the file:line references — no regressions, no documentation drift.

---

## Threat-Model Summary (v5 baseline)

| Attack | v4 status | v5 status |
|---|---|---|
| Session hijack via stolen token from any IP | Closed | Closed |
| Internal RPC impersonation w/o secret | Closed | Closed |
| Internal RPC MAC bypass via canonicalization | Closed | Closed |
| Internal RPC envelope shape drift | Closed | Closed |
| Memory-only rollback corruption | Closed | Closed |
| Username enumeration via timing | Closed | Closed (+ M-V4-6 defense-in-depth) |
| ResumeSession brute-force | Closed | Closed (subject to H-V5-2 socket-multiplex grain) |
| Limiter map inflation via IP spray | Closed | Closed |
| Limiter spray locks out legit users | Closed | Closed |
| Quest-token forgery via length extension | Closed | Closed |
| Offline PBKDF2 brute-force on stolen hash | Closed | Closed |
| Quest token loss across restart (Release) | Closed | Closed |
| DB password leak from binary | Closed | Closed |
| Token-prefix leak via session log | Closed | Closed |
| Misconfigured-peer-secret deploy goes undetected at startup | Open (H-V4-1) | **Closed** (C-V4-2) |
| Misleading "MAC OK" startup log line under secret mismatch | Open (M-V4-1) | **Closed** (via C-V4-2 — startup path) |
| Misdiagnosed login failures under steady-state secret mismatch | n/a | **Open (H-V5-1)** |
| Forced-logout / idle-sweep invisible to structured log | Open (M-V4-2 / L-V4-6) | **Closed** (M-V4-2 reason field) |
| Slow-loris / connection-flood DoS | Open (H-V4-10) | **Closed** for raw IP; **Open** for NAT/CDN (M-V5-1) |
| Per-IP rate-limit bypass via TCP socket multiplexing | n/a | Documented (H-V5-2) |

---

## Suggested triage order

**This week:**
1. **H-V5-1** — apply the C-V4-2 pattern in `HandleLogin` and `HandleRegister`. ~6 LOC × 2 sites + 1 regression test. Closes the steady-state misdiagnosis hole opened by mid-flight secret rotation between Auth and Account.
2. **H-V5-2** — decide between options (a)/(b)/(c). If (a): override `MAX_CONNECTIONS_PER_IP` in `AuthServer` to 4. ~2 LOC.

**Before launch:**
3. **M-V5-1** — plumb `MAX_CONNECTIONS_PER_IP` through `TcpServerBase` ctor + document the assumption. ~10 LOC.
4. **L-V5-6** — wrap `Probe`'s `rpc.value.value("error", ...)` in `is_object()` guard. ~3 LOC.
5. **L-V5-2** — `BCryptGenRandom` failure → Release-fatal, or startup self-test. (3rd audit round.)
6. **L-V5-4** — gate `SeedAccounts` body with `#ifndef NDEBUG`. (2nd audit round.)

**Eventually:**
7. PBKDF2 600k iteration bump pre-launch (L-V5-1).
8. Quest-token secret heap zeroization + rotation (L-V5-5).
9. Common/tests case for `ServiceClient::Probe` returning each `ProbeOutcome` value (gap noted in C-V4-2 verification).

---

## Assurance — what's provably correct (v5 update)

- **Crypto primitives unchanged:** PBKDF2-HMAC-SHA256 at 200k iters, HMAC-SHA256 RFC 2104, constant-time byte/hex compare. All unchanged from v4 except `ParseStoredHash`'s new odd-length guard.
- **Envelope MAC input layout:** `v || '\n' || method || '\n' || params_json || '\n' || ts`. Unchanged from v4; golden-file pinned.
- **Envelope wire shape:** golden-file pinned at `Server/Common/tests/EnvelopeShapeTest.cpp`. `BuildEnvelope` is the only producer.
- **Probe classification:** five outcomes (`Ok`, `Unreachable`, `AuthFailed`, `EnvelopeVersionMismatch`, `UnknownError`) all reachable from real server responses. Verified by walking `ServiceEndpoint` response branches at `:199-204` (version mismatch), `:216-219` (MAC failure), `:240-242` (success).
- **SessionCache envelope classification:** matches Probe's pattern. Walked all three rpc-handling branches in `Validate` (cache hit, cache miss reachable, cache miss with envelope error).
- **Connection caps:** total 2048, per-IP 16. Pre-spawn check under m_clientsMutex; cleanup decrements on disconnect; counter erased when zero. Tested by inspection that refusal path closes socket + skips thread spawn cleanly.
- **Env-var enforcement matrix (unchanged from v4):**
  - `APHELYON_INTERNAL_SECRET`: Release-fatal in all three services.
  - `APHELYON_DB_CONNECTION`: Release-fatal in Account.
  - `APHELYON_QUEST_TOKEN_SECRET`: Release-fatal in Account.
  - `g_rateLimitingEnabled`: Release-fatal if disabled.
  - `g_authIPBindingEnabled`: Release-fatal if disabled (Auth only).
- **Session lifecycle:** every read path either runs `GetValidSession` (full check) or is documented as bare-flag (`GetSession`, `GetPlayerName` — only used by HandleLogout / HandleHeartbeat for log-line playerName fields). Token-hash logging at LogSessionEvent. Token entropy 256-bit, IP-bound default, retry-capped collision check.
- **Structured session event coverage:** `created`, `validated`, `invalidated` (no reason — InvalidateByToken / explicit), `invalidated` + reason="invalid" / "expired" / "idle" (cleanup sweep), `invalidated` + no reason (forced-logout via re-login). The full session lifecycle is now visible in the structured stream.
- **Three unauthenticated entrypoints:** Login, Register, ResumeSession. All three rate-limited.
- **SQL injection surface:** all queries use `pqxx::params{}` or hardcoded literals (e.g., `"CALL partman.run_maintenance_proc()"`). No string concatenation of user input into SQL. Verified by walking `RelationalFlush.hpp`, `AccountRepository.hpp`, `OutboxRelay.hpp`, `EventStore.hpp`.

---

## Agent observations

The v4 → v5 sweep continues the v3 → v4 trajectory — every named security item lands correctly, no regressions, the Probe / SessionCache / Logger fixes are all small and exact. The new H findings are not regressions of v4 fixes but **completion gaps on the same finding shape** — C-V4-2 fixed the Probe path; H-V5-1 says "the same pattern lives in two more handlers." The fix is mechanical; the cost is one test case per site.

H-V5-2 and M-V5-1 are the first real architectural surface this audit has flagged in the connection-cap territory — H-V4-10 is correct as a slow-loris mitigation but its threat model isn't quite the same as a NAT-aware rate-limit. Worth a brainstorming session before launch on what the production deployment shape will actually look like (direct internet? CloudFront? Cloudflare? bare loopback?) — the answer shapes whether the per-IP cap is sufficient or actively harmful.

The carry-forward Low items continue to dominate (5 of 6 Lows here are 3rd-or-greater audit appearances). Worth a single "low-batch" PR that closes L-V5-1 / L-V5-2 / L-V5-4 / L-V5-5 / L-V5-6 in one sweep — none individually deserves a PR; collectively they retire the noise floor.

Recommend: ship H-V5-1 + L-V5-6 this week (both are 3 / 6 LOC fixes with one test case each); decide H-V5-2 and M-V5-1 against the production deployment shape before launch; queue the rest behind C-V4-2's missing tests in Common/tests.
