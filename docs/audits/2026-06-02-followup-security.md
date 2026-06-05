# Follow-up Audit: Security

**Date:** 2026-06-02
**Scope:** Security follow-up after `2026-06-02-server-persistence-audit.md` closed C3/C5/H6/M3/M4/M5/M8/M9/M10.
**Method:** Single security-lens pass over the five fix commits plus adjacent attack surface — Auth/Account RPC, session lifecycle, crypto primitives, secrets handling, login-flow timing.

---

## Summary

Most of the named fixes are correct in code. Two **CRITICAL** regressions/holes remain: `HandleResumeSession` completely bypasses H6 IP binding (and idle/expiry validation) by calling `GetSession` directly instead of `ValidateSession` with the client IP — a stolen token is fully usable from any IP via the auto-login path. The HMAC envelope (M3) re-serializes `params` from a parsed Json on the receive side, which is bit-exact today only because every current RPC payload is strings — the first time anyone adds a float / nested object that round-trips lossy, every internal RPC silently starts failing with `Authentication failed`. Plus a username-enumeration timing oracle in `VerifyCredentials` (no constant-time dummy hash on lookup miss) and the SessionManager helpers (`GetSession`, `GetPlayerName`) returning data without checking `IsExpired`/`IsIdle`, exposing future call sites to the same class of bug as ResumeSession.

---

## Critical

### C1. ResumeSession bypasses IP binding and full session validation

**Files:** `Server/Auth/src/AuthServer.hpp:80, 385-412`, `Server/Auth/src/SessionManager.hpp:237-246`, `Server/data/protocol.json:889-895`

**Status:** NEW (post-H6 attack surface)

`ResumeSession` is declared `requires_auth: false` so `TcpServerBase` does not run `ValidateSession` before dispatch. The handler then runs:

```cpp
std::string HandleResumeSession(const std::string& token)
{
    auto session = m_sessionManager.GetSession(token);
    if (!session || session->IsExpired())
        return CreateAuthResponse(false, "Session expired or not found", ...);
    // returns AuthResponse with the SAME session_token + service endpoints
}
```

Three independent gaps in this path:

1. **No IP check.** `GetSession` does not consult `m_config.bindToIP`. The H6 fix wires IP binding through `ValidateSession`, but `ResumeSession` does not call it. A token stolen from a user's saved auto-login state (filesystem, log scrape, malware) gives the attacker a complete `AuthResponse` from any IP, including the service endpoint list — full takeover, no rate limit on the path (`Login` has 10/min/IP, `ResumeSession` has none).
2. **No idle check.** `GetSession` returns the session if `valid` and (in `HandleResumeSession`) not expired, but `IsIdle(...)` is not consulted. A 31-day-idle session that hasn't been swept by `CleanupExpired` yet is resumable. The session's `lastActivity` is touched only by `ValidateSession`, so cleanup is best-effort; an attacker who held a token through an Auth restart (sessions survive in-memory only, so this requires no-restart, but the window is `sessionLifetimeSeconds` = 24h not `idleTimeoutSeconds` = 30min).
3. **`GetSession` itself is a footgun.** It checks `valid` only — not `IsExpired`, not `IsIdle`, not IP. Today it's used by `HandleResumeSession`, `HandleHeartbeat` (post-validation, OK), `HandleValidateSession` (re-checks `IsExpired` but not idle/IP), and `HandleLogout` (logout doesn't need full validation). The next code path that uses it without a full `ValidateSession` first will have the same bypass.

This is the **canonical session-hijack scenario** the H6 fix was supposed to mitigate, and it is wide open via the auto-login flow.

**Fix:**
- In `HandleResumeSession(token, clientIP)`: change the OnProcessMessage dispatch at `AuthServer.hpp:80` to pass `clientIP`, and run `ValidateSession(token, clientIP, bumpIdle=true)` first. Only return the AuthResponse on `Valid`; map IdleTimeout / IPMismatch / Expired to the existing "Session expired or not found" error.
- Optionally, rate-limit ResumeSession per-IP at the same grain as Login (`10/min`). The Resume path is silent-auto-login so legitimate traffic is dominated by app launches; 10/min/IP is plenty.
- Either remove `GetSession` and force callers through `ValidateSession`, or rename it `PeekSession_NoValidation` and audit every existing call.

### C2. Internal RPC HMAC bit-exact assumption is brittle by accident, not by design

**Files:** `Server/Common/src/ServiceEndpoint.hpp:151-167`, `Server/Common/src/ServiceClient.hpp:114-127`, `Server/Common/src/InternalRpcAuth.hpp:62-75`

**Status:** NEW

The endpoint signs/verifies over `method || '\n' || params.dump() || '\n' || ts`. The sender computes `params.dump()` on the in-memory `Json` it just built; the receiver computes `params.dump()` after `Json::parse(wireMsg)` then `request.value("params", Json::object())`. The comment at `ServiceEndpoint.hpp:155-159` acknowledges this:

```cpp
// Note we re-serialize `params` rather than capturing the on-wire substring —
// both sides use nlohmann::json's canonical dump(), so this is bit-exact in
// practice.
```

This is true today **only because every current RPC payload is purely strings** (`token`, `username`, `password`, `clientIP`, `playerId`). nlohmann's default `json` template uses `std::map` for objects (lex-sorted keys), so for string-valued payloads the round-trip is stable. The first time any RPC carries:

- a floating-point value (parse/dump can change representation: `1.0` ↔ `1` ↔ `1.000000`),
- a number large enough to round-trip as a different integer type,
- a nested object with non-ASCII keys (depending on locale, though nlohmann's default is locale-independent — but the assumption isn't enforced),
- an array whose entries are themselves objects with mixed types,

the receiver's `params.dump()` won't match the sender's, every call to the affected method silently fails with `Authentication failed`, and the only diagnostic is a generic LOG_NET_WARN. This will land as a bug report that looks like "internal RPC unreachable" and waste hours to chase.

Worse from a security standpoint: if a clever attacker who has the shared secret (already implied by the loopback-trust framing) can craft a payload that **parses to one thing but dumps to another** on the receiver, the MAC over the original dump is valid for the parsed-then-redumped version. Concretely, JSON allows duplicate keys; the parser keeps last, but `dump()` outputs each key once — this means the receiver-side `dump()` of a payload with `{"foo":1,"foo":2}` is `{"foo":2}`. Sender computes MAC over `{"foo":1,"foo":2}`; receiver re-dumps `{"foo":2}` and MAC over that fails. So duplicate keys break the MAC — fine. But: the inverse — a sender computing the MAC over `{"foo":2}` while sending `{"foo":1,"foo":2}` on the wire — would let an attacker route a different logical payload than the one MAC'd. Today nothing in the codebase does this, but the design is fragile.

**Fix:** Capture the on-wire `params` substring (the bytes the sender actually transmitted, not a re-serialization). Easiest is to put the MAC over the full wire envelope minus the `mac` field — e.g. serialize `{"method":...,"params":...,"ts":...}` once, hash that, then add `"mac":...` at the JSON layer. The receiver hashes the same canonical form. Alternatively, base64-encode the sender's `params.dump()` and ship it as a string field so the receiver can MAC the literal bytes without re-parsing them.

### C3. Username enumeration via VerifyCredentials timing

**File:** `Server/Account/src/AccountServer.hpp:207-217`

**Status:** NEW

```cpp
auto accountData = m_repository.LoadByUsername(username);
if (!accountData)
    return {{"valid", false}, {"error", "not_found"}};

if (!Crypto::VerifyPassword(password, accountData->passwordHash))
    return {{"valid", false}, {"error", "invalid_password"}};
```

Nonexistent username → returns after one indexed SQL lookup (~ms).
Existing username → runs `PBKDF2_HMAC_SHA256` with 200,000 iterations before returning (~150-400ms on a typical Release build, by design).

The C5 fix raised iterations 20× — which is the right call for offline brute-force resistance, but it makes the timing oracle **20× louder** than it was before. An attacker enumerating usernames by login latency now gets a clean signal: <50ms = unknown user, >100ms = known user. The Login wrapper at `AuthServer.hpp:238` exposes the timing externally too (the rate limit doesn't kick until the 10th attempt/minute, so 9 timing samples per IP per minute is plenty).

Additional finding: the internal RPC `error` field discloses `"not_found"` vs `"invalid_password"`. Auth maps both to `"Invalid username or password"` externally (`AuthServer.hpp:275`), so the wire-level disclosure is mitigated for non-loopback observers. With M3 HMAC closed, only an attacker with the shared secret can directly read this — but the timing channel works for any external attacker.

**Fix:** On `LoadByUsername` miss, run a "dummy" PBKDF2 against a constant hash with `DEFAULT_ITERATIONS` so the timing for known-good and known-bad usernames matches. Standard pattern — e.g.:

```cpp
static const std::string kDummyHash = Crypto::HashPassword("dummy-password-do-not-match", Crypto::DEFAULT_ITERATIONS);
if (!accountData) {
    (void)Crypto::VerifyPassword(password, kDummyHash);  // burn the time
    return {{"valid", false}, {"error", "not_found"}};
}
```

Cache `kDummyHash` once at startup so 200k iterations don't run twice on the first miss.

---

## High

### H1. SessionManager::GetSession / GetPlayerName / GetSessionByPlayerId leak idle and IP-mismatched sessions

**Files:** `Server/Auth/src/SessionManager.hpp:226-262`

**Status:** NEW (defense-in-depth gap exposed by C1)

`GetSession` checks `valid` only. `GetPlayerName` does the same. `GetSessionByPlayerId` only checks `valid`. None of them consult `IsExpired`, `IsIdle`, or the IP-binding policy. The C1 bypass exploits this directly, but every other call site is one merge conflict away from inheriting the same hole. The internal contract — "Valid means valid, period" — is not enforced by the API shape.

`HandleHeartbeat` calls `ProcessHeartbeat` first (which checks `IsExpired`) then `GetSession` for the expiry remaining-time, which is OK in isolation. `HandleValidateSession` checks `IsExpired` after `GetSession`, but not `IsIdle` or IP — same class of bug as C1, just less reachable.

**Fix:** Either:
- Remove `GetSession` from the public API entirely. Make every caller go through `ValidateSession(token, clientIP, bumpIdle)`, returning a session-snapshot on Valid.
- Or rename the existing functions to make the contract obvious (`PeekSessionUnverified`) and add a `GetValidSession(token, clientIP)` that runs the full check.

### H2. nlohmann::json `params.dump()` round-trip is undefined for any non-string value

**Files:** Same as C2.

**Status:** NEW (related to C2 — separated because the impact differs)

See C2 for the root cause. C2 is filed as the security-critical version (MAC bypass / silent break). The same root cause means **any future RPC that adds a numeric field has a non-zero chance of breaking authentication after a compiler / nlohmann update changes a digit of floating-point formatting.** This is a maintainability time bomb separate from the immediate hijack risk. Fixing C2 (sign the on-wire bytes, not the re-serialization) closes this too.

### H3. Internal RPC envelope re-serialization computes secret-handling work inside the JSON path

**File:** `Server/Common/src/ServiceEndpoint.hpp:160-167`

**Status:** NEW (defense-in-depth)

```cpp
const std::int64_t ts = request.value("ts", static_cast<std::int64_t>(0));
const std::string mac = request.value("mac", "");
const std::string secret = InternalRpcAuth::GetSharedSecret();
if (!InternalRpcAuth::Verify(method, params.dump(), ts, mac, secret))
```

`GetSharedSecret()` reads `std::getenv` on every RPC. `std::getenv` is **not thread-safe** under POSIX (calls to `setenv` from any thread invalidate previous return pointers). Aphelyon doesn't call `setenv` itself, but a loaded library (libpq, BCrypt initialization on Windows) might. Symptom on misbehavior: rare segfault or torn read of the secret string → spurious MAC failure or worse.

Reading the env var on every call also re-allocates a `std::string` for every RPC, which is a minor perf wart but more importantly burns the secret through `std::string`'s destructor (no `memset` zeroing — secret stays in freed-but-uncleared heap pages, increasing memory-dump exposure surface).

**Fix:** Read the secret once at startup into a process-static `const std::string`. Verify against that. Optionally lock the page with `VirtualLock` / `mlock` (the audit's framing notes memory protection is out of scope, but this is cheap).

### H4. Login limiter Reset removal (M10) is correct; ResumeSession has no rate limit at all

**Files:** `Server/Auth/src/AuthServer.hpp:240-244, 385-389`

**Status:** NEW

The M10 fix correctly removed the per-success `Reset(clientIP)` from `HandleLogin`. But `HandleResumeSession` has no `RateLimiter::Allow` gate at all — and (per C1) accepts any token-shaped string from any IP without IP binding. An attacker can spray ResumeSession with guessed tokens from a single IP indefinitely with no cooldown.

Token entropy is 256 bits (64 hex), so blind guessing is infeasible at scale. But:
- Combined with C1 (no IP check on resume), a partial-prefix leak from `LogSessionEvent` (token_prefix = 8 hex chars = 32 bits) reduces the search space.
- An attacker with one valid stolen token can probe `ResumeSession` infinitely from anywhere — useful for token-revocation oracles (does the server still know this token? a "Session expired" response confirms revocation timing).

**Fix:** Add `m_loginLimiter` (or a dedicated `m_resumeLimiter`) gate at the top of `HandleResumeSession` with the same `RateLimits::Login()` config.

---

## Medium

### M1. `LogAuthStateChange(false)` is called inside `m_mutex` in SessionCache::Validate

**File:** `Server/Common/src/SessionCache.hpp:55-100, 197-225`

The comment at line 193-196 says "Locking contract: the recovery branch acquires m_mutex itself. It's ONLY reachable from the outside-the-lock Auth-RPC-success call site." This is **true today** because the false→true transition only happens via `LogAuthStateChange(true)` at line 129, which is outside the lock. But the inside-the-lock `LogAuthStateChange(false)` calls at lines 76 and 96 and 111 are one refactor away from accidentally being called with `true` (e.g., a future "try-revalidate-then-extend" path), which would deadlock immediately under the existing `m_mutex` guard.

Defense-in-depth: factor `LogAuthStateChange` into two functions — `MarkAuthDown()` (no lock needed, only writes the atomic) and `MarkAuthUpAndDropExtensions()` (acquires the lock). Callers pick the right one.

### M2. SessionConfig::bindToIP default is `false`

**File:** `Server/Auth/src/SessionManager.hpp:24`

```cpp
bool bindToIP = false;  // Invalidate on IP change
```

The H6 fix wires the global toggle `g_authIPBindingEnabled{true}` and sets `config.bindToIP = g_authIPBindingEnabled.load()` in `CreateSessionConfig` (`AuthServer.hpp:176`). So AuthServer in production gets `bindToIP=true`. But the `SessionConfig` **struct default remains `false`** — any test, future utility, or fresh `SessionManager{}` instantiation in some other component gets IP binding off by accident. The struct default should match the production-safe value.

**Fix:** `bool bindToIP = true;` in the struct.

### M3. clientIP=="" silently bypasses IP binding

**File:** `Server/Auth/src/SessionManager.hpp:209-216`

```cpp
if (m_config.bindToIP && !clientIP.empty() &&
    session.clientIP != clientIP)
```

Empty `clientIP` short-circuits the check. The current call paths always populate `clientIP` from a real socket address, but:
- If TcpServerBase ever resolves `clientIP` from a forwarded header (X-Forwarded-For style), an attacker who can omit/blank that header bypasses IP binding.
- Internal RPC `AuthorizeToken` accepts `clientIP` from JSON params (`AuthServer.hpp:138`); if SessionCache forwards an empty string (e.g., the calling service didn't have one), the check no-ops.

**Fix:** When `bindToIP` is on and `clientIP` is empty, **reject** rather than skip — fail closed.

### M4. Quest token secret fallback warns but allows process to start

**File:** `Server/Account/src/AccountServer.hpp:367-380`

When `APHELYON_QUEST_TOKEN_SECRET` is unset in Release, the service falls back to per-process random and logs a warning. Per audit framing this is the intentional design. But two concerns:

1. The fallback secret is 32 bytes (`Crypto::GenerateSecureToken(32)`) which is 64 hex chars — **half** the byte width of a typical HMAC key. Since `Crypto::HmacSha256Hex` hashes keys longer than 64 bytes and pads shorter ones, this still produces a valid MAC but the key has only 256 bits of entropy. Adequate.
2. The WARN is logged once at startup. If an operator misses it (e.g., automated deploy that swallows WARN), production runs with non-persistent tokens forever — every restart silently invalidates all in-flight quest minigames. Consider: in Release, refuse to start without the env var (same pattern as `APHELYON_INTERNAL_SECRET`). The "10 minute window dropped on restart" is more user-facing breakage than a quiet warn deserves.

### M5. Hardcoded DB password in AccountServer constructor default + tests

**File:** `Server/Account/src/AccountServer.hpp:51`, all 5 integration tests

```cpp
std::string dbConn = "postgresql://aphelyon:aphelyon@localhost:5432/aphelyon"
```

Not in scope for the named audit fixes, but the Postgres password is a plain literal in source. If this ships without env-var override:
- The password is in the compiled binary's read-only data section, recoverable with `strings`.
- The test files commit the dev password to git history forever.

For pre-launch this is fine, but the dev DB password is a single string-replace away from a prod leak. **Fix:** Read `APHELYON_DB_CONN` env var with the existing string as Debug-only fallback (same pattern as `APHELYON_INTERNAL_SECRET`).

### M6. VerifyCredentials lazy-rehash double-work under concurrent logins

**File:** `Server/Account/src/AccountServer.hpp:211-254`

Concurrent login attempts for the same user:
- T1: `LoadByUsername(u)` → reads old hash (10k iter version)
- T2: `LoadByUsername(u)` → reads old hash (10k iter version)
- T1: verifies password (200ms PBKDF2 at 10k)
- T2: verifies password (200ms)
- T1: takes stripe lock, `NeedsRehash(old)` true, runs `HashPassword` (400ms at 200k), UPDATEs DB, releases stripe.
- T2: takes stripe lock, `NeedsRehash(accountData->passwordHash)` — but `accountData` is T2's local copy with the OLD hash, so still true. Runs `HashPassword` again. UPDATEs DB with a different salt+hash (also valid).

Effect: harmless functionally — both rehashes produce valid 200k-iteration hashes — but wastes a full PBKDF2 derivation under concurrent traffic. More worryingly, it shows the stripe lock does **not actually guard against double-rehash work**, which is the symptom; the deeper class is "TOCTOU on `accountData` snapshot" — there's nothing forcing T2 to observe T1's write.

**Fix:** After acquiring the stripe lock, re-fetch the row's `password_hash` and re-check `NeedsRehash` against the freshly-read value before re-hashing. Trivial change, eliminates the double-work.

### M7. Token prefix in `LogSessionEvent` is logged on every validated message

**File:** `Server/Common/src/Logger.hpp:216-226`, `Server/Auth/src/SessionManager.hpp:221`

`LogSessionEvent("validated", playerId, token)` runs on every successful `ValidateSession`, recording the first 8 hex chars (32 bits) of the token. For a high-traffic player this writes a session_validated line per RPC, so logs accumulate (player → prefix) bindings forever. An attacker who later obtains a log dump can:
- Confirm a specific player was online at a specific time (1 in 2^32 false positives across players is effectively zero).
- Correlate prefix observations to narrow down player identity if combined with metadata (IP, message timestamps).

Not a vuln on its own, but the per-message frequency is excessive — most session loggers emit one event per **session**, not per message.

**Fix:** Drop the per-validate logging to `LogCategory::Trace` or remove the token_prefix field from the validated event.

### M8. RateLimiter aggressive-sweep fails closed under sustained 10k+ active-IP attack

**File:** `Server/Common/src/RateLimiter.hpp:59-69`

When the map hits `MAX_RECORDS` (10k) and the aggressive sweep finds nothing >30s old, new entries are refused (`return false`, treating the request as rate-limited). Audit framing says "the safest default — error on the side of blocking unknown attackers vs. memory blow-up." This is the right call for memory safety.

But: under a sustained 10k-active-IP attack where every IP refreshes within 30s, **every legitimate new IP is also refused**. Effectively a single attacker can deny new connections globally. The login limiter for the legitimate user's IP `192.0.2.42` returns `false` not because that IP misbehaved but because the map is full of attacker traffic.

Standard mitigation: instead of a flat global cap, partition limiters per-/24 or per-/16 (so an attacker IP-spraying within one block can't evict legitimate users in a different block). Or pair the global cap with a separate "reservation" pool for never-seen IPs that bypasses the cap up to some smaller quota.

Today's traffic is far from 10k IPs / 30s so this is a defense-in-depth gap, not an active vulnerability. Worth a comment in the code, at minimum.

### M9. `std::getenv` thread-safety in InternalRpcAuth::GetSharedSecret

**File:** `Server/Common/src/InternalRpcAuth.hpp:44-54`

Covered under H3 — same root cause. Filed here separately because the maintenance impact is lower-severity than the secret-exposure-via-string-destructor angle.

---

## Low / Observation

- **L1.** `Crypto::HexEquals` early-returns on size mismatch — the audit framing accepts this. Worth noting the comment is accurate (length is wire-format-known to the attacker). No fix needed.
- **L2.** `Crypto::GenerateSecureToken` falls back to `std::random_device` on BCrypt failure with only a WARN. `std::random_device` is **not** guaranteed CSPRNG on all libstdc++ versions (some platforms it's deterministic). For Windows this is moot (BCrypt rarely fails), but worth either making it fatal or testing the fallback path.
- **L3.** `Crypto::PBKDF2_F` XORs into a result vector inside the iteration loop. Correct, but for keyLength=32 the standard PBKDF2 short-circuits (blockCount=1, no XOR-over-multiple-blocks). The current code handles both correctly.
- **L4.** No RFC 4231 test vectors in the test suite for `HmacSha256Hex`. The code reads correct (block size 64, ipad 0x36, opad 0x5c, key-hash-when-too-long), but golden-vector tests would gate future refactors.
- **L5.** `CreateSessionConfig` reads `g_authIPBindingEnabled.load()` **once at AuthServer construction**. The Debug-only `--no-ip-bind` flag must be set before AuthServer construction (which `main.cpp` does correctly). Worth a comment near the global so future code knows the snapshot semantics.
- **L6.** `SessionManager::GenerateToken` collision check uses `find` on `m_sessions`. With 256-bit tokens the collision probability is negligible (~2^-128 birthday bound), but the `while` loop has no max-retry — a theoretical pathological RNG could spin forever. Cap at e.g. 10 retries and throw.
- **L7.** `ParseStoredHash` accepts negative iteration counts as long as `stoi` succeeds and `iterations > 0`. The check `iterations > 0` saves it. Marginal — already correct.
- **L8.** `Crypto::DEFAULT_ITERATIONS = 200000` is below the OWASP 2023 recommendation of 600,000 for PBKDF2-SHA256. The comment acknowledges this. For a 2026-launch consumer game the 200k floor is defensible; for a 2027-launch I'd revisit.
- **L9.** The internal-RPC `error` field on MAC failure is the literal string `"Authentication failed"` — fine, but a malicious local attacker could use it as an oracle for "is the secret rotation in progress yet" by spamming with old secrets. Mitigation is just "rotate without overlap"; noting for the runbook.
- **L10.** `m_rateLimitingEnabled` and `g_authIPBindingEnabled` are global atomics. A future test that wants to flip these between test cases has no encapsulated API — risk of test pollution. Wrap in a RAII guard if/when tests need it.

---

## Verified Closed

1. **C3 (rate limiting default-on):** `g_rateLimitingEnabled{true}` at `RateLimiter.hpp:24`; Debug `--no-rate-limit` flag wired in all three `main.cpp`; Release refuses to start when disabled (Auth/Account/Combat `main.cpp` `#ifdef NDEBUG` branch).
2. **C5 (PBKDF2 200k):** `DEFAULT_ITERATIONS = 200000` at `Crypto.hpp:41`. `NeedsRehash` + `IterationsOfStoredHash` helpers exposed. `UpdatePasswordHash` in `AccountRepository.hpp:215` performs targeted UPDATE, logs the upgrade.
3. **C5 (lazy rehash):** Sequence at `AccountServer.hpp:243-254` runs under the stripe lock acquired at line 235 (after the slow PBKDF2 verify, so concurrent legit logins don't queue). Cached Account's hash updated under map lock.
4. **H1 (stripe lock for VerifyCredentials):** `m_playerLocks.LockFor(accountData->id)` at `AccountServer.hpp:235`, taken before any cache mutation.
5. **H6 (IP binding end-to-end):** `clientIP` plumbed through `SessionCache::Validate` → `AuthorizeToken` RPC → `SessionManager::ValidateSession`. `g_authIPBindingEnabled{true}` default, Release refuses to start when disabled. **Note:** C1 finding above shows ResumeSession bypasses this entirely.
6. **H6 (auth-down extension cap):** `MAX_AUTH_DOWN_EXTENSIONS = 3` at `SessionCache.hpp:40`, enforced at lines 70-90.
7. **H6 (force-revalidate on Auth recovery):** `LogAuthStateChange(true)` branch at `SessionCache.hpp:197-219` drops all extended sessions.
8. **M3 (internal RPC HMAC):** `InternalRpcAuth::ComputeMac` / `Verify` over `method || \n || params.dump() || \n || ts`. Sender at `ServiceClient.hpp:117-127`, verifier at `ServiceEndpoint.hpp:153-167`. 30s skew window at `InternalRpcAuth.hpp:38, 89-91`. **Caveats:** C2/H2 (re-serialization brittleness) and H3 (per-call getenv).
9. **M3 (env var required in Release):** `IsSecretConfigured()` check in all three `main.cpp`; Debug fallback to `"DEBUG-aphelyon-internal-rpc-secret-do-not-ship"`.
10. **M4 (quest token HMAC):** `HmacSha256Hex` at `QuestHandlers.hpp:159` (mint) and `:822` (verify). `Crypto::HexEquals` constant-time compare at `:823`. Old `secret || msg` raw-SHA-256 length-extension surface is gone.
11. **M5 (quest token secret env var):** `APHELYON_QUEST_TOKEN_SECRET` read at `AccountServer.hpp:367-380`, WARN on unset, per-process random fallback. **Caveat:** M4 finding above (Release should refuse to start without it).
12. **M8 (RateLimiter cap + sweep):** `MAX_RECORDS = 10000` + `AggressiveSweep(now)` evicting >30s entries. `Allow` returns `false` if still at cap. **Caveat:** M8 finding above (global cap allows attack-induced DoS).
13. **M9 (idle timer not bumped on heartbeat):** `bumpIdle = (messageType != m_idHeartbeat)` at `AuthServer.hpp:55`. Plumbed through `SessionManager::ValidateSession(token, clientIP, bumpIdle)`. `ProcessHeartbeat` does **not** call `Touch`.
14. **M10 (no Reset on successful login):** `m_loginLimiter.Reset(clientIP)` removed from successful `HandleLogin`. Comment at `AuthServer.hpp:283-289` explains the reasoning. **Caveat:** H4 finding above (ResumeSession has no rate limit at all).

---

## Assurance — what new security guarantees the recent fixes provide

- **Brute-force resistance:** PBKDF2 at 200k iterations + 10 logins/min/IP + no per-success-reset = an attacker spraying credentials against a known username pays ~150ms × 10/min = ~25s of compute per minute per IP, capped by the rate limiter cooldown. Offline brute force against a stolen `password_hash` column is 20× harder than before C5.
- **Internal RPC authenticity:** Loopback alone is no longer the trust boundary. A local non-Aphelyon process that opens 127.0.0.1:7770 cannot impersonate AuthorizeToken/CreateAccount/VerifyCredentials without `APHELYON_INTERNAL_SECRET`. The 30s skew window bounds replay of captured envelopes to that interval. (C2 above qualifies the canonicalization assumption.)
- **Quest token length-extension closed:** HMAC-SHA256 replaces raw `SHA-256(secret || message)`. Constant-time hex compare closes the per-byte timing channel on token verification.
- **Session takeover via IP change:** For the **active** session path (`ValidateSession` via Account/Combat/Auth ProcessMessage), IP binding fires. (C1 above qualifies this for the ResumeSession path.)
- **Auth-partition tolerance:** Bounded — a revoked token under Auth outage stays valid for at most 3 × 60s = 3 minutes, and on Auth recovery all extended sessions force re-validation.
- **Rate-limiter DoS-via-IP-spray:** Bounded to 10k entries with aggressive sweep + fail-closed. Memory pressure cannot blow the service up via map inflation.
- **Idle-timer correctness:** A client whose game is open in the background and only sending heartbeats now actually idles out within `idleTimeoutSeconds` (30 min default). Pre-fix, a heartbeat resurrected the idle timer forever.
- **Successful-login does not reward attackers:** A brute-forcer who happens to land a valid credential continues to be rate-limited from the same IP — no free reset.

The five named items hold up under direct examination. The two CRITICAL findings (C1 ResumeSession, C2 HMAC canonicalization) are about adjacent attack surface the fixes did not touch but are now load-bearing; they need to be closed before the H6/M3 guarantees are real-world meaningful.
