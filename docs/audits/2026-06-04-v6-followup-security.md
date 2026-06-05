# v6 followup — Security + Auth + Secrets

**Date:** 2026-06-04
**Dimension:** Security / Auth / Secrets
**Scope:** AuthServer (HandleLogin / HandleRegister / HandleResumeSession / HandleValidateSession), SessionManager, InternalRpcAuth, ServiceEndpoint, Crypto, AddCurrency authorization, protocol.json auth boundary, rate limiter, Debug-vs-Release CLI flag matrix, env-var fail-fast matrix, `--seed-accounts` reachability.
**Method:** Verified v5 carry-forwards in code at HEAD; walked `ProcessMessage` → `RequiresAuthentication` → `ValidateSession` chain; re-derived auth bypass surfaces from protocol.json; confirmed env-var enforcement matrix; checked the new per-account-lock model (M-V5-4 concurrency Scope 4 close) for crypto-rehash race.

---

## Verdict

**Clean.** The v5 cycle's two High items shipped fixes; H-V5-1 (HandleLogin/HandleRegister envelope-error classify) is closed in `88a9346`, verified at `Server/Auth/src/AuthServer.hpp:295-300` and `:366-371`. The trust surface around InternalRpcAuth, ServiceEndpoint MAC verification, Crypto's lazy rehash under per-account locks, and the three env-var fail-fast checks (`APHELYON_INTERNAL_SECRET`, `APHELYON_QUEST_TOKEN_SECRET`, `APHELYON_DB_CONNECTION`) all hold up. The per-account lock refactor (Scope 4, batches m–r) does not regress the lazy-rehash path — `NeedsRehash` + `UpdatePasswordHash` + `UpdateCachedPasswordHash` are sequenced under `m_cache.LockFor(accountId)` per-stripe, so the rehash-after-PBKDF2-verify race the audit was looking for cannot fire.

One **new Medium** surfaces around the protocol.json auth-default asymmetry: a typo or omission of `requires_auth` in protocol.json silently produces an unauthenticated handler entry (`Server/Common/src/Net/Protocol.hpp:190` defaults to `false`), while a totally-missing message definition fails-closed (`:393` defaults to `true`). Today's protocol.json sets the flag explicitly on every `client_to_server` request so there's no live exploit, but the fail-open default is the wrong shape for a security boundary read from a hand-edited JSON file.

C/H/M/L: **0 / 0 / 1 / 7**

---

## Status of v5 items in this dimension

| ID | Title | Status |
|---|---|---|
| H-V5-1 | HandleLogin/HandleRegister envelope-error classify | **CLOSED** (commit `88a9346`, verified at AuthServer.hpp:295-300, :366-371) |
| H-V5-2 | ResumeSession rate-limit × per-IP cap interaction | **CLOSED as documented** (commit `bc57411` annotated the complementary-not-multiplicative semantics at AuthServer.hpp:510-528; no code change needed) |
| M-V5-1 | Per-IP cap keyed by raw IP (NAT/CDN concern) | **PARTIAL** (commit `27d7917` added log throttle + made the cap configurable via protocol.json `settings`; raw-IP keying still in place — documented design tradeoff per v5 verdict) |
| L-V5-1 | PBKDF2 iterations 200k vs OWASP 600k | **STILL OPEN** (DEFER marker added at Crypto.hpp:41-47 — pre-launch decision) |
| L-V5-2 | BCryptGenRandom failure → silent fallback | **STILL OPEN** (DEFER marker at Crypto.hpp:150-159 — Windows-only pre-launch risk) |
| L-V5-3 | InternalRpcAuth symmetric 30s window | **STILL OPEN** (DEFER marker at InternalRpcAuth.hpp:25-34 — loopback-only) |
| L-V5-4 | `SeedAccounts` body compiles into Release binary | **STILL OPEN** (no `#ifndef NDEBUG` gate at Account/main.cpp:51-86 or :131-140; reachable in Release if operator passes `--seed-accounts` AND env vars are configured) |
| L-V5-5 | `m_questTokenSecret` heap zeroization | **STILL OPEN** (AccountServer.hpp:372 — `std::string` destructor doesn't zero) |
| L-V5-6 | `Probe`'s `rpc.value.value("error", ...)` no `is_object()` guard | **STILL OPEN** (ServiceClient.hpp:334 — theoretical, today's ServiceEndpoint only emits objects) |

---

## NEW findings

### [M-V6-1] `RequiresAuthentication` defaults to FALSE on missing `requires_auth` field in protocol.json

**File:** `Server/Common/src/Net/Protocol.hpp:190` (loader default) vs `:386-394` (lookup default)
**Status:** NEW

`ProtocolLoader::Load` at line 190:
```cpp
msg.requiresAuth = msgJson.value("requires_auth", false);
```

A protocol.json entry that defines a message ID + name but omits `requires_auth` (typo'd as `requires_authn`, accidentally removed during an edit, copied from a response template that doesn't carry the field, etc.) silently registers as `requiresAuth=false`. `RequiresAuthentication` then returns `def->requiresAuth` → `false`, so `TcpServerBase::ProcessMessage` skips token validation and dispatches the handler with an empty `playerId`. Handler code that reads `playerId.empty()` may early-return (defense-in-depth) or may pass the empty string into `m_cache.GetLockedAccount("")`, which `LookupAccount` returns "not found" for — but other handlers (e.g., `HandleAddCurrency` line 342) take whatever `playerId` they get and would attempt the grant against the empty account.

**Today's exploit status:** none. All 24 `client_to_server` request messages in `Server/data/protocol.json` set `requires_auth` explicitly (21 with `true`, 3 with `false` — Login/Register/ResumeSession). The two id-≥100 client-to-server requests (GetInventory id=119, GetCharacterStats id=121, both intentional protocol-numbering violations) also set `requires_auth: true`. So the live wire is correct.

**Impact:** future protocol.json edits (most likely path: adding a new gameplay message during a feature PR) silently open an unauthenticated handler if the author forgets the field. The lookup-side fallback at `Protocol.hpp:393` is correctly `return def ? def->requiresAuth : true;` — fail-closed on totally-missing definitions — but the load-side default at `:190` reverses the polarity. Two different defaults for the same conceptual question is the bug shape.

**Fix sketch:** flip the loader default to `true`:
```cpp
msg.requiresAuth = msgJson.value("requires_auth", true);
```
A new explicit `false` is still possible (the three unauthenticated entry points), but the editor mistake fails-closed. ~1 LOC. Add a `CommonTests` golden-file test that loads a synthetic protocol.json with a request-direction message missing `requires_auth` and asserts `RequiresAuthentication(id) == true`. ~10 LOC of test.

---

## Observations / Lows

- **L-V6-1 (Observation).** `HandleValidateSession` (AuthServer.hpp:465-481) uses `m_sessionManager.GetSession(token)` (bare-flag, no IP/idle check) followed by an explicit `IsExpired()` check. This is **safe today** because the message is `requires_auth=true` in protocol.json, so `TcpServerBase::ProcessMessage` ran the full `GetValidSession` check before dispatch. But the handler reads as if it owns the validation contract — a future refactor that flips `ValidateSession` to `requires_auth=false` (likely tempting since the handler's stated purpose is "tell me if this token is valid") would silently regress the IP/idle enforcement. Either (a) leave a comment pinning the dependency on the base's prior validation, or (b) switch to `GetValidSession` for parity with the other handlers.

- **L-V6-2 (Observation).** `RateLimits::AddCurrency()` is named `(debug)` in its inline comment at `RateLimiter.hpp:190`, but per CLAUDE.md the AddCurrency RPC is the **canonical wallet-grant path** (purchases, quest rewards, achievements). Comment-vs-truth drift; not a security defect. Rename the comment to `// Add currency (canonical grant: purchases/quests/achievements)`. ~1 LOC.

- **L-V6-3 (Observation).** `IsRequestMessage(id) = id > 0 && id < 100` and `IsResponseMessage(id) = id >= 100` are defined at `Protocol.hpp:381-382` but **never called** by `ProcessMessage`. Two client-to-server messages (GetInventory id=119, GetCharacterStats id=121) intentionally violate the request<100 convention; the helpers' contract is therefore already broken. Defense-in-depth opportunity: reject client_to_server messages whose direction in protocol.json doesn't match the inbound side. Today's TcpServerBase has no orientation check — a client can submit any registered message ID and the server dispatches if a handler exists. Today: not exploitable because all registered handlers run their own auth+input validation. Future: a hand-rolled response message becoming a writable handler would create an unauthenticated write surface.

- **L-V6-4 (Observation).** `Server/Account/src/main.cpp:51-86` (`SeedAccounts`) still compiles into Release binaries. Carry-forward L-V5-4. To reach this code in Release, an operator must also have set `APHELYON_INTERNAL_SECRET`, `APHELYON_DB_CONNECTION`, and `APHELYON_QUEST_TOKEN_SECRET` (the env-var checks run before the seed-mode branch). So the realistic risk is: "operator inherits a staging env that's almost-prod-shape, runs `Account.exe --seed-accounts` thinking it's a no-op, gets 10 accounts named `test`/`test1`..`test9` with `password == username` written to prod." The fix is still the v5-proposed 3-LOC `#ifndef NDEBUG` gate around both the function body and the CLI flag arm. Not raising severity because the operator has to deliberately pass the flag; documenting because three audits have flagged it and the fix is trivial.

- **L-V6-5 (Observation).** Quest-token secret (`m_questTokenSecret` at AccountServer.hpp:372) heap zeroization still missing — `std::string` destructor frees but doesn't zero. Five audit rounds. Memory-disclosure window is process-lifetime + tail of allocator's free-list; threat model is local-host attacker with /proc/$pid/mem access. Pre-launch on Windows the risk is zero; documenting for the L-V5-5 carry-forward record.

- **L-V6-6 (Observation).** `InternalRpcAuth::MAX_AGE_SECONDS = 30` symmetric window unchanged from v3. Loopback-only attack surface. Five audit rounds.

- **L-V6-7 (Observation).** `Probe`'s `rpc.value.value("error", std::string{})` at ServiceClient.hpp:334 still lacks an `is_object()` guard. Carry-forward L-V5-6. `ServiceEndpoint` only emits objects today so the `type_error` throw is theoretical. ~3 LOC fix when next touching the file:
  ```cpp
  if (!rpc.value.is_object()) return ProbeOutcome::UnknownError;
  const auto err = rpc.value.value("error", std::string{});
  ```

---

## Verified Closed (from v5)

### H-V5-1 — HandleLogin/HandleRegister envelope-error classify
**Commit `88a9346`** — verified at `Server/Auth/src/AuthServer.hpp:295-300` (HandleRegister) and `:366-371` (HandleLogin). Both sites now check `rpc.value.contains("error")` BEFORE the `value("result", rpc.value)` fallback. On envelope error:
- HandleRegister logs `LOG_AUTH_WARN("Register: Auth-to-Account envelope error: {} (IP={})", err, clientIP)` and returns `"Account service unavailable"` response — same outcome shape as the WireError path above.
- HandleLogin logs `LOG_AUTH_WARN("Login: Auth-to-Account envelope error: ...")` and returns the same "Account service unavailable" envelope.

This closes the steady-state secret-rotation blindness — a mid-flight `APHELYON_INTERNAL_SECRET` rotation that lands on Auth before Account now produces a sustained WARN stream identifying envelope errors instead of silently downgrading every login to "Invalid username or password." Symmetric with the M-V4-9 SessionCache fix and the C-V4-2 Probe fix; all four `m_authClient.Call` / `m_accountClient.Call` consumers now classify envelope errors loudly.

### H-V5-2 — ResumeSession × per-IP cap interaction
**Commit `bc57411`** (docs-only) — verified at `Server/Auth/src/AuthServer.hpp:510-528`. The annotation pins down the audit's question: the per-IP rate limit serializes through a single RateLimiter mutex, so 16 sockets from one IP all draw from the same 10/min budget. No 16× amplification. The 256-bit token entropy makes brute force infeasible regardless; the rate limit is server-CPU protection, not a credential-guess boundary. No code change required — the audit's option (c) was selected.

### Verified-still-correct from v4 sweep
- **InternalRpcAuth HMAC enforcement.** ServiceEndpoint.hpp:283-304 — every internal RPC verifies envelope version FIRST, then MAC, then dispatches. The check is on the inbound recv path, so there is no method-name-dependent skip.
- **Env-var fail-fast matrix (unchanged from v5).**
  - `APHELYON_INTERNAL_SECRET`: Release-fatal in Auth/Account/Combat main.cpp (Auth:117-122, Account:158-162, Combat:86-91).
  - `APHELYON_QUEST_TOKEN_SECRET`: Release-fatal in Account/main.cpp:172-185; Debug falls through to AccountServer::InitializeQuests's per-process random with WARN.
  - `APHELYON_DB_CONNECTION`: Release-fatal in Account/main.cpp:219-224.
  - `g_rateLimitingEnabled` default `true` at RateLimiter.hpp:24; Release-fatal on disabled in all three services; CLI flag `--no-rate-limit` gated by `#ifndef NDEBUG`.
  - `g_authIPBindingEnabled` default `true` at AuthServer.hpp:31; Release-fatal on disabled in Auth/main.cpp:104-113; CLI flag `--no-ip-bind` gated by `#ifndef NDEBUG`.
- **Crypto lazy rehash under per-account locks.** InternalRpcHandlers.hpp:114 (`m_cache.LockFor(accountData->id)`) → :121 (re-read under lock) → :125-133 (NeedsRehash + UpdatePasswordHash + UpdateCachedPasswordHash). The check runs after PBKDF2 verify but inside the stripe lock, so two concurrent VerifyCredentials calls for the same account serialize on rehash. Scope 4 batch q closed the `UpdateCachedPasswordHash` race the audit was looking for.
- **AddCurrency authorization path.** AccountHandlers.hpp:319 — `requires_auth=true` in protocol.json, so `TcpServerBase::ProcessMessage` validated the session before dispatch. `HandleAddCurrency` parses strict, validates `currency_type` against the allowlist, clamps amount to `[1, 1_000_000]`, runs idempotency check, then rate-limits against `RateLimits::AddCurrency() = 30/min/IP, 30s cooldown`. No bypass surface.
- **Username/password input validation.** `IsValidUsernameFormat` (AuthServer.hpp:592-601) restricts to `[A-Za-z0-9_]` with length 3-20 and no leading/trailing underscore. `ValidatePassword` enforces length 4-128. The username being log-safe means `LOG_AUTH_INFO("Register: new account player={} username={} IP={}")` cannot be log-injected via newline/control chars in the username. Password is never logged.
- **Token entropy.** 256-bit (`Crypto::GenerateSecureToken(32)` → 64 hex chars). Retry-capped collision check still in SessionManager. IP-bound default (V3-L7 fix held).

---

## Threat-Model Summary (v6 baseline)

| Attack | v5 status | v6 status |
|---|---|---|
| Session hijack via stolen token from any IP | Closed | Closed |
| Session-validation IP-binding bypass | Closed (C7-B) | Closed |
| Internal RPC impersonation w/o secret | Closed | Closed |
| Internal RPC MAC bypass via canonicalization | Closed | Closed |
| Internal RPC envelope shape drift | Closed | Closed |
| Username enumeration via timing | Closed | Closed |
| ResumeSession brute force | Closed | Closed (documented) |
| Limiter map inflation via IP spray | Closed | Closed |
| Quest-token forgery via length extension | Closed | Closed |
| Offline PBKDF2 brute-force on stolen hash | Closed | Closed |
| Quest token loss across restart (Release) | Closed | Closed |
| Misconfigured-peer-secret deploy goes undetected at startup | Closed (C-V4-2) | Closed |
| Misdiagnosed login failures under steady-state secret mismatch | Open (H-V5-1) | **Closed** (commit `88a9346`) |
| Slow-loris / connection-flood DoS | Closed for raw IP | Closed for raw IP |
| Per-IP cap NAT/CDN lock-out | Open (M-V5-1) | **Partial** (configurable cap; raw-IP keying unchanged) |
| protocol.json typo opens unauth handler | n/a | **Open (M-V6-1)** |

---

## Suggested triage order

**This week:**
1. **M-V6-1** — flip `Protocol.hpp:190` default to `true`. 1 LOC + 1 test case. Closes the editor-mistake auth-bypass shape for good.

**Before launch:**
2. **L-V6-4 / L-V5-4** — gate `SeedAccounts` body + CLI arm with `#ifndef NDEBUG`. 3 LOC. Fourth audit appearance.
3. **L-V6-2** — fix RateLimits::AddCurrency comment ("debug" → "canonical grant path"). 1 LOC.
4. **L-V6-7 / L-V5-6** — `is_object()` guard in `Probe`. 3 LOC.
5. **L-V6-1** — comment-pin `HandleValidateSession`'s dependency on base-level validation OR switch to `GetValidSession`. 1-3 LOC.

**Eventually:**
6. PBKDF2 600k iteration bump pre-launch (L-V5-1 / L-V6).
7. Quest-token secret heap zeroization + rotation (L-V5-5 / L-V6-5).
8. L-V6-3 defense-in-depth orientation check (reject client_to_server msgIds whose registered direction is `server_to_client`).

---

## Assurance — what's provably correct (v6 update)

- **HMAC enforcement on internal RPC.** Every inbound internal RPC frame hits ServiceEndpoint.hpp:283 envelope-version check, then :300 MAC verification. No method-table dispatch happens before MAC verifies. Verified by reading the HandleConnection control flow end-to-end.
- **Envelope version pinning.** `APHELYON_ENVELOPE_VERSION = 1` at InternalRpcAuth.hpp:64 is part of the MAC input (line 119); version skew fails MAC even if the explicit `v` check is bypassed. Golden-file test at Common/tests/EnvelopeShapeTest.cpp.
- **Probe classification (5 outcomes).** Ok / Unreachable / AuthFailed / EnvelopeVersionMismatch / UnknownError all reachable from real ServiceEndpoint responses. Walked the dispatch table.
- **SessionCache envelope classification.** Matches Probe's pattern. Auth-down detection fires WARN + flips state.
- **HandleLogin/HandleRegister envelope classification (NEW v6 close).** Match the Probe + SessionCache patterns. Steady-state secret-mismatch deploys now produce loud WARN signals on every login attempt instead of silently degrading to "Invalid username or password."
- **Crypto lazy rehash under per-account locks.** InternalRpcHandlers.hpp:114-133. The check + write are serialized per-stripe via `m_cache.LockFor`. Scope 4 batch q (`5917756`) closed the `UpdateCachedPasswordHash` race specifically.
- **Three unauthenticated entrypoints** (Login, Register, ResumeSession) all rate-limited. ResumeSession's 10/min budget is shared across all 16 sockets from one IP (single RateLimiter mutex).
- **Three Debug-only CLI flags** (`--no-rate-limit`, `--no-ip-bind`, `--seed-accounts`) — first two `#ifndef NDEBUG`-gated AND runtime Release-fatal if reached; third unconditionally compiled (L-V5-4 / L-V6-4).
- **SQL injection surface clean.** All queries use `pqxx::params{}` or hardcoded literals. Verified across RelationalFlush, AccountRepository, OutboxRelay, EventStore.
- **Input validation.** `IsValidUsernameFormat` rejects everything outside `[A-Za-z0-9_]`. ValidateUsername length 3-20 + no leading/trailing underscore. ValidatePassword length 4-128. Username is the only user-controlled string that lands in log lines and it's restricted to log-safe characters.

---

## Agent observations

The v5 → v6 sweep is the cleanest yet. Both v5 Highs landed clean fixes (one code, one docs). Lows continue to dominate the carry-forward list (5 of 7 in this audit appear in 3+ prior cycles), reinforcing the v5 recommendation to ship a single "low-batch" PR — every fix is ≤ 5 LOC and collectively they retire the noise floor that's been the same five items for half a year.

The one new finding (M-V6-1) is a default-polarity mismatch in `Protocol.hpp` that's been latent since the protocol-loader was authored, surfaced now by walking the auth boundary fresh. Today's protocol.json is correct so there's no live exploit — but the asymmetry between the loader's `false` default and the lookup's `true` default is exactly the shape of bug that gets caught by an audit and silently regressed by a future feature PR adding a new message. Worth a 1-LOC fix.

The Crypto lazy-rehash race the audit was looking for under the new per-account lock model is **definitively closed** — InternalRpcHandlers.hpp:114 takes the stripe lock between `Crypto::VerifyPassword` and `Crypto::NeedsRehash` + `UpdatePasswordHash`, and the cache update path `UpdateCachedPasswordHash` was specifically race-fixed in Scope 4 batch q. The threat model the audit was probing — two simultaneous logins for the same account both deciding to rehash and racing the DB write — is no longer reachable.

Recommend: ship M-V6-1 this week with the test case; queue the Low batch behind the v6 cross-cutting close.
