# v3 Follow-up Audit — Error Handling + Rollback
**Date:** 2026-06-03
**Auditor:** Auditor v3 (follow-up to v1 2026-06-02 + v2 2026-06-02)
**Scope:** AccountTransaction commit/rollback chain (C7-A reorder, M19 post-commit catch, M-V2-6 reverse ordering, M-V2-8 audit_log normalization), AccountCache extraction (M-V2-3), AccountServer::OnStopped, all handler ParseJson sites (H-V2-7), EventStore exception translation, InternalRpcAuth env handling, Auth ResumeSession/ValidateSession paths, all three main.cpp startup-fatal paths.

---

## Summary

The v1+v2 remediation passes have closed every Critical, High, and Medium that v3 was scoped to verify. The C7-A reorder lands cleanly in all 5 event-sourced handlers (Pull, MultiPull, Level, Ascend, ClaimQuestReward) — `Begin()` is now first, capturing the Memento snapshot BEFORE the wallet/RNG/pity/collection mutations, so a Commit-throw genuinely restores pre-mutation state. The M19 catch block correctly reverses cursor advance and ClearDirty (M-V2-6) and unconditionally MarkStaleForReload on any throw. M-V2-1 logging is wired in both `CleanupIdleAccounts` and `SaveAllAndClear` with per-account ERROR + summary WARN. M-V2-8 target normalization is defensive-only (the lone RecordAudit call site at ProgressionHandlers.hpp:543 passes a populated target). H-V2-7 strict-parse closes the QuestHandlers gap. All three services (Auth, Account, Combat) gate on `IsSecretConfigured()` at startup; Account additionally gates `APHELYON_QUEST_TOKEN_SECRET` in Release. EventStore's `pqxx::unique_violation → ConcurrencyConflict` translation is in place at L80-81; the C7-A reorder means this is the canonical Commit-throw path AccountTransaction's dtor must handle, and it does. ServiceEndpoint's `Json::parse` for `params_json` (C7-C) is inside the existing try-catch, so a malformed inner JSON surfaces as `"Internal error: ..."`.

The v3 sweep turns up **zero new Critical and zero new High**. Three small Mediums remain — all pre-existing residue rather than regressions of the v2 fixes — plus a handful of Lows. The error-handling dimension is in the best shape it's been since this dev cycle began.

---

## Critical

**None.** All v1 + v2 Criticals verified closed.

---

## High

**None.** All v1 + v2 Highs verified closed.

---

## Medium

### M-V3-1. `HandleBeginMinigame` still parses with `ParseJsonSafe`
**File:** `Server/Account/src/QuestHandlers.hpp:112`
**Status:** NEW (sweep residue — H-V2-7 finished ReportQuestProgress + CompleteQuest but left BeginMinigame on Safe).

BeginMinigame is the third quest-side handler that touches authoritative state — it mints a HMAC-signed challenge_token tied to `playerId|questId|minigameId|tsHex`. Currently still uses `ParseJsonSafe`, so a malformed payload silently defaults `quest_id` and `minigame_id` to "" and short-circuits at the emptiness check. Behavior is benign today (empty IDs reject), but the M11 contract was "every handler that produces authoritative state uses strict parse" — and BeginMinigame produces a signed authorization artifact, which is squarely in that category. The signed token only encodes the resolved (post-empty-check) strings, so the actual MAC output is fine — the gap is the invariant, not a live bug.

**Fix:** Mechanical migration to `ParseJsonStrict` + "Invalid request payload" early return, matching the v2 H-V2-7 sites at L174 (ReportQuestProgress) and L648 (CompleteQuest).

### M-V3-2. `AccountHandlers::HandleGetInventory` + `HandleGetCharacterStats` use `ParseJsonSafe`
**Files:** `Server/Account/src/AccountHandlers.hpp:75, 149`
**Status:** OBSERVED-OK as scoped, but worth flagging.

These two are read-only handlers, so the M11 contract ("every MUTATING handler uses strict parse") technically doesn't extend to them. ParseJsonSafe → default 0/empty filters → returns the whole inventory or "Missing char_id" — both safe degradations. Filed as Medium only because the inconsistency with adjacent strict-parse sites invites a future maintainer to add a mutating side effect without realizing the parser is permissive. Either migrate to strict for consistency, or add a brief comment at each site explaining why Safe is fine here (read-only, default fields are well-behaved).

### M-V3-3. `AuthServer::HandleRegister` + `HandleLogin` still use `ParseJsonSafe`
**Files:** `Server/Auth/src/AuthServer.hpp:194, 247`
**Status:** OBSERVED-OK as scoped.

Register and Login mutate authoritative state (account creation, session minting) but ParseJsonSafe is followed by explicit `username.empty()` / `password.empty()` rejects, plus `ValidateUsername` / `ValidatePassword` / `IsValidUsernameFormat`. So a malformed payload becomes "Username cannot be empty" — boundary-equivalent to "Invalid request payload". Filed alongside M-V3-2 for the same reason: invariant drift surface. If the user ever adds a new field that doesn't have its own empty-check (a future `client_capabilities` or `device_id`), the silent-default behavior re-emerges.

---

## Low / Observation

- **L-V3-1.** `RestoreFrom` silent catch (v2 L-V2-1) still has no log line. The justification ("stale-flag is belt-and-suspenders") still holds, but the asymmetry with L1's `tx_->abort()` logging is still there. Same diagnosis as v2 — adding a `LOG_DATA_WARN` is one line.

- **L-V3-2.** `Account.hpp` Snapshot doc-comment (v2 L-V2-2) — now ACTUALLY correct after C7-A. The comment that overstated the restore guarantee in v2 ("by the time the throw unwinds the Account is back to its pre-transaction values") is now true on every handler path. If there's still a stale word about "may not restore RNG/pity on some handlers," scrub it.

- **L-V3-3.** `tx_->abort()` after a successful `tx_->commit()` followed by post-commit throw is unreachable today (M19 catches everything) — but the abort path at L317-333 is still wired through Rollback. Cosmetic only.

- **L-V3-4.** `audit_log.target` normalization (M-V2-8) writes `"{}"` (an empty JSON object) plus WARN. The only live call site (ProgressionHandlers.hpp:543) passes a populated `target` Json, so the WARN is correctly defensive. Confirmed.

- **L-V3-5.** `EnsureOpen()`'s `throw std::logic_error` (v2 L5) still has no LOG_NET_ERROR before the throw — programmer-bug surface that the dispatcher would only see as "Internal server error". Defer.

- **L-V3-6.** `InternalRpcAuth::GetSharedSecret` (M-V2-11) static-local cache means the env var is captured once. Side-effect: operator can't rotate the secret without a process restart. Documented in comment, intended.

- **L-V3-7.** `IsSecretConfigured()` calls `GetSharedSecret()` which triggers the static-local init. If main.cpp's call order is ever inverted so `IsSecretConfigured()` runs AFTER another GetSharedSecret caller (none today), the empty-result-→-refuse-start gate still works correctly. Confirmed safe.

- **L-V3-8.** ServiceEndpoint's `Json::parse(extractResult.rawJson)` and the inner `Json::parse(paramsJson)` both surface as `Internal error: ...` to the caller. The outer parse failure produces a response with `method=""` (since `method` wasn't yet assigned when the outer parse threw) — slightly misleading log line ("handler exception for method ''"). One-line fix: assign `method` from the rawJson manually before the outer parse, or extract the parse out of the try. Cosmetic.

---

## Focus Question Walkthrough

### 1. C7-A correctness across throws
**Verified.** All 5 event-sourced handlers (`HandleSinglePull` GachaHandlers.hpp:140, `HandleMultiPull` :403, `HandleLevel/Ascend` ProgressionHandlers.hpp:184/273/365/455, `HandleClaimQuestReward` QuestHandlers.hpp:549) call `m_ctx.repository->Begin(account)` BEFORE the first wallet/RNG/pity/collection mutation. The C7-A comments at GachaHandlers.hpp:133-145 + :388-398 explicitly call out the reorder rationale. `AccountTransaction::AccountTransaction` captures `preTxSnapshot_(account.CaptureSnapshot())` in its initializer list, so the snapshot is genuinely pre-mutation. If `AppendInTx` throws `ConcurrencyConflict` (the `pqxx::unique_violation` → `ConcurrencyConflict` path at EventStore.hpp:80-81), the propagation chain is: `AppendInTx` throws → `Commit()` propagates → `~AccountTransaction()` runs → `Rollback()` → `RestoreFrom(preTxSnapshot_)` restores genuine pre-mutation state → `MarkStaleForReload()`. The C7 mechanism is now load-bearing instead of dead code.

### 2. M19 post-commit catch + M-V2-6 reverse ordering
**Verified.** AccountTransaction.hpp:271-309. Sequence: `tx_->commit()` → `tx_.reset()` → `state_ = Committed` → try block (cursor advance loop FIRST, then `ClearDirty()` SECOND) → catch `std::exception` and `...` both log and call `MarkStaleForReload()`. Reverse ordering means cursor advances (pure int copies, can't throw) land first; if `ClearDirty` then throws (allocator failure on dirty containers), the catch still fires and marks stale. **Important edge case verified:** if cursor advance itself somehow threw (it can't, but suppose) the catch still marks stale. So `MarkStaleForReload` is guaranteed to fire on any throw inside the post-commit block — `IsStale()` is set, next `GetLockedAccount` evicts and reloads from DB MAX.

### 3. M-V2-1 CleanupIdleAccounts logging
**Verified.** AccountCache.hpp:163-181 (CleanupIdleAccounts) + :202-214 (SaveAllAndClear). Both loops: per-account `if (m_repository.Save(*account))` increments saved-count, else increments failed-count and emits `LOG_DATA_ERROR` with both `account_id` and `playerId`. Summary `LOG_DATA_WARN` fires whenever `failed > 0`. No silent-drop path remains. `OnStopped` (AccountServer.hpp:162-172) uses `SaveAllAndClear()`'s returned `(saved, failed)` pair to emit either `LOG_SERVER_INFO` or `LOG_SERVER_WARN`. End-to-end coverage.

### 4. M-V2-8 audit_log.target normalization
**Verified.** AccountTransaction.hpp:213-220. Null target → `targetStr = "{}"` + `LOG_DATA_WARN` with action and actor. The only live caller (`ProgressionHandlers.hpp:543`) passes a populated target — the WARN is defensive-only today, exactly as documented in the AccountTransaction comment at L203-212.

### 5. H-V2-7 strict parse adoption
**Verified for mutating handlers in scope.** ReportQuestProgress (QuestHandlers.hpp:174), CompleteQuest (:648), ClaimQuestReward (:492), all 5 progression handlers (ProgressionHandlers.hpp:124/219/308/398), pull handlers (GachaHandlers.hpp:60/310), and SetParty + AddCurrency (AccountHandlers.hpp:175/303) all use ParseJsonStrict + early-return on parse failure. **One gap surfaced:** `HandleBeginMinigame` (QuestHandlers.hpp:112) still uses ParseJsonSafe — see M-V3-1.

### 6. HandleResumeSession (C7-B + H-V2-4 + H-V2-8)
**Verified.** AuthServer.hpp:404-438. Rate-limit-reject returns `CreateRateLimitResponse("resume", ...)` which delegates to `CreateAuthResponse(false, ...)` — same shape as login-failure. Validation-reject returns `CreateAuthResponse(false, "Session expired or not found", "", "", "", 0)` — same shape. Success returns the full AuthResponse with player_id + session_token + expires_in + services. No path returns a wrong shape; no path throws (ValidateSession is exception-free; GetValidSession returns a result struct).

### 7. Release-fatal env vars (M-V2-11 + M-V2-12)
**Verified.** `APHELYON_INTERNAL_SECRET` gates all three services via `InternalRpcAuth::IsSecretConfigured()`: Auth/main.cpp:119, Combat/main.cpp:88, Account/main.cpp:102 — each emits LOG_SERVER_CRITICAL and returns 1. Debug builds fall through to the documented `DEBUG-aphelyon-internal-rpc-secret-do-not-ship` static fallback in InternalRpcAuth.hpp:60. `APHELYON_QUEST_TOKEN_SECRET` is gated Release-fatal only in Account/main.cpp:116-130 (Combat and Auth don't sign quest tokens, so they don't need it). The gate uses `#ifdef NDEBUG` so Debug builds fall through to InitializeQuests's per-process random + WARN — matches v2 M-V2-12 design. No service starts successfully in Release without a secret it actually needs.

### 8. DB connection failure mid-call
**Verified.** `Rollback()` at AccountTransaction.hpp:312-358 wraps `tx_->abort()` in try-catch with both `std::exception` and `...` arms, logging to LOG_DATA_WARN. If `EnsureOpen` itself throws `PoolExhausted` (H5 path), `state_` stays Pending, `tx_` is nullptr, dtor → Rollback → `if (tx_)` guard skips the abort — clean. Strongly-exception-safe per Lease move-semantics analysis in v2.

### 9. Json::parse exceptions in ServiceEndpoint
**Verified.** ServiceEndpoint.hpp:147-204. Both the outer `Json::parse(extractResult.rawJson)` and the inner `Json::parse(paramsJson)` (C7-C) live inside the same try-catch. Any malformed JSON surfaces as `LOG_NET_ERROR + response={{"error", "Internal error: " + e.what()}}`. Caller's ServiceClient sees `result.value("error", ...)`, returns the JSON to AuthServer (or wherever), which handles via the established "RPC error → reject login/op" path. The minor `method=""` cosmetic when the OUTER parse fails (since `method` is assigned inside the try) is filed as L-V3-8.

### 10. Anything v1/v2 missed
- `HandleBeginMinigame` ParseJsonSafe (M-V3-1) is the only newly-surfaced item in the M11/H-V2-7 sweep that v1+v2 missed across all 8 dimensions.
- The v2 L-V2-1 (silent RestoreFrom catch) is still open and worth one log line.
- Read-only handlers (M-V3-2/M-V3-3) using ParseJsonSafe is an invariant-drift surface, not a bug.

---

## Verified Closed (v3 sweep)

| Item | Source | Where verified |
|---|---|---|
| C7-A snapshot timing | v2 Critical | Begin() at top of 5 event-sourced handlers |
| C7-B ResumeSession IP binding | v2 Critical | AuthServer.hpp:413 GetValidSession with clientIP |
| C7-C MAC over on-wire bytes | v2 Critical | ServiceEndpoint.hpp:162-166 params_json verbatim |
| H-V2-1 lazy snapshot/lease | v2 High | AccountTransaction.hpp:30-52 + EnsureOpen L379 |
| H-V2-4 ResumeSession rate limit | v2 High | AuthServer.hpp:406-411 m_loginLimiter |
| H-V2-7 strict parse on quest mutators | v2 High | QuestHandlers.hpp:174, :648 |
| H-V2-8 GetValidSession | v2 High | AuthServer.hpp:62, :143, :413 |
| M-V2-1 CleanupIdle save-failure logging | v2 Medium | AccountCache.hpp:163-181 |
| M-V2-6 reverse cursor/ClearDirty order | v2 Medium | AccountTransaction.hpp:290-299 |
| M-V2-8 audit_log.target normalization | v2 Medium | AccountTransaction.hpp:213-220 |
| M-V2-11 cached env-var lookup | v2 Medium | InternalRpcAuth.hpp:53-66 static-local |
| M-V2-12 Release-fatal quest secret | v2 Medium | Account/main.cpp:116-130 #ifdef NDEBUG |
| pqxx::unique_violation translation | scope | EventStore.hpp:80-81 |
| All three main.cpp env-var gates | scope | Auth:119, Combat:88, Account:102 |

---

## Verdict

The error-handling dimension is clean. The v1+v2 audit-and-fix arc has converged: C7's Memento snapshot is now load-bearing for real (not just unit-tested in isolation); M19's post-commit catch is correctly ordered and unconditionally marks stale; M-V2-1 logging closes the silent-save-failure surface; M-V2-8 normalization closes the audit_log NOT NULL trap; all three services gate on the internal-RPC secret at startup. The only items v3 turns up are one sweep-residue Medium (M-V3-1, BeginMinigame ParseJsonSafe), two consistency-drift Mediums (M-V3-2/M-V3-3 on read-only handlers + register/login), and a handful of Low cosmetics. **No new Critical or High.** This dimension can move off the "active risk" list.
