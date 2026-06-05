# Follow-up Audit: Error Handling + Rollback Chain
**Date:** 2026-06-02
**Auditor:** Auditor #5 (follow-up to 2026-06-02-server-persistence-audit)
**Scope:** AccountTransaction commit/rollback chain, Memento snapshot, every error path through the persistence pipeline, exception propagation from handlers → dispatcher → wire.
**Method:** Walked every documented throw site in `Commit`/`Rollback`/`EnsureOpen`/AppendInTx/RelationalFlush/idempotency/audit_log/outbox, plus snapshot capture/restore semantics. Cross-checked dispatcher behavior (TcpServerBase catches `std::exception` → `Internal server error`). Reviewed handler order-of-operations against snapshot-capture timing.

---

## Summary

The Memento + stale-flag + `try { tx_->abort(); } catch` chain is correctly **wired** at the AccountTransaction level — all the failure paths described in the mission terminate in `state_ == RolledBack`, snapshot moved into the Account, and `MarkStaleForReload()`. **However, the snapshot is captured at the wrong instant relative to handler mutations** — every event-sourced handler (Pull, MultiPull, Progression, ClaimQuestReward) mutates wallet/pity/collection state BEFORE constructing `AccountTransaction`, so `CaptureSnapshot()` captures the already-mutated state. On `Commit()` failure, `RestoreFrom` restores the in-memory account to that post-mutation point — defeating C7's "rollback unwinds speculative mutations" guarantee. The stale flag still saves the day (next `GetLockedAccount` reloads from DB), but the current handler's stack is still holding a mutated `Account&` between the throw and the dispatcher's catch, and any in-handler code that touches `account` after `Commit()` would see speculative state. Two real M11 gaps remain (`HandleReportQuestProgress`, `HandleCompleteQuest` use `ParseJsonSafe`). Diagnostic surface: the empty `catch (...)` swallowing `RestoreFrom` failures loses evidence — a `LOG_DATA_WARN` belongs there.

---

## Critical

### C1. Memento snapshot captures POST-mutation state in every event-sourced handler
**Files:**
- `Server/Account/src/GachaHandlers.hpp:133,145,170,182,289` (Pull — mutations before `Begin`)
- `Server/Account/src/GachaHandlers.hpp:~430-614` (MultiPull — same pattern, `Begin` at L614)
- `Server/Account/src/ProgressionHandlers.hpp` (Level/Ascend — wallet spent before txn ctor at the bottom of the handler)
- `Server/Account/src/QuestHandlers.hpp` (ClaimQuestReward — quest state + materials mutated before `Begin`)
- `Server/Account/src/AccountTransaction.hpp:49` (`preTxSnapshot_(account.CaptureSnapshot())`)
**Status:** NEW — regression of C7's intent.

C7's commit message says "rollback restores in-memory state via Memento snapshot." The implementation is sound in isolation — `CaptureSnapshot()` and `RestoreFrom` are correct, snapshot lives in the ctor's member init list, dtor calls `Rollback()` which calls `RestoreFrom`. But the snapshot timing is wrong relative to the handler protocol.

Concrete walk: `HandleSinglePull` (GachaHandlers.hpp):
```
L133:  account.GetWallet().TrySpendForPullByType(slot->ticketType);  // wallet -1
L145:  account.RecordPull(...);                                       // stats++
L170:  account.GetCollection().Dispatch(CollectionAction::AddCharacter{...});
L182:  account.GetCollection().Dispatch(CollectionAction::AddWeapon{...});
...
L289:  auto txn = m_ctx.repository->Begin(account);  // CaptureSnapshot HERE
L290:  txn.AppendEvent(std::move(pullEvent));
L291:  txn.AppendEvent(std::move(walletEvent));
L293:  txn.Commit();   // <-- if this throws
```

When `Commit()` throws (e.g., `ConcurrencyConflict` from AppendInTx), `~AccountTransaction()` runs Rollback → RestoreFrom. But `preTxSnapshot_` already has `wallet.tickets - 1`, `stats.totalCharacterPulls + 1`, the new character/weapon in the collection. The "restore" leaves all those mutations in place.

**Effect:** Stale flag (C1 fix from prior audit) still catches this on the *next* RPC — `GetLockedAccount` sees `IsStale() == true`, evicts, reloads from DB. So the system eventually converges. But:
1. The just-failed handler is still unwinding through `~AccountTransaction()` → throw propagates → TcpServerBase catches → `Internal server error`. The client sees an error (good — at least not a false success). But the **assurance C7 was sold on** — "by the time the throw unwinds the Account is back to its pre-transaction values" (Account.hpp:60-61) — is **false**.
2. If `OnStopped` or `CleanupIdleAccounts` were to flush this account BEFORE the next `GetLockedAccount` reload, the stale-check (C2) catches that. But the order-of-operations comment in Account.hpp lies about what RestoreFrom actually accomplishes.
3. Any future code that reads `account_` after `Rollback()` returns (e.g., a hypothetical handler that recovers from a commit failure and tries to return a "we already deducted credits in memory" response) would observe ghost mutations.

**Fix:** Either
  (a) Move the `Begin()` call to the top of every event-sourced handler so `CaptureSnapshot` runs BEFORE the first mutation. Pull, MultiPull, Level/Ascend, ClaimQuestReward all need this reorder. Cheap because `Begin` is now lazy (M2) — no DB connection cost.
  (b) Refactor handlers to build event payloads from a transaction-local buffer and only apply to `Account` after `Commit()` succeeds (the larger C7 refactor the prior audit explicitly deferred). This is the correct long-term fix but is the size of a 2-day workstream.

Recommend (a) for the launch-blocking patch. Also update the Account.hpp Snapshot doc comment to reflect actual semantics. Add an integration test: pull → force AppendInTx throw → assert wallet/stats/collection restored.

---

## High

### H1. M11 gap — two mutating quest handlers still use `ParseJsonSafe`
**Files:**
- `Server/Account/src/QuestHandlers.hpp:168` (`HandleReportQuestProgress`)
- `Server/Account/src/QuestHandlers.hpp:584` (`HandleCompleteQuest`)
**Status:** NEW — M11 fix incomplete.

M11's recent fix migrated mutating handlers from `ParseJsonSafe` to `ParseJsonStrict` so a malformed payload is rejected at the boundary instead of falling through to defaults. The fix landed for Pull, MultiPull, AddCurrency, SetParty, Level/Ascend, ClaimQuestReward — but `HandleReportQuestProgress` (mutates quest progress, calls `TickQuests::Apply`, writes events) and `HandleCompleteQuest` (mutates quest state, completes button-type quests, writes events) still use `ParseJsonSafe`.

A malformed `ReportQuestProgress` payload becomes `quest_id=""`, `event_type=""`, `metadata={}` — the `quest_id.empty()` guard at L178 rejects with "Missing quest_id", so the *current* behavior degrades gracefully. But the M11 design contract was "every mutating handler uses strict parse"; this is a regression on the invariant that future refactors will rely on (the next person adding an int field with no `.value(...,default)` guard would silently default to 0).

**Fix:** Migrate both handlers to `ParseJsonStrict` + early "Invalid request payload" return, matching the pattern used in the other quest/progression/gacha handlers.

### H2. `tx_->commit()` throw path — `state_` transition is timing-dependent
**Files:** `Server/Account/src/AccountTransaction.hpp:235-237`
**Status:** VERIFIED-OPEN — code is correct but the comment misleads about why.

The sequence at Commit L235-237:
```cpp
tx_->commit();
tx_.reset();
state_ = State::Committed;
```

If `tx_->commit()` throws (deferred-commit failure — e.g., a constraint violation surfaced at COMMIT instead of at exec), control jumps to the catch-nothing path → unwind. The dtor runs, sees `state_ == State::Pending || State::Active` (still `Active` because the line never reached `Committed`), calls `Rollback`. Rollback checks `if (tx_)` — `tx_` is still owned because `tx_.reset()` never ran. So `tx_->abort()` is called on a connection whose tx Postgres has already rolled back (pqxx will likely throw `broken_connection` or no-op). The new try/catch around abort logs it. **State ends correctly: RolledBack, snapshot restored, stale set.**

The comment at L232-234 says "After this returns the DB has the change durably; from the client's perspective the action succeeded regardless of what happens in the in-memory bookkeeping below." That comment is correct ONLY for the success-path semantics — it gives the reader the impression that everything past `tx_->commit()` is post-success bookkeeping. The actual code is correct (the M19 try/catch is below `state_ = Committed`, so a commit-throw does the right thing), but a maintainer reading L232-234 might "tidy up" by moving `state_ = State::Committed` above `tx_->commit()`, which would silently break the rollback path on commit-throw — the dtor would see `Committed` and skip Rollback, leaking the speculative mutations + leaving stale-flag unset.

**Fix:** Add a comment at L235-237 explicitly noting "state_ must be set AFTER tx_->commit() returns so a deferred-commit throw triggers Rollback in the dtor."  Optionally add a test that injects a commit-throw and asserts rollback semantics.

### H3. `RestoreFrom` exception is silently swallowed — loses diagnostic evidence
**Files:** `Server/Account/src/AccountTransaction.hpp:307-314`
**Status:** NEW — diagnostic gap.

```cpp
try {
    account_.RestoreFrom(std::move(preTxSnapshot_));
} catch (...) {
    // Restoring is best-effort. If the snapshot move itself fails
    // (allocator failure on a half-destroyed snapshot, etc.) the
    // stale flag below guarantees the next GetLockedAccount
    // reloads from DB and discards the half-restored state.
}
```

The comment justifies the swallow ("stale flag is belt-and-suspenders"). The justification is correct — the next reload from DB heals everything. But the catch is silent: a torn allocator state, a half-destroyed Snapshot's destructor invariant violation, an `unordered_map`-move-throw-on-hash-equality-cmp — none of these would surface in logs. The C7 design comment in Account.hpp:60-67 implies RestoreFrom is critical to correctness; if it's silently failing on every commit-rollback path in production, we'd never know.

Asymmetry: L1's fix added logging to `tx_->abort()`'s catch with the explicit justification "losing it entirely hid a class of 'post-Commit-failure' investigations during the C7 design pass." The same justification applies here, and was overlooked.

**Fix:** Add `LOG_DATA_WARN("AccountTransaction::Rollback: RestoreFrom threw, relying on stale-flag fallback")` (or with `e.what()` if you split the catch into `std::exception` + `...`).

---

## Medium

### M1. `AccountServer.hpp:428` — `CleanupIdleAccounts` still doesn't check Save() return value
**Files:** `Server/Account/src/AccountServer.hpp:426-430`

```cpp
for (auto& [playerId, account] : accountsToSave)
{
    if (account) m_repository.Save(*account);   // <-- return value ignored
    LOG_DATA_DEBUG("Unloaded idle account: {}", playerId);
}
```

`OnStopped` (the parallel L8 fix) now checks the bool and logs failures. `CleanupIdleAccounts` was missed. Idle-evict of an account whose Save fails (DB partition, pool exhausted) is silently lost. The account has already been moved out of `m_accounts` by the time we get here, so a failed Save means the in-memory state is unrecoverable until the next login (which rehydrates from the last-committed DB state — fine semantically, but if any uncommitted-but-flushed dirty bits were riding on this Save, they're gone with no log entry).

**Fix:** Mirror OnStopped's check-and-log. Increment a failed-evict counter that the cleanup-tick logs at debug.

### M2. Stale account on `OnStopped` still hits Save() before C2's stale check
**Files:** `Server/Account/src/AccountServer.hpp:163-174`, `Server/Account/src/AccountRepository.hpp` (Save)

C2's fix in `AccountRepository::Save` early-returns on `account.IsStale()`. Confirmed in the recent commit. But `OnStopped`'s loop calls `Save(*account)` for every cached account, including stale ones. The early-return inside Save handles this correctly — but the log message at AccountServer.hpp:172 (`failed to save account_id=...`) will fire on a stale-skip, since Save returns `false`. This conflates "stale-skip" with "Save genuinely failed".

**Fix:** In OnStopped, log stale-skips at DEBUG, only log ERROR for non-stale Save failures. OR have Save return an enum (`Saved`, `SkippedStale`, `Failed`) instead of bool, so callers can disambiguate.

### M3. PoolExhausted during EnsureOpen leaks the Pending state — minor lease accounting risk
**Files:** `Server/Account/src/AccountTransaction.hpp:341-348`, `Server/Account/src/db/ConnectionPool.hpp:75-84`

EnsureOpen calls `lease_ = pool_.acquire()` which throws `PoolExhausted`. The exception propagates out of EnsureOpen. `state_` is still `Pending` (the `state_ = Active` line at L347 was never reached). The dtor sees `Pending`, calls Rollback. Rollback sees `tx_ == nullptr`, skips abort. RestoreFrom runs (snapshot was already captured in ctor — pre-EnsureOpen). State → RolledBack. Stale set. **Path is correct.**

However: `acquire()` throws BEFORE assigning to `lease_`. The Lease move-assignment operator (`Lease::operator=`) is the only way `lease_` ever changes, and assignment never happened. So `lease_` remains its default-constructed empty state. `~Lease` checks `if (pool_ && conn_)` — both are nullptr, so it's a no-op. **No leak.** This is correct, but it depends on `pool_.acquire()` being strongly-exception-safe (it must not have partially-moved a connection out of `free_` before throwing). Looking at the acquire() body — the throw is from the wait_for predicate failure, before the `std::move(free_.front())`. So acquire IS strongly-exception-safe. **Verified.**

Filing as Medium only because the chain "throw → strongly-safe move semantics → empty Lease destructor no-op" is a non-obvious correctness chain with no test gating it. A unit test that fills the pool, opens a 17th txn, and asserts (a) `PoolExhausted` is thrown, (b) `account.IsStale() == true`, (c) the pool capacity is unchanged after the txn's dtor runs, would lock this in.

### M4. `audit_log.target` is `NOT NULL` in schema but L7's fix only handles `before/after` nulls
**Files:** `Server/Account/schema.sql:348`, `Server/Account/src/AccountTransaction.hpp:184-200`

Schema: `target JSONB NOT NULL`. The L7 fix wraps `before` and `after` in `std::optional<std::string>` to write SQL NULL on `is_null()`. But `target` is dumped unconditionally:
```cpp
a.target.dump()      // <-- if a.target.is_null(), this is the string "null"
```
A handler calling `RecordAudit(actor, action, nlohmann::json{}, ...)` (target is a default-constructed null Json) writes the string `"null"` to the column. Doesn't violate NOT NULL (it's still a JSON value), but it carries forward the same ambiguity that L7 was fixing for before/after.

Today only one handler uses `RecordAudit` (`ProgressionHandlers.hpp:511`) and it passes a real target. So this is latent — but the same trap is set for the next caller who passes `nullptr` / `{}`.

**Fix:** Either mirror L7's nullopt handling for `target` (treating null-target as a programmer error and explicitly rejecting at the boundary), or document that target must be non-null and assert in RecordAudit (`assert(!target.is_null())`).

### M5. Idempotency cache write — silent no-op on stripe-lock starvation
**Files:** `Server/Account/src/AccountTransaction.hpp:215-230`

The H2 fix changed `ON CONFLICT DO NOTHING` to `DO UPDATE`. Correct for expired-row-replacement.

But: if `idempotency_.push_back(...)` is called twice with the same scoped_key in one transaction (handler bug), the second `INSERT ... ON CONFLICT DO UPDATE` runs against the row the first INSERT just wrote — silently replacing the first row's response_payload with the second's. No error, no warning. Today only one StoreIdempotency call exists per handler, so this is latent.

**Fix:** Defensive `std::unordered_set` of seen keys inside `StoreIdempotency`, or just assert in debug builds.

---

## Low / Observation

### L1. Account.hpp:60-61 comment overstates RestoreFrom guarantee
**File:** `Server/Account/src/Account.hpp:60-67`
The comment says "by the time the throw unwinds the Account is back to its pre-transaction values." This is true only if `AccountTransaction` is constructed BEFORE handler mutations — which is currently the opposite of the convention. See C1. Update the doc-comment to match real semantics OR enforce the ordering in handlers and keep the comment.

### L2. `tx_->abort()` is called even when the underlying pqxx::work is already rolled back
After `tx_->commit()` throws, the DB-side tx is already torn down. The subsequent dtor → Rollback → `tx_->abort()` call typically logs "broken connection" or similar. Not wrong (defensive), but the new L1 log line will fire noisily on every commit-throw. Consider adding an `m_committed_or_dead` bool to skip the abort when commit() already threw. Cosmetic.

### L3. `IsOpen()` doesn't distinguish Pending vs Active to external observers
`bool IsOpen() const { return state_ == State::Pending || state_ == State::Active; }` returns true for a Pending (no-lease) tx. Tests that assert "tx was actually opened" use `DidOpenTx()`. The asymmetry is fine but worth a comment that `IsOpen()` is the mutability check, not the "DB tx exists" check.

### L4. PoolExhausted message includes timeout but not the connection-string identity
`"ConnectionPool::acquire timed out after 5000ms"` — fine for one-node setups, useless when multiple pools exist (future combat-server, snapshot worker). Add the pool's identity if/when that lands.

### L5. `EnsureOpen` throws `std::logic_error` on terminal-state — masks the original cause
If a handler mistakenly calls `AppendEvent` after `Commit()` succeeded, EnsureOpen at L344 throws `std::logic_error("AccountTransaction already committed or rolled back")`. The dispatcher catches and returns "Internal server error". The original cause (programmer bug) is not in the log. A `LOG_NET_ERROR` before the throw would surface it.

---

## Verified Closed

1. **C1 (stale flag honored):** `AccountServer.hpp:458-462` — `GetLockedAccount` checks `IsStale()`, erases under map lock, falls through to DB reload. Correct.
2. **C2 (Save early-return on stale):** `AccountRepository::Save` early-returns; both `CleanupIdleAccounts` and `OnStopped` call Save which now respects the stale check.
3. **C6 (version pre-check in commit path):** `AccountTransaction::Commit` calls `store_.AppendInTx(*tx_, ev)` which runs the `SELECT MAX(version)` pre-check inside the same pqxx::work. Verified at AccountTransaction.hpp:161-163.
4. **C7 (Memento snapshot + restore):** The mechanism is wired correctly at the AccountTransaction layer — but the snapshot is captured at the wrong instant relative to handler mutations. See new C1 above for the gap.
5. **H5 (pool exhaustion bounded):** `ConnectionPool::acquire` uses `cv_.wait_for(acquire_timeout_)` and throws `PoolExhausted`. Propagation through EnsureOpen → handler → dtor → Rollback is correct.
6. **L1 (tx_->abort logs):** Added in AccountTransaction.hpp:288-294. Splits std::exception vs ... cleanly.
7. **L7 (audit_log nulls written as SQL NULL):** `std::optional<std::string>` handling at AccountTransaction.hpp:184-200. Correct for before/after; target still has the ambiguity (see M4).
8. **L8 (OnStopped Save() return value logged):** AccountServer.hpp:169-174. CleanupIdleAccounts is the parallel-loop that was missed (see M1).
9. **M11 (strict parse for mutating handlers):** Migrated for Pull/MultiPull/AddCurrency/SetParty/Level/Ascend/ClaimQuestReward; gap in ReportQuestProgress + CompleteQuest (see H1).
10. **M12 (gear slot range check):** Confirmed at `AccountHandlers.hpp:249-276`; the conversion is now wrapped in a try/catch that returns an error response on malformed slot.
11. **M19 (post-commit bookkeeping caught + swallowed):** Try/catch at AccountTransaction.hpp:247-271; logs + MarkStaleForReload; client sees success. Correct because DB IS committed at this point.
12. **AccountTransaction is move-disabled, copy-disabled:** L54-57. Verified.
13. **`state_ == Committed || RolledBack` guard on Commit + Rollback:** L130-131 (Commit throws), L275 (Rollback returns early). Correct.
14. **Dispatcher catches throws → "Internal server error" to client:** `TcpServerBase.hpp:487-496`. No handler can return a partial-success payload after Commit throws — the throw bypasses the return statement.

---

## Assurance — what the recent fixes provide

- **Stale-flag end-to-end:** The C1+C2 chain now means any commit failure → next handler reload from DB → no double-spend, no version drift, no leaked dirty bits. Even if the C1 finding above lands (snapshot-at-wrong-instant), the system still self-heals on the next RPC.
- **Bounded pool acquisition:** H5's timeout means a stuck DB no longer hangs the entire service via stripe-lock cascade. Worst case is per-handler timeout + per-handler stale-mark.
- **Strict parse for the spend-money handlers:** M11's strict-parse path means malformed `AddCurrency` no longer grants `amount=1` by default; malformed pull payloads no longer silently pull from the empty-string slot.
- **Idempotency cache refresh on retry:** H2's DO UPDATE means long-tail retries of an expired-cache action now repopulate the cache instead of silently re-executing.
- **Dispatcher exception safety:** TcpServerBase's catch-`std::exception` is the universal backstop. No handler can return a partial-success response after Commit throws — the throw aborts the return path. Verified.
- **State machine correctness on commit-throw:** Because `state_ = State::Committed` is set AFTER `tx_->commit()` returns, a deferred-commit throw routes through the dtor → Rollback → snapshot restore + stale-mark path. Verified by code reading; should be locked in with a fault-injection test (see M3 + H2).
- **Outbox INSERT cannot throw on uniqueness:** outbox table has only `outbox_id BIGSERIAL PRIMARY KEY` — no application-level uniques. Verified at schema.sql:329-336.
- **Audit_log NULL handling:** before/after columns now correctly write SQL NULL (L7). `WHERE before IS NULL` works. target column still has the ambiguity (M4).

The Memento snapshot is the right design — it just needs to be captured before any handler mutation, not after. Once C1 above is fixed (either by reordering Begin to the top of every handler, or by the larger "buffer mutations, apply after Commit" refactor), the rollback chain is genuinely sound end-to-end.
