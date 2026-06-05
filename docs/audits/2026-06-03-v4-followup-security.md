# V4 Follow-up Audit: Security

**Date:** 2026-06-03
**Dimension:** Security
**Scope:** Verify the v3 remediation arc (commits `f46d496..HEAD`, ~25 fix commits) and surface any new security defects introduced or exposed by the v3 sweep. Same surface as v3: Auth handlers, SessionManager, SessionCache, InternalRpcAuth, ServiceEndpoint/Client, Crypto primitives, secrets handling, login/resume flow timing, rate-limiter eviction semantics, protocol.json `requires_auth:false` surface, env-var enforcement matrix, token logging.
**Method:** Single security-lens pass — read each v3-touched file end-to-end, walk the call graphs the v3 fixes claim to close, and exercise the failure-mode trees on paper.

---

## Verdict

The v3 remediation pass is **substantially clean**. All ten named v3 security items (H-V3-10, H-V3-11, M-V3-1..4, V3-L5/L7/L10, plus the H-V3-14 envelope-version + canary work) hold up under direct examination. Token-hash logging, function-local dummy hash, LRU `Peek/Touch` split, `bindToIP=true` struct default, token-generation retry cap, envelope version field with golden-file pinning, `RpcCallResult` distinguishing wire/unreachable failures — every one is correct in code. The crypto primitives are unchanged (correctly), `VerifyPassword` now burns one PBKDF2 derivation even on parse failure (M-V3-3 closed). Three unauthenticated entrypoints (Login/Register/ResumeSession) remain the entire public surface and all three are rate-limited. The threat model from v3 is preserved — no new bypass classes opened. **One real High** is the startup probe (H-V3-14(b)): the `ProbeOutcome::AuthFailed` and `ProbeOutcome::EnvelopeVersionMismatch` values are declared but never returned — `ServiceClient::Probe` reports `Ok` on every MAC-rejected response because the server returns a structurally-valid JSON error envelope, which `Call()` reports as `RpcCallStatus::Success`. The canary "works" only against complete reachability failure. Recommend a focused fix this week.

Verdict: **clean** (one High, one Medium, several Lows; no Criticals; no broken v3 fixes).

---

## CRITICAL

*(None.)*

The v3 remediation closes the two v3 Criticals (C-V3-1 reducer default, C-V3-2 lazy-mutating accessors) in adjacent dimensions; nothing in the security surface has Critical impact.

---

## HIGH

### H-V4-1. `ServiceClient::Probe` reports `Ok` when the peer rejects the MAC

**Files:** `Server/Common/src/Net/ServiceClient.hpp:258-294` (Probe), `Server/Common/src/Net/ServiceEndpoint.hpp:179-200` (envelope-version + MAC reject paths)

**Status:** NEW — H-V3-14(b) completeness miss.

The H-V3-14(b) design intent (per `ProbeOutcome` enum + `ServiceClient::Probe` docstring) is to fatal-out a Release service whose peer rejected our envelope version or MAC, distinguishing those *permanent* misconfigurations from a *transient* peer-not-up-yet retry case. In code:

```cpp
// ServiceEndpoint, on MAC failure (L196-200):
response = {{"error", "Authentication failed"}};
// On envelope-version mismatch (L179-185):
response = {{"error", "unsupported_envelope_version"}, {"expected_v", APHELYON_ENVELOPE_VERSION}};
// Both then go through:
std::string wireMsg = FormatRpcMessage(response.dump());
SendAll(clientSocket, wireMsg.c_str(), ...);
```

That's a **structurally well-formed** JSON RPC frame sent back. `ServiceClient::Call` reads it, frames it, parses it successfully, and returns `RpcCallResult{ RpcCallStatus::Success, parsedJson }`. Then `Probe`:

```cpp
auto rpc = Call("Ping", Json::object());
if (rpc.ok())               // ← true even though the body is {"error":"..."}
    return ProbeOutcome::Ok;
```

Nothing inspects `rpc.value` for an `"error"` key. The result: `Probe` returns `ProbeOutcome::Ok` whether the peer accepted the Ping, rejected the MAC, or rejected the envelope version. The Release-fatal path (`Probe → fatal-out`) only fires when the peer is *completely unreachable*. The whole "secret mismatch ops triage" design intent is dead code; the `AuthFailed` / `EnvelopeVersionMismatch` enum values are never returned.

**Exploitability:** zero direct (loopback-only, attacker would need shell on the box). **Operational impact:** moderate — a production deploy with mismatched `APHELYON_INTERNAL_SECRET` across services starts cleanly, the canary logs "Auth/Account reachable, MAC + envelope-v OK", and then every real RPC silently fails authentication. The exact failure mode the canary was supposed to surface up front is masked.

**Fix:** in `Probe`, after `rpc.ok()`, inspect `rpc.value`:
```cpp
if (rpc.ok()) {
    if (rpc.value.contains("error")) {
        const auto err = rpc.value.value("error", "");
        if (err == "unsupported_envelope_version") return ProbeOutcome::EnvelopeVersionMismatch;
        if (err == "Authentication failed")       return ProbeOutcome::AuthFailed;
        return ProbeOutcome::UnknownError;
    }
    if (!rpc.value.contains("result")) return ProbeOutcome::UnknownError;
    return ProbeOutcome::Ok;
}
```
Add a Common-tests case (`ServiceClientProbeTest.cpp`) that stands up a `ServiceEndpoint` with a known secret and probes with the wrong secret to assert `AuthFailed`.

---

## MEDIUM

### M-V4-1. Probe success log line claims "MAC OK" without checking the response body

**File:** `Server/Account/src/AccountServer.hpp:170-185`, `Server/Auth/src/AuthServer.hpp:110-125`

Downstream of H-V4-1: both probe sites log `"Auth reachable, MAC + envelope-v OK"` / `"Account reachable, MAC + envelope-v OK"` on `ProbeOutcome::Ok`. With H-V4-1 unfixed, that log line is **false** on secret mismatch (the peer rejected the MAC; the probe reported Ok). Ops reading startup logs would conclude the channel is healthy when it isn't. After fixing H-V4-1 this log line becomes accurate; tracking as Medium because the misleading-log surface is itself a runbook hazard.

### M-V4-2. `InvalidateByPlayerIdInternal` still doesn't log `LogSessionEvent("invalidated", ...)`

**File:** `Server/Auth/src/SessionManager.hpp:447-456`

V3-M4 from the v3 audit. The v3 sweep didn't action this; the function still erases the session map row without emitting the structured `session_invalidated` event. CreateSession (line 137) emits an unstructured `LOG_AUTH_DEBUG("Invalidating previous session...")` but not the structured event. CleanupExpiredInternal (line 458-487) has the same gap. Result: forced-logout-via-relogin and idle/expired sweep are invisible to the structured session log.

**Fix:** unchanged from V3-M4 — emit `Logger::LogSessionEvent("invalidated", oldSession.playerId, oldSession.token)` inside `InvalidateByPlayerIdInternal` before `m_sessions.erase`. Same fix at the per-iteration drop point in `CleanupExpiredInternal`.

### M-V4-3. Debug-fallback `APHELYON_QUEST_TOKEN_SECRET` warning is below INFO sometimes

**File:** `Server/Account/src/AccountServer.hpp:291-295`

The warning when the env var is unset still goes out at `LOG_AUTH_WARN`, which is correct. But in a Debug build with `-q` (quiet → `Level::Info`), the warn surfaces in the console; in a Debug build with default `-v` it surfaces only because verbose flips file to Trace. Not a code defect — a runbook note: dev operators should not assume the absence of WARN in console output means the secret is configured. Worth verifying via the explicit `LOG_AUTH_INFO("Quest token secret loaded from APHELYON_QUEST_TOKEN_SECRET")` line on the success path; if that line is absent, the fallback is in effect.

### M-V4-4. Lazy DB-conn read in `DbConfig::GetConnectionString` shares main.cpp's NDEBUG gate

**Files:** `Server/Account/src/Db/DbConfig.hpp:35-51`, `Server/Account/src/main.cpp:212-218`

`DbConfig::GetConnectionString` reads `APHELYON_DB_CONNECTION` exactly once via thread-safe static-local init. In Release, an unset env returns `""` and `IsConfigured()` returns false — `main.cpp` then fatal-exits. **But** `IsConfigured()` calls `GetConnectionString()` which fires the static-local init; subsequent calls (e.g., from a test fixture that also imports DbConfig) get the same empty string. For tests that bypass main.cpp and assume the env var is read fresh, this is a subtle gotcha. Today's tests use their own literal `kConn` constants per the audit comment, so no test currently hits this — but a future test that reaches for `DbConfig::GetConnectionString` would silently get the empty-Release-fallback on a Release test build. Worth a comment hardening, not a code change.

### M-V4-5. Quest-token secret is read once at `InitializeQuests`; runtime env-var change requires restart

**File:** `Server/Account/src/AccountServer.hpp:282-295`

Documented behavior; flagged here only because `APHELYON_QUEST_TOKEN_SECRET` is the **only** secret in the system without runbook documentation of its rotation requirements (the internal-RPC secret is cached via `InternalRpcAuth::GetSharedSecret` static-local — same semantics — but is documented). Runbook addendum, not a code change.

### M-V4-6. `Crypto::FromHex` silently accepts odd-length input via the `i + 1 < hex.length()` loop guard

**File:** `Server/Common/src/Crypto/Crypto.hpp:337-349`

If a corrupted DB row stored an odd-length salt or hash hex, `FromHex` produces a truncated byte vector without error. `ParseStoredHash`'s `!salt.empty() && !hash.empty() && iterations > 0` catches the empty case but not the truncated case. With M-V3-3 in place `VerifyPassword` would still burn a PBKDF2 on a truncated hash (good) — but the constant-time compare would fail against the wrong length. Defense-in-depth: have `FromHex` return `std::optional<std::vector<uint8_t>>` and propagate the failure to `ParseStoredHash`, OR add an explicit `hex.length() % 2 == 0` check in `ParseStoredHash`. Low-likelihood, low-impact today; flagging for a future Crypto.hpp cleanup pass.

---

## LOW / OBSERVATION

### L-V4-1. `Crypto::DEFAULT_ITERATIONS = 200000` is unchanged

**File:** `Server/Common/src/Crypto/Crypto.hpp:41`

Carry-forward from V3-L9. OWASP 2023 PBKDF2-SHA256 recommendation is 600k; 200k is the documented lower bound. Pre-launch is fine; bump before 2027 launch.

### L-V4-2. `BCryptGenRandom` failure still silently falls back to `std::random_device`

**File:** `Server/Common/src/Crypto/Crypto.hpp:143-180`

Carry-forward from V3-M8. The `LOG_AUTH_WARN` fires once per failed call but the process continues and uses `random_device` for token + salt + dummy-hash generation. On MSVC + Windows + BCrypt this effectively never fires; on a future Linux build the fallback path becomes load-bearing. Worth a startup self-test that asserts `BCryptGenRandom` works once and refuses to start otherwise (recommended in V3-M8; still open).

### L-V4-3. `MAX_AGE_SECONDS = 30` is symmetric

**File:** `Server/Common/src/Net/InternalRpcAuth.hpp:58, 131-136`

Carry-forward from V3-L2. `|now - ts| > MAX_AGE_SECONDS` accepts a 30s-future-dated envelope. Loopback-only attack surface; runbook note: NTP drift > 30s breaks internal RPC silently.

### L-V4-4. ServiceEndpoint MAC-rejection log line still emits `method` at WARN

**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:198, 181`

Carry-forward from V3-L8. A loopback attacker who has the secret could replay-spam stale envelopes to inflate WARN log volume. Loopback-only; "log volume amplification" not "secret exposure."

### L-V4-5. `SessionCache::Validate`'s extension-path log line still includes the player ID at Trace

**File:** `Server/Common/src/Net/SessionCache.hpp:77-79`

Carry-forward from V3-L1. Trace level so impact is negligible; runbook note.

### L-V4-6. `CleanupExpiredInternal` drops sessions without `LogSessionEvent("expired", ...)`

**File:** `Server/Auth/src/SessionManager.hpp:458-487`

Same defect class as M-V4-2. The structured `session_expired` event isn't emitted by the sweep; only an unstructured `LOG_AUTH_DEBUG("Session cleanup: removed {} expired/idle sessions", removed)` summary. Downstream analytics keying on `LogSessionEvent` will not see expiration unless triggered explicitly by `GetValidSession` returning `Expired` / `IdleTimeout`.

### L-V4-7. `SeedAccounts` (`main.cpp:51-86`) creates accounts with `password == username`

**File:** `Server/Account/src/main.cpp:51-86`

Dev-only path gated behind `--seed-accounts`. Worth a startup banner asserting `#ifndef NDEBUG` so a Release binary refuses to run `--seed-accounts` even if someone tries — today the seed function compiles into Release and the only gate is the operator not passing the flag. Not exploitable (Release rate-limiter + IP binding still apply on the resulting logins), just hygiene.

### L-V4-8. `m_questTokenSecret` heap zeroization unchanged

**File:** `Server/Account/src/AccountServer.hpp:351`

Carry-forward from V3-L6. `std::string` destructor frees but doesn't zero. Same "rotate-on-timer + zero" suggestion as v3.

### L-V4-9. `Crypto::HmacSha256Hex` allocates intermediate `std::vector` per call

**File:** `Server/Common/src/Crypto/Crypto.hpp:195-198`

Not a security defect; just observation. The quest-token verify path runs HMAC twice per minigame report; on a 60-DAU service this is negligible but if a future system batches HMACs in a hot path the per-call allocation chain is worth flagging. Constant-time compare is correctly `HexEquals` afterward.

### L-V4-10. ServiceClient backoff window starts at 1s and doubles to 30s; no jitter

**File:** `Server/Common/src/Net/ServiceClient.hpp:311-326`

If multiple Account workers all hit a transient Auth outage simultaneously, they retry in lockstep. Thundering herd risk is small at 1-Account-1-Auth scale but worth a `+ rand(0, backoff/4)` jitter line for the deployment that lands two Account replicas. Not a security defect; operational hardening note.

---

## Verified Closed (from v3)

### H-V3-10 — dummy password hash → function-local static
**Commit `0079c17`** — verified at `Account/src/Handlers/InternalRpcHandlers.hpp:94-96`. `s_dummyPasswordHash` is a function-local static computed via `Crypto::HashPassword(...)` on first call. C++11 magic-statics guarantee thread-safe init. The previous `const std::string& m_dummyPasswordHash` ref-binding to a not-yet-populated AccountServer member is gone — `InternalRpcHandlers` no longer holds the field at all. The startup-ordering invariant the v3-H1 audit flagged is structurally impossible to break: any future refactor that moves the hash elsewhere would need a recompile of `HandleVerifyCredentials` itself, where the static lives. **Defense check:** the static initializer calls `Crypto::HashPassword` which always emits a well-formed `iter:salt:hash`, so `ParseStoredHash` cannot short-circuit on this value. Coupled with M-V3-3's parse-failure PBKDF2 burn, the timing oracle is now closed by *two* independent defenses.

### H-V3-11 — LruCache Peek/Touch split; RateLimiter touch-iff-allow
**Commit `82f8051`** — verified at `Common/src/Util/LruCache.hpp:60-103` (Get/Peek/Touch methods) + `Common/src/Net/RateLimiter.hpp:74-119` (Allow path). `LruCache::Get` still touches (kept for clarity); the new `Peek(key)` returns the same value pointer without splicing to MRU; the new `Touch(key)` is an explicit MRU refresh. RateLimiter's Allow now uses Peek on the initial lookup and calls Touch **only on accept branches** — cooldown rejection and limit-tripped paths explicitly `return false` with no Touch. The M-V2-10 LRU-inversion attack (sustained attacker spray pushing legit user to LRU) is closed: attacker probes that hit cooldown don't refresh recency. Verified by reading both accept paths (cooldown-expired-reset L93-97, window-reset L100-105, normal-accept L117-119) and both reject paths (cooldown-active L86-93, limit-tripped L107-115) — only the four accept paths touch.

### H-V3-14(a) — envelope version field + golden-file shape test + MAC over v
**Commit `da38a09`** — verified at `Common/src/Net/InternalRpcAuth.hpp:53` (`APHELYON_ENVELOPE_VERSION = 1`), `:100-116` (`ComputeMac` prepends v), `:122-139` (`Verify` checks v explicitly and MAC binds v), `:145-158` (`BuildEnvelope` canonical construction). The `Common/tests/EnvelopeShapeTest.cpp` golden-file test pins the serialized envelope bytes against a known fixed input + MAC. The `Common/tests/InternalRpcAuthRoundTripTest.cpp` exercises string / int / float / bool+null / nested / empty payloads, bit-flip rejection, method mismatch, stale ts, wrong secret, and v-mismatch (both layout-binding and explicit-check). The "wire shape has exactly one owner" invariant is enforced by `BuildEnvelope` being the only producer — `ServiceClient::Call` (L179) routes through it; the test snapshots it. A future shape change forces a re-pin in the same commit. Clean.

### H-V3-14(b) — startup canary Ping with retries (partial — see H-V4-1)
**Commit `bfbcf2c`** — verified at `Common/src/Net/ServiceEndpoint.hpp:31-46` (auto-registered Ping responder), `Common/src/Net/ServiceClient.hpp:258-294` (Probe with retry budget), `Account/src/AccountServer.hpp:170-185` + `Auth/src/AuthServer.hpp:110-125` (Release-fatal vs Debug-warn gating). The reachability-probe aspect works: a peer that's not up gets retried, then reports `Unreachable`, then Release-fatals. **But the secret-mismatch and envelope-version-mismatch arms of the design intent don't actually return their enum values** — that's the H-V4-1 finding above. Filed under "Verified Closed" with an asterisk: the *reachability* canary is correct; the *authentication* canary is structurally present but returns `Ok` on auth failure.

### M-V3-1 security — token-hash log instead of token-prefix
**Commit `7c0790b`** — verified at `Common/src/Util/Logger.hpp:231-248`. `LogSessionEvent` now SHA-256s the token and emits the first 8 hex chars as `token_hash` in the JSON event. The correlation property (same token → same log marker) is preserved; preimage resistance means the full log set leaks nothing about the underlying token. The three call sites (`SessionManager.hpp:161` created, `:274` validated, `:330` invalidated) all pass the full token; the hash is computed inside `LogSessionEvent`. Per-RPC validated-event volume is still high (~30-60/min per active client) but no longer leaks token bytes.

### M-V3-2 security — APHELYON_DB_CONNECTION env var; Debug fallback
**Commit `d1b9e3f`** — verified at `Account/src/Db/DbConfig.hpp` (new file) + `Account/src/main.cpp:212-232` (gate + pass to AccountServer) + `Account/src/AccountServer.hpp:62-67` (required ctor arg, no default). The literal `postgresql://aphelyon:aphelyon@localhost:5432/aphelyon` no longer appears in `AccountServer.hpp`. Test fixtures still own their own `kConn` literals per the documented exception, but Release binaries no longer carry the dev password. The static-local init read pattern matches `InternalRpcAuth::GetSharedSecret`. Subtle gotcha noted in M-V4-4 above for future test authors.

### M-V3-3 security — VerifyPassword burns PBKDF2 on parse failure
**Commit observed in code** — verified at `Common/src/Crypto/Crypto.hpp:113-128`. On `ParseStoredHash` failure, `VerifyPassword` runs `PBKDF2_HMAC_SHA256(password, dummySalt(0xA5), DEFAULT_ITERATIONS, HASH_LENGTH)` with the result discarded. The wall time on a malformed-stored-hash path now matches the legitimate parse-then-derive path. Defense-in-depth pairing with H-V3-10: even if some future regression handed `VerifyPassword` an empty / malformed hash (an init-order bug that H-V3-10 itself made structurally impossible, but which could still arise via a corrupted DB row), the timing oracle stays closed.

### M-V3-4 security — legacy SessionManager::ValidateSession removed
**Verified at `Auth/src/SessionManager.hpp:171-177`** — the previous `bool ValidateSession(token, ip, bumpIdle)` method is gone. The header carries an audit comment explaining the removal and pointing future readers at `GetValidSession`. No call sites remain (verified by grep — `LogSessionEvent("validated", ...)` is the only `validated` literal in the codebase and is internal to `GetValidSession`). The narrower-contract API can no longer be reached by mistake.

### V3-L7 security — SessionConfig::bindToIP default true
**Commit `6483b01`** — verified at `Auth/src/SessionManager.hpp:24-28`. Default value changed from `false` to `true`. Audit comment documents the rationale (stray `SessionManager{}` in a future test gets production-safe value). AuthServer's `CreateSessionConfig` still overrides from the runtime-toggleable global, so the production behavior is unchanged.

### V3-L10 security — token generation retry cap
**Commit `6483b01`** — verified at `Auth/src/SessionManager.hpp:417-445`. `TOKEN_GEN_MAX_RETRIES = 10`. Loop tries up to 10 times to find a non-colliding token; on the 11th attempt throws `std::runtime_error`. A buggy CSPRNG returning a constant value now produces a loud failure instead of a silent infinite loop. 256-bit entropy makes the birthday bound theoretical (~2^-128 per call); the cap is pure defense-in-depth.

### V3-L5 security — global IP-binding snapshot semantics doc
**Commit `6483b01`** — verified at `Auth/src/AuthServer.hpp:19-31`. The `g_authIPBindingEnabled` global now carries an explicit comment noting that `CreateSessionConfig` reads it once at SessionManager construction and subsequent toggle changes don't propagate. Documentation hardening; no code change. Future runtime-toggle work would need to plumb through `SessionManager::SetConfig`.

---

## Threat-Model Summary (v4 baseline)

| Attack | v3 status | v4 status |
|---|---|---|
| Session hijack via stolen token from any IP | Closed | Closed |
| Internal RPC impersonation w/o secret | Closed | Closed |
| Internal RPC MAC bypass via canonicalization | Closed | Closed |
| Internal RPC envelope shape drift | Latent | **Closed** (H-V3-14(a)) |
| Memory-only rollback corruption | Closed | Closed |
| Username enumeration via timing | Closed (subject to V3-H1) | **Closed** (H-V3-10 + M-V3-3) |
| ResumeSession brute-force | Closed | Closed |
| Limiter map inflation via IP spray | Closed | Closed |
| Limiter spray locks out legit users | Closed (subject to V3-H2) | **Closed** (H-V3-11) |
| Quest-token forgery via length extension | Closed | Closed |
| Offline PBKDF2 brute-force on stolen hash | Closed | Closed |
| Quest token loss across restart (Release) | Closed | Closed |
| DB password leak from binary | Partial (V3-M2) | **Closed** (M-V3-2) |
| Token-prefix leak via session log | Open (V3-M1) | **Closed** (M-V3-1) |
| Misconfigured-peer-secret deploy goes undetected at startup | n/a | **Open (H-V4-1)** |
| Misleading "MAC OK" startup log line under secret mismatch | n/a | Open (M-V4-1, follow-on of H-V4-1) |
| Forced-logout / idle-sweep invisible to structured log | Open (V3-M4) | **Open** (M-V4-2 / L-V4-6) |

---

## Suggested triage order

**This week:**
1. **H-V4-1** — extend `ServiceClient::Probe` to inspect `rpc.value` for `error` strings and return `AuthFailed` / `EnvelopeVersionMismatch` / `UnknownError` accordingly. ~15 LOC + one test stand-up.
2. **M-V4-2** — emit `LogSessionEvent("invalidated", ...)` from `InvalidateByPlayerIdInternal` and the cleanup sweep. ~4 LOC.

**Before launch:**
3. **L-V4-2** — `BCryptGenRandom` failure → Release-fatal, or startup self-test.
4. **L-V4-6** — emit `LogSessionEvent("expired", ...)` from `CleanupExpiredInternal` per-iteration drop.
5. **L-V4-7** — gate `SeedAccounts` body with `#ifndef NDEBUG` so Release rejects `--seed-accounts`.
6. **M-V4-6** — `Crypto::FromHex` odd-length guard / propagated `optional`.

**Eventually:**
7. PBKDF2 600k bump pre-launch (L-V4-1).
8. ServiceClient backoff jitter (L-V4-10) when scaling Account replicas.
9. Quest-token secret heap zeroization + rotation (L-V4-8).
10. MAC-rejection log volume rate-limit (L-V4-4).

---

## Assurance — what's provably correct (v4 update)

- **Crypto primitives unchanged:** PBKDF2-HMAC-SHA256 at 200k iters (constant), HMAC-SHA256 over HMAC primitive (`Crypto::HMAC_SHA256` is a stock RFC 2104 impl, verified inner/outer pad XOR with 0x36/0x5c at L289-291), SHA-256 via picosha2. Constant-time compare is byte-vector XOR-accumulate (`ConstantTimeCompare` L384-395) and hex-string XOR-accumulate (`HexEquals` L205-212). Both fail-fast on length mismatch which is acceptable since lengths are known to attackers.
- **Envelope MAC input layout:** `v || '\n' || method || '\n' || params_json || '\n' || ts`. Verified by reading `ComputeMac` L106-115 and confirmed by the round-trip test suite covering string/int/float/bool/null/nested/empty payloads with bit-flip / method-mismatch / stale-ts / wrong-secret / v-mismatch rejection.
- **Envelope wire shape:** golden-file pinned at `Common/tests/EnvelopeShapeTest.cpp:65-75`. `BuildEnvelope` is the only producer; `ServiceClient::Call` routes through it (L179). Field set is exactly {v, method, params_json, ts, mac}, enforced by the `env.size() == 5` assertion in the test.
- **Token entropy:** 32 bytes (256 bits) via `BCryptGenRandom` (`Crypto::GenerateRandomBytes` L147-160 + `GenerateSecureToken` L67-82). 24h lifetime, 30-min idle, IP-bound. Collision retry capped at 10 (V3-L10). `tokenLength` is configurable via `protocol.json` settings.
- **Session lifecycle:** every read path either runs `GetValidSession` (full check) or is documented as the bare-flag accessor. The legacy `ValidateSession` method is gone; the bare-flag `GetSession`/`GetPlayerName` are used only by `HandleLogout`/`HandleHeartbeat` for read-only fields where invariant enforcement isn't required.
- **Env-var enforcement matrix:**
  - `APHELYON_INTERNAL_SECRET`: Release-fatal in all three services (`Auth/src/main.cpp:117-122`, `Account/src/main.cpp:155-160`, `Combat/src/main.cpp:86-91`). Debug fallback `"DEBUG-aphelyon-internal-rpc-secret-do-not-ship"`.
  - `APHELYON_DB_CONNECTION`: Release-fatal in Account (`Account/src/main.cpp:212-217`). Debug fallback `postgresql://aphelyon:aphelyon@localhost:5432/aphelyon` with a startup WARN. Test fixtures own their own literals.
  - `APHELYON_QUEST_TOKEN_SECRET`: Release-fatal in Account (`Account/src/main.cpp:169-182`). Debug fallback = `Crypto::GenerateSecureToken(32)` with a startup WARN.
  - `g_rateLimitingEnabled`: Release-fatal if disabled (all three services).
  - `g_authIPBindingEnabled`: Release-fatal if disabled (Auth only).
- **Three unauthenticated entrypoints:** Login (id 9), Register (id 8), ResumeSession (id 24). All three rate-limited (login + resume share the login limiter; register has its own). Resume runs full `GetValidSession` (C7-B). No other `requires_auth: false` exists in `protocol.json`.
- **Token logging:** all three `LogSessionEvent` call sites pass the full token; the hash + 8-char-prefix happens inside `Logger::LogSessionEvent`. No raw-token surface in any structured log.

---

## Agent observations

The v3 → v4 sweep is the cleanest remediation pass in this audit's history — 25 fix commits, ten security items closed, zero fixes broken, every named fix structurally correct. The lone H finding (H-V4-1) is an unfinished-design item, not a regression: the Probe enum has `AuthFailed` and `EnvelopeVersionMismatch` values that were declared with intent but never wired up. The fix is mechanical and the test cost is one Catch2 case.

Notable wins: H-V3-10's function-local static is structurally bulletproof against future init-order regressions in a way that the original v2 fix wasn't. The H-V3-11 Peek/Touch split is small, exact, and includes both mutable and const overloads for the right scenarios. The H-V3-14(a) golden-file test is the kind of test-as-contract that should be the template for future wire-shape work — it forces a re-pin in the same commit as any envelope change.

Recommend: ship H-V4-1 + M-V4-2 this week, then the security backlog is in a defensible pre-launch state with no outstanding H-class items.
