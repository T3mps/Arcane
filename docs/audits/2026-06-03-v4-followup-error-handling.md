# v4 Follow-up Audit — Error Handling + Rollback
**Date:** 2026-06-03
**Auditor:** Auditor v4 (follow-up to v1 2026-06-02, v2 2026-06-02, v3 2026-06-03)
**Scope:** Verify the v3 remediation arc (commits f46d496..HEAD) for the error-handling + rollback dimension. Focus on try/catch discipline, ParseJsonStrict adoption across mutating handlers, AccountTransaction destructor behavior, Rollback restore completeness, EnsureOpen exception safety, stale-flag fallback when restore itself throws, and log-or-throw discipline.

Sources reviewed:
- v3 synthesis (`2026-06-03-server-persistence-audit-v3.md`)
- v3 per-dimension report (`2026-06-03-v3-followup-error-handling.md`)
- v2 synthesis (`2026-06-02-server-persistence-audit-v2.md`) — context only
- Current code: `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/State/Account.hpp`, all handler files in `Server/Account/src/Handlers/`, `Server/Common/src/Net/Protocol.hpp`, `Server/Common/src/Crypto/Crypto.hpp`, `Server/Common/src/Net/ServiceEndpoint.hpp`, `Server/Account/src/Db/EventStore.hpp`, `Server/Auth/src/AuthServer.hpp`, `Server/Common/src/Net/SessionCache.hpp`.
- v3 remediation commits relevant to this dimension: `21256f5` (RestoreFrom log + outer-parse diagnostic), `4b452b0` (ParseJsonStrict sweep + Crypto burn-PBKDF2), `0079c17` (dummy hash function-local static), `7185405` (ServiceClient::Call → RpcCallResult), `eaac521` (sibling-key invariant + outer-parse diagnostic).

---

## Verdict

The error-handling dimension is in materially better shape than at v3 baseline. Every v3 Medium and Low in this dimension is verified closed in code:

- **M-V3-1 (BeginMinigame ParseJsonStrict)** — closed at `QuestHandlers.hpp:120` with explanatory comment at `:112-119`.
- **M-V3-2 (GetInventory + GetCharacterStats strict parse)** — closed at `AccountHandlers.hpp:81` (GetInventory) and `:160` (GetCharacterStats).
- **M-V3-3 (HandleRegister + HandleLogin strict parse)** — closed at `AuthServer.hpp:242` and `:307` with comments at `:233-241` documenting why the downstream empty-checks were boundary-equivalent before but invariant-fragile.
- **M-V3-3 security (Crypto::VerifyPassword burns PBKDF2 on parse failure)** — closed at `Crypto.hpp:116-128`; a malformed stored hash now eats one PBKDF2 derivation against a fixed dummy salt + `DEFAULT_ITERATIONS` before returning false, so a corrupted DB row / H-V3-10 init-order regression / future caller handing in an undecodable hash cannot short-circuit and reopen the timing oracle.
- **L-V3-1 (RestoreFrom silent catch)** — closed at `AccountTransaction.hpp:347-357`; both `std::exception` and `...` arms now emit `LOG_DATA_WARN` while preserving the stale-flag fallback at L362.
- **L-V3-2 / L-V3-8 (ServiceEndpoint outer-parse diagnostic)** — closed at `ServiceEndpoint.hpp:239-254`; when the outer `Json::parse` throws and `method` is empty, the catch block now logs a 64-byte hex prefix of the raw bytes plus `totalBytes`, so ops can triage attacker probes / version skew / corrupt frames from the log alone.

**One new Low** emerged — `HandleClaimQuestReward` calls `TickQuests::Apply` (a mutator that touches `m_dirty.quest_ids` and `m_questStates` via Upsert / state transitions) BEFORE `Begin()`. Same defect class as v3's H-V3-1 and C-V3-2, missed by the H-V3-1 scope expansion. The v4 event-sourcing dimension already filed this as `H-V4-1` from the C7-A angle; the error-handling angle is that a Commit-throw cannot restore the pre-tick quest state via Rollback's snapshot. Listed here as **L-V4-1** because (a) the v4 event-sourcing audit owns the load-bearing fix, (b) the stale-flag eviction belt-and-suspenders still recovers on the next handler call so no live corruption surface exists, and (c) only the speculative TickQuests dirt persists in memory until the next handler reloads — non-catastrophic.

**No new Critical or High.** Test coverage of the dtor-Rollback path (vs explicit Rollback) is the most material remaining gap and joins the v3 backlog as L-V4-2.

---

## CRITICAL

**None.** Every v1 + v2 + v3 Critical in this dimension is verified closed.

---

## HIGH

**None.** Every v1 + v2 + v3 High in this dimension is verified closed.

---

## MEDIUM

**None.** All three v3 Mediums in this dimension are verified closed (see Verified Closed table below).

---

## LOW

### L-V4-1. `HandleClaimQuestReward` calls `TickQuests::Apply` BEFORE `Begin()`
**Files:** `Server/Account/src/Handlers/QuestHandlers.hpp:579` (`TickQuests::Apply`), `:605` (`Begin()`)
**Status:** NEW — error-handling angle on a defect already filed by v4 event-sourcing as H-V4-1.

`TickQuests::Apply` at L579 is a mutator: it calls `account.MutableDirty().quest_ids.insert(questId)` at TickQuests.hpp:59 for any quest it unlocks, calls `states.Upsert(def.MakeInitialState())` for newly-eligible quests, calls `states.ExpireStale(now)` / `states.RecycleClaimed(now)`, and rewrites `q->state / q->startedAt / q->completedAt / q->resetAt / q->objectives` for stale active quests. `Begin()` at L605 captures the Memento snapshot AFTER these mutations have landed.

**Error-handling impact:**
- A Commit-throw in the claim path triggers `~AccountTransaction()` → `Rollback()` → `RestoreFrom(preTxSnapshot_)` which restores to the POST-TickQuests state. The TickQuests mutations remain in the in-memory Account.
- Belt-and-suspenders: `Rollback` also calls `MarkStaleForReload()`, so the next handler call evicts and reloads from DB — the speculative TickQuests state in memory is discarded then.
- Same recovery shape as C-V3-2 had before its fix, so live correctness is preserved by the stale-flag fallback.

**Why this is Low rather than High:** the early-return validation chain at L581-586 also runs against post-Tick state, so a benign rejection (e.g., "Quest not started") leaves the speculative Tick state in the live Account until the next access. The dirty bits get persisted on the next handler's `Begin()/Commit()` cycle. No corruption, no double-grant — just minor speculative-state churn. The actual DB state was never wrong, and the next reload normalizes everything.

**Fix:** Mechanical — move `auto txn = m_ctx.repository->Begin(account);` from L605 to L578 (above `TickQuests::Apply`). The validation chain at L581-586 ("quest not started" / "quest not yet completed" / "quest already claimed") then early-returns with txn alive; the dtor calls Rollback which restores the pre-tick state and marks stale. Same shape as the H-V3-1 fix landed for HandleReportQuestProgress (`QuestHandlers.hpp:244`) and HandleCompleteQuest (`:725`).

### L-V4-2. dtor-Rollback path has no integration test
**Files:** `Server/Account/tests/Integration/AccountTransactionTest.cpp:182-231`
**Status:** OBSERVATION — carried into v4 from the v3 test backlog.

The existing C7 Rollback test calls `txn.Rollback()` explicitly. The actual production path is `Commit()` throws → handler unwinds → `~AccountTransaction()` fires → dtor's `if (state_ == State::Pending || state_ == State::Active) Rollback();` (`AccountTransaction.hpp:63`) calls Rollback indirectly. No test exercises this RAII path — a future regression that flips the dtor guard to the wrong polarity, or that constructs AccountTransaction with non-trivial Snapshot side effects, would not be caught.

**Fix:** Add a test case that constructs `AccountTransaction`, mutates the account, then lets the txn fall out of scope without calling Rollback explicitly (e.g., by throwing from a handler-like lambda inside the scope). Assert the same restore-then-stale invariants as the existing explicit-Rollback test.

### L-V4-3. `EnsureOpen()` throws `std::logic_error` with no log line
**File:** `Server/Account/src/Cache/AccountTransaction.hpp:392-393`
**Status:** OBSERVATION — v2 L-V2-5 / v3 L-V3-5 carry-forward.

`EnsureOpen()` throws `std::logic_error("AccountTransaction already committed or rolled back")` if state is neither Pending nor Active. This is a programmer-bug surface (handler trying to AppendEvent after Commit) but the dispatcher would only see "Internal server error" via the outer exception handler — no log line on the throw site identifies which RPC tripped the violation. One `LOG_NET_ERROR` before the throw would make this O(seconds) to triage instead of O(hours).

### L-V4-4. `EnsureOpen` exception-safety leaves `lease_` acquired if `make_unique<pqxx::work>` throws
**File:** `Server/Account/src/Cache/AccountTransaction.hpp:390-398`
**Status:** OBSERVATION — benign today, fragile under future refactor.

```cpp
lease_ = pool_.acquire();                                       // L394
tx_    = std::make_unique<pqxx::work>(*lease_);                 // L395 — may throw
db::EventStore::AcquireAdvisoryLockInTx(*tx_, account_.GetAccountId());  // L396 — may throw
state_ = State::Active;                                         // L397
```

If `make_unique<pqxx::work>` throws (broken connection at construction), `lease_` is acquired but `tx_` stays null and `state_` stays Pending. `~AccountTransaction()` → `Rollback()` → `if (tx_)` skips abort → state set to RolledBack → lease destructor returns the connection to the pool (`db::ConnectionPool::Lease` is a member, destroyed normally). So the lease IS returned, but via the member destructor chain rather than an explicit release inside EnsureOpen. Functionally correct.

If `AcquireAdvisoryLockInTx` throws (DB connection failure during the lock SQL), `tx_` is set but `state_` is still Pending. Rollback runs tx_->abort() correctly. Postgres releases the advisory lock at abort regardless of whether AcquireAdvisoryLockInTx actually held it (no half-acquired state). Functionally correct.

**Why it's Low and not Medium:** today's `ConnectionPool::Lease` is RAII-safe, so the lease is always returned. Worth knowing because a future refactor that adds intermediate steps between L394 and L397 (e.g., per-tx telemetry hook, prepared-statement cache warmup) needs to keep the strong exception guarantee or risk leaving `state_ = Active` without a valid `tx_` — at which point the dtor's `if (tx_)` skip-abort path is wrong.

### L-V4-5. Crypto::VerifyPassword burn-PBKDF2 uses fixed-byte dummy salt
**File:** `Server/Common/src/Crypto/Crypto.hpp:126-127`
**Status:** OBSERVATION — defensive design, not a bug.

```cpp
std::vector<uint8_t> dummySalt(SALT_LENGTH, 0xA5);
(void)PBKDF2_HMAC_SHA256(password, dummySalt, DEFAULT_ITERATIONS, HASH_LENGTH);
```

The dummy salt is hardcoded (16 bytes of `0xA5`). This is intentional — the goal is to consume the SAME wall time as a legit verify on a stored hash, not to actually verify. The salt value doesn't matter for timing: PBKDF2 wall time is `O(iterations)` and independent of salt content for any fixed-length salt. The `0xA5` choice is arbitrary; what matters is `SALT_LENGTH` matching what `ParseStoredHash` would produce (so HMAC's block-padding behaves identically). Confirmed correct.

### L-V4-6. ServiceEndpoint outer-parse hex prefix is 64 raw bytes
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:242`
**Status:** OBSERVATION — L-V3-8 close-out commentary.

`const std::size_t prefixLen = std::min<std::size_t>(raw.size(), 64);` caps at 64 raw bytes, encoded to 128 hex chars in the log. Sufficient for triage (attacker probe shapes, version-skew prefix bytes are detectable in the first 16-32 bytes). 64 is a reasonable choice — large enough to identify the frame, small enough not to flood logs on a sustained-attack probe.

### L-V4-7. `Crypto::HashPassword` first-call cost on `VerifyCredentials` username miss
**File:** `Server/Account/src/Handlers/InternalRpcHandlers.hpp:94-95`
**Status:** OBSERVATION — H-V3-10 fix has a one-time latency cost.

The function-local static `s_dummyPasswordHash` is computed on first call via `Crypto::HashPassword(...)`. C++11 magic-static guarantees thread-safe single init. First-call latency is one full PBKDF2 derivation (~150-400ms) so the FIRST username-miss after server start is ~2× slower than subsequent misses. Not a security issue (attackers don't know whether they're hitting first-call vs subsequent), and not a UX issue (one-time startup-adjacent cost), but worth knowing for ops who might see a single anomalous-latency event in the AuthorizeToken metrics after each Account.exe restart.

### L-V4-8. EventStore::AppendInTx only translates `pqxx::unique_violation` → `ConcurrencyConflict`
**File:** `Server/Account/src/Db/EventStore.hpp:98-100`
**Status:** OBSERVATION — by design.

```cpp
} catch (const pqxx::unique_violation& e) {
    throw ConcurrencyConflict(std::string("Append conflict: ") + e.what());
}
```

Other pqxx exceptions (`broken_connection`, `data_exception`, `integrity_constraint_violation`, etc.) propagate as-is. `AccountTransaction::Commit` doesn't catch them, so they propagate to the dispatcher's outer try/catch, which converts to `Internal server error` and triggers `~AccountTransaction()` → `Rollback()` → Memento restore + stale-mark. Behavior is correct: the rollback path doesn't care WHICH exception caused the throw, it just needs to know one happened. The narrow `unique_violation` catch is purely for the diagnostic-quality upgrade (semantic name in the log) without disturbing the propagation chain.

---

## Verified Closed (from v3)

| v3 item | Closure site | Notes |
|---|---|---|
| **M-V3-1 (BeginMinigame strict parse)** | `QuestHandlers.hpp:120` + comment `:112-119` | One-line mechanical migration; behavior unchanged on happy path (downstream empty-check still rejects), but the boundary is now explicit. |
| **M-V3-2 (GetInventory + GetCharacterStats strict parse)** | `AccountHandlers.hpp:81, :160` | Read-only handlers still strict — invariant-drift surface closed. |
| **M-V3-3 (Register + Login strict parse)** | `AuthServer.hpp:242, :307` | Documented at `:233-241` and `:305-306` that downstream empty-check is the previous boundary-equivalent; strict parse makes it explicit. |
| **M-V3-3 security (Crypto burn-PBKDF2 on parse failure)** | `Crypto.hpp:116-128` | Closes the timing-oracle window where a malformed stored hash would short-circuit. Result intentionally discarded; comments cite the H-V2-5 + H-V3-10 chain. |
| **L-V3-1 (RestoreFrom silent catch)** | `AccountTransaction.hpp:347-357` | Both `std::exception` and `...` arms emit `LOG_DATA_WARN`; stale-flag fallback at L362 preserved. |
| **L-V3-2 / L-V3-8 (ServiceEndpoint outer-parse diagnostic)** | `ServiceEndpoint.hpp:239-254` | When `method` is empty (outer parse failed), logs hex-prefix (64 raw bytes → 128 hex chars) + `totalBytes`. |
| **L-V3-3 (RestoreFrom log doc-comment)** | Account.hpp Snapshot doc updated | Comment at `Account.hpp:76-110` now accurately describes the C7-A-correct restore semantics. |
| **L-V3-4 (audit_log.target normalization confirmed defensive)** | `AccountTransaction.hpp:213-220` | Confirmed: only live caller (`ProgressionHandlers.hpp:543`) passes populated target; WARN fires only on the defensive-only null path. |
| **L-V3-6 (env-var caching in `InternalRpcAuth::GetSharedSecret`)** | `InternalRpcAuth.hpp:53-66` | static-local cache — env var captured once on first call. Operator cannot rotate without process restart (documented). |

---

## Focus Question Walkthrough

### 1. Every try/catch site — does it log, swallow, propagate?

Enumerated try/catch sites across the audit scope:

- **`AccountTransaction::Commit`** post-commit block (`:279-309`) — catches `std::exception` AND `...`, logs `LOG_DATA_WARN`, calls `MarkStaleForReload()`. Both arms log + recover. ✓
- **`AccountTransaction::Rollback`** tx_->abort path (`:326-332`) — catches `std::exception` AND `...`, logs `LOG_DATA_WARN`. No propagation (abort is best-effort). ✓
- **`AccountTransaction::Rollback`** RestoreFrom path (`:345-357`) — closed in v3; catches both, logs, falls through to MarkStaleForReload. ✓
- **`EventStore::AppendInTx`** (`:98-100`) — catches `pqxx::unique_violation`, translates to `ConcurrencyConflict` (semantic propagation). Other pqxx exceptions propagate. ✓
- **`EventStore::AppendIdempotent`** (`:135-144`) — catches `ConcurrencyConflict`, does the dedup-on-conflict re-check, re-throws if not idempotency-key-based. ✓
- **`ServiceEndpoint::HandleConnection`** outer try (`:164-260`) — catches `std::exception`, surfaces as `{"error", "Internal error: " + e.what()}` to the caller, logs `LOG_NET_ERROR` with method (if known) or hex prefix. ✓
- **`Crypto::ParseStoredHash`** (`:370-380`) — catches `...`, returns false. Caller VerifyPassword burns one PBKDF2 to mask the parse-failure timing gap. ✓
- **`Protocol::ParseJsonStrict`** (`:62-69`) — catches `Json::parse_error`, returns nullopt. Callers explicitly handle nullopt. ✓
- **`Protocol::ParseJsonSafe`** (`:42-52`) — catches `Json::parse_error`, returns empty object. No remaining callers in source (only the function definition exists; all call sites migrated to Strict). ✓
- **`AccountHandlers::HandleSetParty`** UUID parse (`:226-227, :264-265`) — catches `...`, returns error response. ✓
- **`AccountHandlers::HandleSetParty`** gear slot parse (`:280-290`) — catches `...`, returns error response. Range-checks via `std::stoi` THEN explicit `< Helmet || > Boots` range check. ✓
- **`ProgressionHandlers`** weapon instance ID parses (`:321-322, :411-412`) — catches `...`, returns error response. ✓
- **`QuestHandlers::VerifyQuestToken`** timestamp parse (`:928-933`) — catches `...`, logs DEBUG, returns false. ✓

**No silent-swallow patterns remain.** Every catch either logs, propagates, or returns an explicit error response. The Crypto::ParseStoredHash + VerifyPassword pair is the one case where catch returns false silently, but the burn-PBKDF2 follow-up mitigates the timing-side-channel that would otherwise reveal the parse failure to an attacker.

### 2. ParseJsonStrict vs ParseJsonSafe adoption

Confirmed via grep: zero remaining `ParseJsonSafe` call sites in `Server/` source code. All 13 handler ParseJson sites use Strict:
- Auth: HandleRegister (`AuthServer.hpp:242`), HandleLogin (`:307`).
- Account/Quests: HandleBeginMinigame (`:120`), HandleReportQuestProgress (`:185`), HandleClaimQuestReward (`:548`), HandleCompleteQuest (`:704`).
- Account/Gacha: HandlePull (`:60`), HandleMultiPull (`:310`).
- Account/Progression: HandleLevelCharacter (`:124`), HandleAscendCharacter (`:219`), HandleLevelWeapon (`:308`), HandleAscendWeapon (`:398`).
- Account/Account: HandleGetInventory (`:81`), HandleGetCharacterStats (`:160`), HandleSetParty (`:189`), HandleAddCurrency (`:324`).

`ParseJsonSafe` function still defined at `Protocol.hpp:42-52` for documentation and any future read-only handler that wants the lenient behavior. Could be retired entirely — see L-V4-9 below.

**L-V4-9 (added):** `ParseJsonSafe` is now used only in its own definition site. Either retire the function and remove the definition (force every future caller to make the strict/lenient decision explicitly) or document why the function is kept for the next audit. Cosmetic.

### 3. AccountTransaction destructor behavior

```cpp
~AccountTransaction() {
    if (state_ == State::Pending || state_ == State::Active) Rollback();
}
```

`AccountTransaction.hpp:59-64`. Both Pending (no lease acquired yet) and Active (lease + tx open) trigger Rollback. The Rollback path handles both:
- If `tx_` is null (Pending), the abort block at L317 short-circuits via `if (tx_)`.
- The RestoreFrom + MarkStaleForReload path runs unconditionally, so even a Pending state with a captured Snapshot gets the correct in-memory restore.

State machine transitions:
- Pending → Active via `EnsureOpen()` (first buffered op or Commit's flush path).
- Pending → Committed via `Commit()` early-return when no buffered ops + no dirty (M2 short-circuit).
- Pending → RolledBack via dtor / explicit Rollback when nothing was ever buffered.
- Active → Committed via `Commit()` success.
- Active → RolledBack via dtor / Commit-throw / explicit Rollback.
- Committed / RolledBack are terminal — both Commit and Rollback short-circuit on these (`:144-145` and `:313`).

Double-Commit and double-Rollback are guarded. Dtor invariant holds.

### 4. Rollback restore completeness

`Account::Snapshot` X-macro at `Account.hpp:112-132` captures 18 directly-snapshotable mutable fields. Indirect fields handled manually: `rngState` (via `m_rng.State()` / `m_rng.SetState()`) and `collectionState` (via `m_collection.GetState()` / `m_collection.SetState()`).

Cross-referenced against `HandleClaimQuestReward`'s mutation surface (densest user, per Account.hpp:103-110 comment):
- `m_wallet` ✓ (via DispatchCurrencyReward)
- `m_questStates` ✓ (via q->state assignments, PropagateClaim, UnlockEligibleQuests)
- `m_storyLevel` / `m_storyXp` ✓ (via AddStoryXp)
- `m_difficultyTier` ✓ (via AdvanceDifficultyTier)
- `m_loginStreak` / `m_lastStreakDay` ✓ (via AdvanceStreakIfNewDay)
- `m_dirty` ✓ (captured directly)
- Indirect: m_rng (used in pulls but not claims), m_collection (used in pulls + progression).

All Account mutable members the v4-scoped handlers touch are in the Snapshot. The Snapshot's intentional exclusions (m_id, m_accountId, m_publicUid, m_stale) are documented at `Account.hpp:87-91` — hydrate-once / explicit-set / belt-and-suspenders.

### 5. Exception safety in EnsureOpen, AppendInTx, Commit

- **`EnsureOpen`** — see L-V4-4. Strong exception guarantee holds because `lease_` is RAII (member destructor returns the connection). Strict-exception-safe under the current member layout.
- **`AppendInTx`** — pre-check + INSERT run inside caller-provided `pqxx::work`. If the SELECT MAX throws (broken connection), the work is invalid; pqxx::work destructor calls abort. ConcurrencyConflict translates `unique_violation` for diagnostic clarity. Strong guarantee.
- **`Commit`** — multi-step (events loop → relational flush → outbox → audit → idempotency → tx_->commit → cursor advance → ClearDirty). Failure mode analysis:
  - Failure BEFORE `tx_->commit()`: pqxx::work destructor aborts. AccountTransaction state stays Active (or gets set to RolledBack by the outer scope unwinding through ~AccountTransaction → Rollback). DB rolled back; in-memory restored from Snapshot.
  - Failure DURING `tx_->commit()` (the pqxx commit itself throws): same as above — DB sees no commit, AccountTransaction unwinds via dtor → Rollback.
  - Failure AFTER `tx_->commit()` returns but before state_ is set to Committed: pqxx already committed durably; AccountTransaction's dtor will see state_ still Active and call Rollback, which will try to abort an already-committed tx (no-op in pqxx since tx_ was reset on L268) and then RestoreFrom — which would DESYNC from the committed DB. **But** the code orders this so `tx_.reset()` happens at L268 (after successful commit), `state_ = State::Committed` at L269 IMMEDIATELY after. Nothing can throw between L267's commit and L269's state assignment because L268's `tx_.reset()` only invokes the unique_ptr destructor on a pqxx::work that has already committed (idempotent — the work doesn't try to re-commit). Confirmed correct via code reading.
  - Failure in post-commit catch (cursor advance / ClearDirty): caught at L300-309, logged, MarkStaleForReload, swallowed. DB durably committed, in-memory will reload on next access. ✓

### 6. Stale-flag fallback when restore itself throws

Restore can throw if Account's std::unordered_map move-assignments throw allocator-bad-alloc. v3 closed L-V3-1 by adding catch + log. The MarkStaleForReload at L362 happens AFTER the try/catch around RestoreFrom, so it runs regardless of whether restore succeeded or threw. Next GetLockedAccount sees `IsStale() == true` and erases + reloads from DB.

There's a subtle case: if RestoreFrom partially completes (some X-macro fields restored, then bad_alloc on a later field), the Account is in an intermediate state — half-restored, half-post-mutation. MarkStaleForReload guarantees the next read uses a clean reload from DB, so the intermediate state never escapes the stripe lock. ✓

### 7. Log-or-throw discipline (programmer bugs vs operational failures)

Programmer-bug throws (`std::logic_error`):
- `AccountTransaction::Commit` already-committed (`:145`) — throws.
- `AccountTransaction::EnsureOpen` already-committed (`:393`) — throws (see L-V4-3 for the missing log).
- `AccountServer` constructor null-repository (`:100`) — throws.
- `InitializeBanners` / `InitializeTemplates` / `InitializeProgression` (`AccountServer.hpp:253, 259, 265`) — throw `std::runtime_error` on init failure (operational, but at startup the only sensible response is to crash, so propagating to main is correct).

Operational-failure paths:
- DB connection failure → propagates through pqxx → caught at AccountTransaction Commit or Rollback boundaries.
- Auth RPC failure → ServiceClient::Call returns RpcCallResult with Unreachable/WireError → callers reject the operation without throwing.
- Session validation failure → ValidateSession returns AuthResult — no throw path.

**Discipline:** programmer-bug surfaces throw (with at most one missing log site, L-V4-3); operational surfaces use result objects + explicit error responses. Consistent.

### 8. Comparison to v3 follow-up

v3 closed the door on the "every mutating handler uses strict parse" invariant. v4 confirms the closure holds and adds the Crypto burn-PBKDF2 defense-in-depth that v3 deferred to M-V3-3 security. The only structural change from v3 is the H-V3-1 scope expansion landed for SetParty/ReportQuestProgress/CompleteQuest, leaving HandleClaimQuestReward's TickQuests-before-Begin as v4's L-V4-1.

---

## Test Coverage Status (this dimension)

- C7 Rollback restore tested via `AccountTransactionTest.cpp:182-231`. Tests EXPLICIT Rollback only; dtor-via-RAII path uncovered (L-V4-2).
- M-V3-3 security burn-PBKDF2: not directly tested. A targeted unit test calling `Crypto::VerifyPassword(pwd, malformed_hash)` and asserting both `false` AND elapsed-time ≥ N ms would close this gap.
- L-V3-1 RestoreFrom log: not testable without a mock that injects an allocator failure into Account's map move-assignment. Defer.
- L-V3-2 ServiceEndpoint outer-parse hex log: not directly tested. A test that sends `"not-json"` over the wire and asserts `LOG_NET_ERROR` contains the hex prefix would close this.
- M-V3-1/2/3 strict-parse adoption: tested implicitly via the existing handler tests' happy-path coverage; no negative tests confirm "malformed payload → Invalid request payload" response.

**Recommended top-3 additions for this dimension:**
1. Dtor-RAII Rollback test (L-V4-2): construct AccountTransaction inside a scope, throw from a handler-like lambda, assert restore + stale.
2. Crypto::VerifyPassword timing-equivalence test: feed legit hash, dummy hash, and malformed hash; assert all three take ≥ N ms.
3. Per-handler "malformed payload rejected with explicit error" smoke test (one per ParseJsonStrict adopter — ~14 handlers, ~14 test cases).

---

## Suggested triage order

**This week (closes L-V4-1):**
1. **L-V4-1** — move `Begin()` above `TickQuests::Apply` in HandleClaimQuestReward (QuestHandlers.hpp:579 → 605). Same 1-line mechanical fix as the H-V3-1 sites. The v4 event-sourcing audit has this as **H-V4-1** since the snapshot-correctness angle is sharper; both audits agree on the fix.

**Before launch:**
2. **L-V4-2** — add the dtor-RAII Rollback test. ~30 LOC, no new test fixtures needed.
3. **L-V4-3** — add `LOG_NET_ERROR` before EnsureOpen's logic_error throw. ~2 LOC.

**Eventually (cosmetic):**
4. **L-V4-9** — retire `ParseJsonSafe` from Protocol.hpp now that no handler uses it.

---

## Verdict

The error-handling + rollback dimension is **clean**. The v3 remediation arc landed every Medium and Low in this dimension. The single new finding (L-V4-1) is the error-handling angle on a defect that the v4 event-sourcing dimension already owns as H-V4-1 (with the same mechanical fix). No new Critical, no new High, no new Medium. Test coverage of the dtor path is the most material residual gap.
