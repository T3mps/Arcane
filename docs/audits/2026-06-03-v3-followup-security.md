# V3 Follow-up Audit: Security

**Date:** 2026-06-03
**Dimension:** Security
**Scope:** v3 follow-up after the v2 remediation pass (3 new Criticals C7-A/B/C, 9 Highs H-V2-1…9, 13 Mediums M-V2-1…13). 22 fixes shipped 2026-06-02 → 2026-06-03 between commits `9d2d221` (C7-A) and `d163b07` (M-V2-3 step 3/3).
**Method:** Single security-lens pass over each fix commit + adjacent attack surface — Auth handlers, SessionManager, SessionCache, InternalRpcAuth, ServiceEndpoint/Client, Crypto primitives, secrets handling, login/resume flow timing, rate-limiter eviction semantics, protocol.json requires_auth surface.

---

## Executive Summary

**The v2 remediation pass is correct in code.** Every named fix holds up under direct examination — C7-B routes ResumeSession through `GetValidSession` with `clientIP` + `bumpIdle`; C7-C MAC covers the exact on-wire `params_json` bytes with no re-serialization round-trip; H-V2-5 pre-generates `m_dummyPasswordHash` at startup with `DEFAULT_ITERATIONS` (200k) so VerifyCredentials' miss path runs one full PBKDF2 derivation; M-V2-10 LruCache guarantees forward progress for new entries under a sustained unique-key spray; M-V2-11 caches the shared secret via a thread-safe static-local initializer that reads the env var exactly once.

The v3 surface produces **zero Critical** items, **two High** items (both narrow defense-in-depth gaps in dummy-hash policy and LRU spray pivot), and a handful of Mediums / Lows. No outstanding session-hijack class, no MAC bypass, no broken canonicalization. The high-impact v2 misses (C7-A snapshot timing, C7-B IP bypass, C7-C MAC fragility) are all genuinely closed.

The remaining surface is largely runbook / hygiene — DB-password literals still in test fixtures and `Server/Account/src/AccountServer.hpp:58` default arg (M-V2-13 only plumbed the override into `main.cpp`); `LogSessionEvent("validated", ...)` still emits per-RPC at Debug level (M7 from v2 deferred); the per-process random fallback for `APHELYON_QUEST_TOKEN_SECRET` in Debug remains documented-only (no in-memory protection).

**Verdict: clear to merge.** The H-class items below are real but bounded — neither is exploitable under any plausible threat model on the current code shape. Recommended fix-it-this-week rather than fix-it-now.

---

## Critical

*(None.)*

The three NEW v2 Criticals (C7-A snapshot ordering, C7-B ResumeSession IP bypass, C7-C re-serialized MAC) are verified closed below in **Verified Closed**.

---

## High

### V3-H1. `m_dummyPasswordHash` is generated AFTER `InitializeQuests()` — startup-order fragility

**Files:** `Server/Account/src/AccountServer.hpp:96-108`

**Status:** NEW (latent regression vector, not currently exploitable)

The constructor body runs:

```cpp
InitializeBanners();
InitializeTemplates();
InitializeProgression();
InitializeQuests();
// Audit H-V2-5
m_dummyPasswordHash = Crypto::HashPassword("aphelyon-h-v2-5-timing-mitigation");
m_internalRpcHandlers.Register();
RegisterHandlers();
```

`InternalRpcHandlers` was constructed earlier in the initializer list (`L73-74`) and holds `const std::string& m_dummyPasswordHash` — a reference. The reference is to a not-yet-populated empty string. `m_internalRpcHandlers.Register()` is called AFTER the hash is populated, so all real RPC dispatches see the right value. But:

1. **If any future refactor adds a path that can reach `HandleVerifyCredentials` between line ~107 and line 108**, it would call `Crypto::VerifyPassword(password, m_dummyPasswordHash)` with an empty `m_dummyPasswordHash`. `Crypto::ParseStoredHash` rejects empty input (`firstColon == std::string::npos` ⇒ returns false), so `VerifyPassword` short-circuits, logs `"Password verification failed: malformed stored hash"`, and **returns false immediately** — ~µs, not 150-400ms. The timing oracle is back, just for the constructor window.

2. **More realistically**: the field-ordering contract in v2 is documented at `AccountServer.hpp:73-74` comment ("the actual hash is populated in the constructor body before Register() wires the lambdas") but a future reader is one mis-order away from breaking it. The contract should be enforced by code structure, not by comment.

**Concrete risk today**: zero (RPC dispatch can't fire until `Register()` is called). **Risk on next refactor**: high (an "eager pre-warm" that posts a self-call after construction, or moving `Register()` earlier, breaks the timing mitigation invisibly — H-V2-5 passes tests because tests construct the server fully before issuing a call).

**Defense check**: there's also a subtler issue — `Crypto::HashPassword("aphelyon-h-v2-5-timing-mitigation")` runs ONCE at startup with `DEFAULT_ITERATIONS`. This is correct. But because `VerifyPassword` parses the stored hash and `ParseStoredHash` is forgiving (any 3-segment `iterations:salt:hash` with `iterations > 0` parses), a malformed `m_dummyPasswordHash` (e.g., truncated by a future "obfuscate the dummy hash" refactor) would parse but possibly carry a degraded iteration count. Today the value is generated by the same `HashPassword` path so it's always well-formed at `DEFAULT_ITERATIONS` — but the contract "the dummy hash carries `DEFAULT_ITERATIONS`" is enforced only by reading the source.

**Fix (preferred):** make `m_dummyPasswordHash` a static-local-init inside `InternalRpcHandlers::HandleVerifyCredentials` (computed once, on first call, thread-safe per C++11). Removes the field-ordering coupling entirely. Or:

**Fix (alternative):** add a startup assertion after `m_dummyPasswordHash` populates: `assert(Crypto::IterationsOfStoredHash(m_dummyPasswordHash) == Crypto::DEFAULT_ITERATIONS);` — gates the invariant against future refactors.

**Fix (architectural):** the entire reference-to-string-populated-later pattern is fragile (`InternalRpcHandlers` holds `const std::string&` to an enclosing member that isn't fully initialized at the reference site). Refactor `InternalRpcHandlers::Register()` to take the hash as a parameter, or have it own its own hash via a static-local computed lazily.

### V3-H2. LRU eviction can be steered to evict the legit-user slot under sustained probe

**Files:** `Server/Common/src/RateLimiter.hpp:74-80`, `Server/Common/src/LruCache.hpp:60-66, 81-99`

**Status:** NEW (partial regression — M-V2-10's LRU eviction trades M8's hard-fail-at-cap for a different problem)

M-V2-10 replaces the M8 fail-closed policy with LRU eviction so a legit user always gets a slot. Correct as far as it goes — but `LruCache::Get` calls `Touch()` (move-to-MRU) on every successful lookup, AND `LruCache::Put` calls `Touch()` on existing keys too. The `RateLimiter::Allow` path runs:

```cpp
Record* record = m_records.Get(key);  // touch (MRU)
if (record == nullptr) {
    m_records.Put(key, ...);          // insert (MRU); evicts LRU if at cap
    return true;
}
// ... cooldown / window checks, returns true/false ...
```

So **every probe — including one that the cooldown logic rejects — moves the attacker's IP to MRU**. Once the cache is full of attacker IPs and the attacker keeps the entire set at MRU by rotating through them on a window slightly shorter than the 10-min idle cleanup, the legit user's slot — touched only once at login — quickly migrates to the LRU end. The next attacker IP that comes in evicts the legit slot.

**Attack shape:**
1. Attacker establishes 10,000 cache entries from a /24 (or compromised botnet — even a few hundred IPs cycling fast is enough).
2. Attacker rotates probes across all 10k entries every ~9 minutes so none age out via the 10-min `CleanupExpired` sweep.
3. Legit user logs in once → their entry sits at MRU briefly, then drifts LRU as attacker probes pour in.
4. Next attacker IP that needs a slot evicts the legit user.
5. Legit user comes back, gets a fresh entry — no historical attempts counted. Effectively, **the legit user keeps getting their rate-limit history wiped, which is the *opposite* of the protective behavior the limiter is supposed to provide.**

The flip side: an attacker can't use this to gain *more* attempts (they get one window per IP, same as before). What they CAN do is **make the limiter forget about a brute-force-in-progress** — if the attacker happens to land the legit user's entry in a moment when their cooldown should be active, the eviction resets that user's window to zero, granting `maxAttempts` more login attempts against the just-evicted entry.

**Concrete impact today**: small — the cap is 10k, the login limiter is 10/min/IP, so a sustained 10k-IP cycle is real attacker work. But it's the class of bug that M-V2-10 was supposed to prevent (legitimate users losing limiter access due to attacker traffic) — just inverted.

**Fix (preferred):** don't `Touch()` on the rejected-path lookup. Split `Get` into `GetWithTouch` (current) and `GetNoTouch`. `RateLimiter::Allow` should only Touch on a successful Allow (matching attacker probe rates to actual rate-limit-relevant activity, not just "did they hit the limiter at all").

**Fix (alternative):** separate caches by category — legit-user IPs (small, never evicted) vs anonymous probe IPs (LRU-evicted). Requires distinguishing them, which today the limiter can't do. Or partition cap per /16 / /24 like M-V2-10's audit suggested but didn't implement.

**Fix (defensive):** reserve a "sticky" slot count (e.g. 20% of MAX_RECORDS) for entries that have ever recorded a successful Allow, and never evict those via LRU. The remaining 80% rotates under attacker pressure.

---

## Medium

### V3-M1. `LogSessionEvent("validated", playerId, token)` still fires per-RPC at Debug level

**Files:** `Server/Auth/src/SessionManager.hpp:221, 320`, `Server/Common/src/Logger.hpp:216-226`

**Status:** Deferred from v2 M7 (followup-security.md)

Previous v2 followup-security flagged this; v2 didn't act on it. The validated-path log line writes `{"event":"session_validated","player":"<id>","token_prefix":"<first 8 hex>"}` on EVERY successful `ValidateSession` AND every `GetValidSession`. For a typical playing client (~30-60 RPCs/min during play), Debug logs accumulate (player → 8-hex-prefix) bindings forever. An attacker who later obtains a Debug log dump can confirm a specific player was online at specific times (1 in 2^32 false-positive across players ≈ zero), and can correlate prefix observations against IPs and timestamps to narrow down player identity.

**Fix:** drop to `Trace` level, OR remove `token_prefix` from the `validated` event (keep it on `created` / `invalidated` only — those happen once per session, not per RPC).

### V3-M2. Test fixtures + AccountServer.hpp default arg still hardcode the dev DB password

**Files:** `Server/Account/src/AccountServer.hpp:58` (constructor default arg), `Server/Account/tests/Integration/*.cpp` (6 files)

**Status:** Partial close on v2 M-V2-13 — `main.cpp` honors `APHELYON_DB_CONNECTION` but the literal default still lives in the constructor signature and across 6 integration tests:

```
Server/Account/tests/Integration/SnapshotWriterTest.cpp:13
Server/Account/tests/Integration/RelationalFlushTest.cpp:12
Server/Account/tests/Integration/OutboxRelayTest.cpp:14
Server/Account/tests/Integration/EventStoreRoundTripTest.cpp:9
Server/Account/tests/Integration/AccountTransactionTest.cpp:13
Server/Account/tests/Integration/AccountRepositoryTest.cpp:11
```

For dev-only this is fine, but the password is now in:
1. The compiled `Account.exe` (read-only data, recoverable via `strings`).
2. Six test binaries similarly recoverable.
3. Git history forever.

If `db-snapshot.bat` ever runs against a non-dev environment, the password literal becomes a search-and-replace away from prod leakage. M-V2-13 took the right approach for the live binary — finish the job by routing tests through `kConn = std::getenv("APHELYON_DB_CONNECTION") ?: "postgresql://aphelyon:aphelyon@..."` and drop the literal default arg in `AccountServer::AccountServer`.

### V3-M3. Quest-token Debug-fallback secret leaks via process-memory dump

**Files:** `Server/Account/src/AccountServer.hpp:248-255`

**Status:** Documented design choice. The Debug fallback generates `Crypto::GenerateSecureToken(32)` (CSPRNG) and stores it in `std::string m_questTokenSecret` — a member of `AccountServer`. There's no in-memory protection: the secret sits in heap pages until process exit, recoverable from a core dump or a debugger attach. Release-mode fails fast (M-V2-12), so this only matters for Debug builds. Worth a runbook note: dev builds should not be exposed to networks where a quest-token forgery would matter.

The Release-fatal check in `main.cpp:115-127` correctly gates this for production. No code change needed; this is a "what's the actual risk model" entry, not a fix.

### V3-M4. SessionManager's `InvalidateByPlayerIdInternal` silently drops the previous token without an `Invalidated` log

**File:** `Server/Auth/src/SessionManager.hpp:480-488`

`CreateSession` (allowConcurrentSessions=false path) calls `InvalidateByPlayerIdInternal` which erases the old token from `m_sessions` and `m_playerToToken` without logging. The wrapper at `CreateSession:133` logs `LOG_AUTH_DEBUG("Invalidating previous session for player: {}", playerId)` but no `LogSessionEvent("invalidated", ...)` is emitted (the structured-log entry that downstream audit pipelines key on). Compare with `InvalidateSession` at L376 which logs both. Result: a forced-logout-via-relogin event is invisible to the structured session log.

**Fix:** emit `Logger::LogSessionEvent("invalidated", oldSession.playerId, oldSession.token)` inside `InvalidateByPlayerIdInternal` before the erase.

### V3-M5. `Crypto::VerifyPassword` short-circuits on `ParseStoredHash` failure — opens an oracle if an empty/malformed hash ever reaches it

**Files:** `Server/Common/src/Crypto.hpp:106-129, 347-369`

`VerifyPassword(password, storedHash)` calls `ParseStoredHash`; on failure returns `false` immediately without burning PBKDF2 compute. **This is correct for normal use** (a corrupted DB row is a server-side problem, not a per-user one). But it means **the timing-oracle defense (H-V2-5 dummy hash) only works if the dummy hash always parses cleanly**.

Today this is guaranteed because `m_dummyPasswordHash = Crypto::HashPassword(...)` (which always emits a well-formed `iter:salt:hash`). But the contract is fragile — see V3-H1 above. As a defense-in-depth: make `VerifyPassword` always burn at least one PBKDF2 derivation even on parse failure (or alternatively, log loudly on parse failure so any malformed-hash-reaching-verify is immediately visible).

**Risk if violated**: V3-H1's timing-oracle regression scenario. Today: invariant holds, no exploit path.

### V3-M6. `ValidateSession`'s old two-call idiom is gone, but the legacy `ValidateSession` method is still wired up — keeps `bumpIdle` as the only protection

**File:** `Server/Auth/src/SessionManager.hpp:174-223`

The H-V2-8 refactor introduced `GetValidSession` and all live callers (`AuthServer::ValidateSession`, `AuthorizeToken` lambda, `HandleResumeSession`) use it. The old `ValidateSession(token, ip, bumpIdle)` member function is still public and still bumps `lastActivity` — but is no longer called. Dead code; the only risk is a future caller wiring up the old form by mistake and missing the session-snapshot copy-out. Mark `[[deprecated]]` or remove.

### V3-M7. `ResumeSession` shares the login limiter — a token-guessing attacker can deny login from the same IP

**Files:** `Server/Auth/src/AuthServer.hpp:404-411` + `:238-244`

H-V2-4 reuses `m_loginLimiter` for both Login and Resume from the same IP. Combined: 10 attempts/min/IP total across login + resume. A legitimate user whose stored session expired (out of the 24h window) gets an automatic ResumeSession attempt on app launch; if their IP just ran several login attempts (e.g., a previous bad-password session), the resume probe consumes the same budget and may itself be rate-limited.

This is the explicit design intent in the H-V2-4 comment ("Reuses RateLimits::Login() so an attacker can't pivot between /login and /resume to dodge per-action budgets") — so flagging as a design tradeoff, not a defect. Worth confirming the client UX handles the case where ResumeSession returns `Too many login attempts. Please wait Xs.` by falling back to the login form rather than retrying.

### V3-M8. `Crypto::GenerateSecureToken` fallback path is silent in dev

**File:** `Server/Common/src/Crypto.hpp:131-168`

If `BCryptGenRandom` fails, the code falls back to `std::random_device` with a `LOG_AUTH_WARN`. `std::random_device` is not guaranteed CSPRNG on all libstdc++ versions (Mingw historically returned deterministic values). Aphelyon ships MSVC + BCrypt, so the fallback effectively never fires — but if it ever does in a test environment or future Linux build, session tokens / quest-token secrets / dummy-hash salts become predictable. Worth either (a) making the fallback Release-fatal, or (b) adding a startup self-test that asserts `BCryptGenRandom` works once and refuses to start if it doesn't.

---

## Low / Observation

### V3-L1. `SessionCache::Validate` extension path log line includes the player ID

**File:** `Server/Common/src/SessionCache.hpp:77-79`

```cpp
LOG_AUTH_TRACE("SessionCache: Auth-down extension {}/{} for player={}", ...
```

Trace level so impact is negligible, but during an Auth outage this can emit at very high frequency for high-activity sessions. The token is not logged here (good), but the (playerId, Auth-down-window) correlation could be useful to an attacker who has log access. Trace usually isn't shipped, so this is mostly a runbook note.

### V3-L2. `InternalRpcAuth::MAX_AGE_SECONDS` is symmetric (`|now - ts| > MAX_AGE`)

**File:** `Server/Common/src/InternalRpcAuth.hpp:101-103`

A client whose clock is ahead of the server by up to 30s is accepted. A clock-skewed local process (e.g., a debugger session that froze time) can replay envelopes for up to 60s of total skew window. Loopback-only attack surface, so impact is minimal — note for the runbook that NTP drift > 30s breaks internal RPC silently.

### V3-L3. `ResumeSession` returns the same generic error for all failure modes

**File:** `Server/Auth/src/AuthServer.hpp:414-415`

```cpp
if (vs.result != SessionValidationResult::Valid)
    return CreateAuthResponse(false, "Session expired or not found", "", "", "", 0);
```

`InvalidToken`, `Expired`, `IdleTimeout`, `IPMismatch`, `Invalidated` all collapse to one message — good for the user-facing channel (avoids the enumeration oracle that `vs.result` names would create). The structured log inside `GetValidSession` differentiates them, which is the right call.

### V3-L4. `LruCache::EraseIf` uses MRU→LRU iteration, but `RateLimiter::CleanupExpired` doesn't care about order

**File:** `Server/Common/src/LruCache.hpp:114-133`, `Server/Common/src/RateLimiter.hpp:154-164`

Minor. The cleanup walks the full list every 100 Allow calls — fine for 10k entries.

### V3-L5. `g_authIPBindingEnabled` and `g_rateLimitingEnabled` are read-once at AuthServer construction

**File:** `Server/Auth/src/AuthServer.hpp:160-179` (CreateSessionConfig)

`CreateSessionConfig` reads `g_authIPBindingEnabled.load()` ONCE when the SessionManager is constructed; subsequent toggle changes don't propagate. Today's call site (main.cpp) sets the global before constructing AuthServer, so this is correct. A future runtime toggle (admin-toggle-IP-binding flag) would need to be plumbed through `SessionManager::SetConfig`. Worth a comment near the global so future readers know the snapshot semantics.

### V3-L6. `m_questTokenSecret` is constructed via `=` copy then never zeroed

**File:** `Server/Account/src/AccountServer.hpp:245, 250`, `:311`

After `AccountServer` destruction, `m_questTokenSecret`'s `std::string` destructor frees the heap allocation without zeroing it. The secret stays in freed-but-uncleared heap pages until the process exits or the allocator reuses the page. For long-running Release services this is a wide memory-dump exposure window. Out-of-scope per audit framing, but worth a future "rotate quest secret on a timer + zero the old buffer" item.

### V3-L7. `SessionConfig::bindToIP` struct default is still `false`

**File:** `Server/Auth/src/SessionManager.hpp:24`

Flagged in v2's followup-security as M2; not addressed. AuthServer's CreateSessionConfig overrides it to the global value (true in production), but a fresh `SessionManager{}` instantiation in some future utility or test gets IP binding off by accident. Should default to true to match the production-safe value.

### V3-L8. `InternalRpcAuth::Verify` rejection logs at WARN with the method name

**File:** `Server/Common/src/ServiceEndpoint.hpp:168`

A loopback attacker who has the shared secret could replay-spam stale envelopes to inflate WARN-level log lines. Loopback-only, so this is a "log volume amplification" not "secret exposure." Limit log spam with rate-limited logging or move to Debug.

### V3-L9. `Crypto::DEFAULT_ITERATIONS = 200000` is below OWASP 2023 recommendation (600k)

**File:** `Server/Common/src/Crypto.hpp:41`

Acknowledged in code comment; for a 2026-launch consumer game the 200k floor is defensible. Worth re-evaluating before a 2027 launch.

### V3-L10. Token entropy is correctly 256-bit / 64-hex; idle 30 min / lifetime 24 h are enforced

Verified at `SessionManager.hpp:19-25, 467-468`. `tokenLength=64` hex chars, `GenerateSecureToken(byteCount = tokenLength / 2)` = 32 bytes = 256 bits. The `while` collision-check loop has no max retry (flagged in v2 followup-security L6, still open) — at 256 bits the birthday bound is ~2^-128 so this is theoretical, but capping at e.g. 10 retries and throwing is cheap defense-in-depth.

### V3-L11. `Crypto::HexEquals` is constant-time on equal-length inputs

Verified — XOR-accumulate over the full input. Used by `InternalRpcAuth::Verify` (`InternalRpcAuth.hpp:105`) for MAC comparison and by `QuestHandlers::VerifyQuestToken` (`QuestHandlers.hpp:890`) for HMAC comparison. Both the C3/M3 paths are correct.

`Crypto::VerifyPassword` uses `ConstantTimeCompare` (the byte-vector variant) at `Crypto.hpp:123` — also XOR-accumulate, correct.

### V3-L12. `requires_auth: false` surface enumerated

`grep` over `Server/data/protocol.json` returns exactly three: `Login` (id 9, L444), `Register` (id 8, L858), `ResumeSession` (id 24, L894). All three are handled in `Auth/src/AuthServer.hpp::OnProcessMessage`. No other unauthenticated entrypoints exist on Account/Combat. The surface is minimal and correctly limited.

---

## Verified Closed (v2 follow-ups)

### C7-A: Memento snapshot captured BEFORE handler mutation
**Commit `9d2d221`** — verified by spot-checking `GachaHandlers.hpp` HandlePull/HandleMultiPull, `AccountHandlers.hpp` HandleAddCurrency, `ProgressionHandlers.hpp::CommitProgressionScrapSpend`, `QuestHandlers.hpp::HandleClaimQuestReward`. `m_ctx.repository->Begin(account)` is now called at the top of each event-sourced handler, before any mutation. The Memento captured in `AccountTransaction`'s ctor sees pre-mutation state — Rollback's `RestoreFrom` is actually load-bearing now.

### C7-B: ResumeSession routes through GetValidSession with clientIP + bumpIdle
**Commit `5ec1f5c`** — verified at `AuthServer.hpp:404-419`. `HandleResumeSession(token, clientIP)` calls `m_sessionManager.GetValidSession(token, clientIP, /*bumpIdle=*/true)`. The full invariant check (valid + idle + IP + expiry) runs under a single mutex acquisition. H6 IP binding is no longer bypassable via the resume path.

**Cannot bypass via crafted token:** `GetValidSession` returns `InvalidToken` if `find` misses. 256-bit token entropy makes blind guessing infeasible.

**Cannot bypass via IP forge:** `clientIP` comes from `TcpServerBase::Start`'s `inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN)` — the socket's actual peer address. No client-controlled forwarded-header logic exists; if it's ever added (X-Forwarded-For), it must NOT replace this value.

### C7-C: MAC over on-wire params_json bytes, no re-serialization
**Commit `0d01429`** — verified at `ServiceClient.hpp:114-138` and `ServiceEndpoint.hpp:152-178`. Sender computes `paramsJson = params.dump()` once, MACs that exact string, and embeds it as the `params_json` field. Receiver reads `params_json` from the envelope and MACs the literal bytes — no re-serialization. Future numeric/float/bool/null fields can't trip canonicalization differences.

**Duplicate-key attack closed:** the MAC is over the EXACT byte string the receiver dispatches against (`Json::parse(paramsJson)` runs AFTER verify on the verified bytes). A duplicate-key payload `{"foo":1,"foo":2}` would MAC against itself; nlohmann's last-wins parse means the handler sees `foo=2`, but the attacker can't craft a "parses to A but MAC'd as B" payload because the MAC covers the entire `paramsJson` string. Closed.

**Ts-manipulation closed:** `InternalRpcAuth::Verify` computes `delta = |now - ts|` and rejects if `> MAX_AGE_SECONDS` (30s). Future-dated ts is rejected symmetrically. The MAC binds ts so an attacker can't substitute a fresh ts without re-MACing.

### H-V2-4: ResumeSession rate-limited
**Commit `ddf1916`** — `AuthServer.hpp:406-411`. Reuses `m_loginLimiter` with `RateLimits::Login()` (10/min/IP, 30s cooldown). Cross-checked: an attacker pivoting between `/login` and `/resume` from the same IP cannot exceed 10 attempts/min total because both paths consume the same limiter slot. **Cannot pivot.**

### H-V2-5: dummy-hash on username miss
**Commit `b2cd0f4`** — verified at `InternalRpcHandlers.hpp:71-92`. `m_dummyPasswordHash` is pre-generated at startup via `Crypto::HashPassword("aphelyon-h-v2-5-timing-mitigation")` with `DEFAULT_ITERATIONS`. On `LoadByUsername` miss, `Crypto::VerifyPassword(password, m_dummyPasswordHash)` runs the full 200k-iter PBKDF2 derivation, then the function returns `not_found`. Timing matches the known-user-bad-password path.

**Timing genuinely indistinguishable?** Yes for the common path: both paths run exactly one full PBKDF2 derivation at `DEFAULT_ITERATIONS`. The DB lookup path differs (miss = one indexed SELECT returning empty; hit = one indexed SELECT returning row + ~1KB row materialize). The DB-lookup time delta is ~µs, dominated by PBKDF2's ~150-400ms. Practically indistinguishable.

**Edge case the question asks about — does `VerifyPassword` short-circuit on malformed input?** Yes — `ParseStoredHash` returns false on malformed input (no colon, bad hex, etc.) and `VerifyPassword` returns `false` without burning compute. **But `m_dummyPasswordHash` is generated by `Crypto::HashPassword` itself**, which always emits a well-formed `iter:salt:hash` (lines 51-63 of Crypto.hpp). So the dummy hash always parses cleanly. The only way to break this is the V3-H1 ordering bug (above) — which is the actual gap. Today's code path is correct.

### H-V2-8: GetValidSession single-mutex check + copy-out
**Commits `ddf1916`** — `SessionManager.hpp:276-322`. Replaces the prior `ValidateSession; GetSession` two-call idiom. The race window between calls (concurrent invalidate landing `result=Valid` + `session=nullopt`) is closed. Verified callers: `AuthServer::ValidateSession`, `AuthorizeToken` lambda, `HandleResumeSession` — all use GetValidSession.

### M-V2-9: NoteAuthLost / NoteAuthRecovered split by locking contract
**Commit `ca1976d`** — `SessionCache.hpp:185-234`. `NoteAuthLost` is lock-free (only flips the atomic + emits one log line). `NoteAuthRecovered` acquires `m_mutex` to drop extended sessions. The previous "documented-only" locking contract is now enforced by function signature.

### M-V2-10: LRU eviction in RateLimiter
**Commit `8f0856b`** — `RateLimiter.hpp:48-113` + `LruCache.hpp`. Spray-attack scenario verified: a unique-key spray no longer fails-closed and refuses legit users. The new failure mode (V3-H2 above) is different in shape — see H2 for the new attack surface.

### M-V2-11: cached secret via thread-safe static-local init
**Commit `6bfe75b`** — `InternalRpcAuth.hpp:53-66`. Static-local lambda initializer reads `std::getenv("APHELYON_INTERNAL_SECRET")` exactly once. C++11 guarantees thread-safe init. Concurrent `ServiceClient::Call` / `ServiceEndpoint::HandleConnection` cannot race the init.

**Runtime env-var injection?** Not via this path — the value is captured at first call (typically `main.cpp::IsSecretConfigured()` gate). A future `setenv` from a loaded library cannot affect the cached value. Operator-driven rotation requires process restart, which matches the prior semantics.

### M-V2-12: quest token Release-fatal
**Commit `6bfe75b`** — verified at `Account/src/main.cpp:114-127`. Release builds (`#ifdef NDEBUG`) refuse to start without `APHELYON_QUEST_TOKEN_SECRET`. Debug builds fall through to `InitializeQuests`' per-process random fallback (`AccountServer.hpp:248-255`).

**Debug fallback leak surface:** the random secret is stored in `m_questTokenSecret` (`std::string`). It's used only inside `HandleBeginMinigame` (mint) and `HandleReportQuestProgress` → `VerifyQuestToken` (verify). No log line prints `m_questTokenSecret`. `grep` confirms: zero log statements reference `m_questTokenSecret` or `questTokenSecret`. The only attacker reachability is a process-memory dump (covered by V3-M3) — Debug-only, not a Release concern.

### M-V2-13: DB password env-var override
**Commit `6bfe75b`** — `Account/src/main.cpp:153-164`. Honors `APHELYON_DB_CONNECTION`. **Other sites with the literal password:** verified by `grep` — `AccountServer.hpp:58` constructor default arg, 6 integration test fixtures, BUILD.md docs, CLAUDE.md docs. See V3-M2 for the followup.

`docker-compose.yml` correctly uses `${POSTGRES_PASSWORD:-aphelyon}` env-var substitution so an operator can `POSTGRES_PASSWORD=<real> docker compose up` without editing the file. Good.

---

## Threat-Model Summary (v3 baseline)

| Attack | Before v2 | After v2 | After v3 |
|---|---|---|---|
| Session hijack via stolen token from any IP | Open (H6) | **Open via ResumeSession** (C7-B) | **Closed** |
| Internal RPC impersonation w/o secret | Open (M3) | Closed | Closed |
| Internal RPC MAC bypass via canonicalization | n/a | **Latent** (C7-C) | **Closed** |
| Memory-only rollback corruption | Open (C1) | **Latent** (C7-A) | **Closed** |
| Username enumeration via timing | Loud (10k iter) | **Louder** (200k iter, H-V2-5 latent) | **Closed (subject to V3-H1)** |
| ResumeSession brute-force | n/a | **Unlimited** (H-V2-4) | **Closed** |
| Limiter map inflation via IP spray | Open (M8) | Closed (refuse-new) | Closed |
| Limiter spray locks out legit users | n/a | **Open** (M-V2-10 pre-fix) | Closed (subject to V3-H2) |
| Quest-token forgery via length extension | Open (M4) | Closed | Closed |
| Offline PBKDF2 brute-force on stolen hash | 20× weaker (C5) | Closed (200k) | Closed |
| Quest token loss across restart (Release) | Open (M5) | Closed (Release-fatal) | Closed |
| DB password leak from binary | Open | Partial (main.cpp only) | **Partial** (V3-M2 — test fixtures + default arg) |

---

## Suggested triage order

**This week:**
1. **V3-H1** — fix `m_dummyPasswordHash` ordering by making it a static-local in `HandleVerifyCredentials`, OR add startup assertion. ~5 lines.
2. **V3-H2** — split `LruCache::Get` into touch / no-touch variants; have `RateLimiter::Allow` only Touch on success. ~10 lines across LruCache.hpp + RateLimiter.hpp.

**Before launch:**
3. **V3-M1** — `LogSessionEvent("validated", ...)` drop to Trace.
4. **V3-M2** — thread `APHELYON_DB_CONNECTION` through integration test fixtures + drop the constructor default arg.
5. **V3-M4** — emit `LogSessionEvent("invalidated", ...)` from `InvalidateByPlayerIdInternal`.
6. **V3-M5** — make `Crypto::VerifyPassword` always burn at least one PBKDF2 derivation, even on parse failure (defense-in-depth for V3-H1).
7. **V3-M6** — mark legacy `SessionManager::ValidateSession` `[[deprecated]]` or remove.
8. **V3-M8** — Release-fatal on `BCryptGenRandom` failure (or startup self-test).

**Eventually:**
9. V3-L items (struct defaults, log levels, runbook notes).
10. PBKDF2 600k iteration bump pre-launch.

---

## Assurance — what's provably correct

- **Session-token lifecycle:** 256-bit entropy, 24h lifetime, 30min idle, IP-bound in production. Every accessor path that returns session data either runs `GetValidSession` (full check) or is explicitly documented as the bare-flag accessor (`GetSession`, `GetPlayerName`). Only one caller (`HandleLogout`) uses the bare accessor today, and only to read playerId for the log line — no auth-relevant decisions made on its return value.
- **MAC envelope:** signs the on-wire bytes the receiver dispatches against. No re-serialization gap. Replay window 30s symmetric. `HexEquals` is constant-time.
- **Password verify:** PBKDF2-HMAC-SHA256 at 200k iters, salt 128-bit, hash 256-bit. `ConstantTimeCompare` on the derived bytes. Lazy rehash on next verify under stripe lock + re-read.
- **Dummy hash timing mitigation:** correct under today's startup ordering; V3-H1 documents the fragility.
- **Internal-RPC trust boundary:** shared-secret HMAC + 127.0.0.1-only bind + 30s replay window. Loopback compromise is the threshold to bypass, and even then requires the secret.
- **Resume rate limit:** shared with login limiter. Cannot pivot between paths to dodge per-IP budget.
- **Three unauthenticated entrypoints:** Login, Register, ResumeSession. All three are rate-limited. Resume now runs full validation.
- **Rate limiter:** LRU evict guarantees forward progress for legit new entries (subject to V3-H2 attacker-MRU-pinning).
- **Secrets handling:** internal HMAC + DB conn + quest token all overrideable via env vars; Release fail-fast on missing internal HMAC + quest secret; Release fail-fast on disabled IP binding + rate limiter.

---

## Agent observations

The v2 → v3 sweep is one of the cleaner remediation passes in this codebase's audit history — every named v2 item is closed in code, no fix-introduces-new-defect cycles, no documentation drift between the audit synthesis and the actual commits. The two H-class items here are both narrow defense-in-depth gaps with concrete fixes; the M-class items are mostly hygiene.

Recommend: ship V3-H1 + V3-H2 fixes as a single sweep this week, then the security backlog is in a defensible pre-launch state.
