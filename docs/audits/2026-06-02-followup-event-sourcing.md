# Follow-up Audit: Event Sourcing + Atomicity
**Date:** 2026-06-02
**Scope:** Re-verification of C6/C7/M2/M19/H8/H9/H10/M13 fixes; new findings introduced or exposed by the remediation pass.
**Method:** Read-through of `AccountTransaction.hpp`, `EventStore.hpp`, `Account.hpp` (Snapshot/RestoreFrom), `AccountRepository.hpp` (LoadEventVersions + Save), `EffectDispatcher.hpp`, `AccountDirty.hpp`, and the four event-emitting handlers (`GachaHandlers`, `QuestHandlers`, `AccountHandlers`, `ProgressionHandlers`).

## Summary

The version-cursor + idempotency fixes are correct in their own scope and chain naturally in multi-event commits via `AppendInTx`'s in-tx `SELECT MAX`. However, the C7 Memento pattern has a **critical correctness gap exposed by its interaction with the existing handler pattern**: handlers mutate the live `Account` *before* calling `repository->Begin(account)`, so the snapshot captured by `AccountTransaction`'s constructor reflects the **post-mutation** state, not the pre-RPC state. On Rollback, the snapshot restore is a no-op (the snapshot already encodes the speculative mutations) — the entire C7 promise reduces to belt-and-suspenders, and recovery relies solely on the C1 stale-flag reload. Two additional issues: the H10 dispatcher cursor is wrong if `EffectDispatcher` is ever wired alongside handler-built wallet events, and the C6 in-tx `SELECT MAX` still lacks `FOR UPDATE` so the cross-process race the original C6 called out is not closed for any future horizontal scale.

---

## Critical

### C1. C7 snapshot captures post-mutation state — Memento restore is a no-op for every event-sourced handler
**Files:**
- `Server/Account/src/AccountTransaction.hpp:49` (snapshot captured in constructor)
- `Server/Account/src/GachaHandlers.hpp:117-184,289` (pull mutates pity/RNG/wallet/collection, THEN Begin)
- `Server/Account/src/GachaHandlers.hpp:614` (multi-pull: same pattern)
- `Server/Account/src/AccountHandlers.hpp:281-283,289` (SetParty mutates, then Begin)
- `Server/Account/src/AccountHandlers.hpp:362,396` (AddCurrency: `wallet.AddBy(...)` then Begin)
- `Server/Account/src/ProgressionHandlers.hpp:426,433,479` (SpendScrap + Dispatch then Begin)
- `Server/Account/src/QuestHandlers.hpp:381-388,402,412,420,572` (applyCurrency / streak / tier / propagate, then Begin)
**Status:** REGRESSION (introduced by C7 fix; the design claim in the C7 comment block is false)

The C7 comment promises:

> by the time the throw unwinds the Account is back to its pre-transaction values
> a handler whose Commit throws can no longer build a "success" response from speculatively mutated state

But the snapshot is captured in `AccountTransaction`'s constructor (`preTxSnapshot_(account.CaptureSnapshot())` at `AccountTransaction.hpp:49`), which runs at `repository->Begin(account)`. Every event-sourced handler mutates `account` BEFORE calling `Begin()`:

`GachaHandlers::HandlePull` example:
```cpp
account.GetWallet().TrySpendForPullByType(slot->ticketType);   // line 133
PullResult result = banner.Pull(account.GetRNG(), pity, guarantee, ...);  // 144
account.RecordPull(...);                                          // 145
account.GetCollection().Dispatch(CollectionAction::AddCharacter{...});  // 170
// ... 100+ lines of payload construction ...
auto txn = m_ctx.repository->Begin(account);   // 289 — snapshot captured HERE
txn.AppendEvent(std::move(pullEvent));
txn.AppendEvent(std::move(walletEvent));
txn.Commit();
```

So `preTxSnapshot_` already contains: the decremented tickets, the bumped pity counter, the advanced RNG state, the new character in the collection, the incremented stats. If `Commit()` throws (DB down, version collision, idempotency-cache UNIQUE backstop fires), `Rollback()` calls `account_.RestoreFrom(preTxSnapshot_)` — which puts the account right back into the **post-mutation** state. DB rolled back to pre-pull; memory restored to post-pull. Same divergence the previous C7 was supposed to fix.

Recovery only works because of the belt-and-suspenders `MarkStaleForReload()` at `AccountTransaction.hpp:319` plus the C1 fix in `GetLockedAccount` that evicts on `IsStale()`. So the system is functionally safe, but the C7 fix as shipped provides no actual benefit beyond the C1 stale flag — the Memento pattern is dead code on every real path.

**Fix (architectural — pick one):**
1. **Buffer mutations** in transaction-local structs and apply them to `Account` only inside `Commit()` after the DB write succeeds. This is the larger refactor the original C7 finding called out and is what makes the C7 design semantically correct.
2. **Move `Begin()` to the top of each handler** (immediately after `getLockedAccount`) so the snapshot captures the true pre-RPC state. Cheaper change, but it means every handler now holds the connection lease through its full body — undoes part of the M2 lazy-acquisition win. Still strictly better than the current state.
3. **Add a `CaptureSnapshot(Account&)` call to each handler at the top** and pass it explicitly into `Begin()`. Preserves M2 but requires per-handler discipline.
4. **Document the current limitation** and rely on C1 stale-flag eviction as the sole recovery mechanism — drop the C7 RestoreFrom call entirely so future readers don't mistakenly trust the snapshot path. (Lowest-effort acknowledgment.)

Either way: the comment block at `Account.hpp:51-72` and `AccountTransaction.hpp:299-306` is currently misleading and should be corrected.

---

## High

### H1. `EffectDispatcher::wallet_version_cursor_` collides with handler-buffered wallet events
**Files:** `Server/Account/src/EffectDispatcher.hpp:34`, `Server/Account/src/AccountTransaction.hpp:161-163` (AppendInTx loop)
**Status:** NEW — latent until EffectDispatcher is wired in

H10 fixed the dispatcher's two-effects-collide-on-cached+1 bug by seeding a local cursor from `account.Dirty().cached_wallet_version` and pre-incrementing it on each `GrantCurrencyEffect`. But the cursor is seeded ONLY from the cached value, not from any wallet events the handler has already queued on `txn_` for the same commit.

Concrete scenario: a future quest-claim handler that uses `EffectDispatcher` for reward effects but still builds the claim event by hand:
```cpp
auto txn = repo->Begin(account);
// Handler manually queues a wallet event:
walletEvent.version = dirty.cached_wallet_version + 1;  // e.g. version 6
txn.AppendEvent(walletEvent);
// Dispatcher runs the GrantCurrencyEffect from the reducer:
EffectDispatcher disp(account, txn);   // cursor seeded at 5
disp(grantCurrencyEffect);              // emits ev.version = ++cursor = 6  ← COLLIDES
```

At Commit time `AppendInTx`'s in-tx SELECT MAX catches it on the second INSERT (MAX returns 6 from the handler's INSERT, dispatcher's event has version=6 too, `6 <= 6` → ConcurrencyConflict). So the safety guard fires correctly — the COMMIT aborts, the player sees an error, the cache reloads. But the design intent of H10 was that the dispatcher coexists with manual handler events without coordination, and as written it cannot.

**Fix:** Either (a) take a `starting_cursor` parameter so handlers pass `dirty.cached_wallet_version + manualEventCount`, or (b) move cursor advancement into `AccountTransaction::AppendEvent` itself so the dispatcher and handler share a single source of truth that already reflects everything queued so far. Option (b) is cleaner and would also let handlers stop computing `cached_*_version + 1` by hand.

### H2. Schema-level event-version race remains open under multi-instance scale
**Files:** `Server/Account/src/db/EventStore.hpp:37-46`, `Server/Account/schema.sql:285`
**Status:** VERIFIED-OPEN (acknowledged in original C6 as "horizontal scaling fragility"; not closed by C6 fix)

The C6 fix moves the `SELECT MAX(version)` pre-check into the commit's `pqxx::work`. But the SELECT does not include `FOR UPDATE` and the UNIQUE backstop `(account_id, aggregate_kind, version, created_at)` includes `created_at`. Under READ COMMITTED (Postgres default), two concurrent tx on different connections (different instances or stripe-lock bypass) both see the same MAX, both INSERT version=N+1, both succeed if their `created_at` microseconds differ. Per-player stripe locks save us for single-instance, single-process, but the schema invariant is still not enforced.

The original C6 mitigation suggestion "OR document explicitly that stripe locks are the sole guard and add startup checks" — the documentation now lives in the AppendInTx comment ("the unique constraint cannot enforce monotonic version on its own — two appends at different timestamps slip past it"), but there's no startup check that horizontal scaling is forbidden, and no advisory-lock or `FOR UPDATE` row lock that would close the multi-process hole.

**Fix:** Add `pg_advisory_xact_lock(account_id, aggregate_kind_hash)` at the top of `AppendInTx` — same `pqxx::work`, lock auto-released at commit. Negligible cost (one round-trip), closes the multi-instance race regardless of stripe-lock bypass paths.

### H3. Post-commit bookkeeping catch block masks version-cursor drift on transient failures
**Files:** `Server/Account/src/AccountTransaction.hpp:247-271`
**Status:** NEW (introduced by M19 try/catch)

The M19 catch is correct in principle (DB is committed, don't propagate a fake failure to the client), but the recovery has a subtle gap. The loop at lines 253-261 is the ONLY place that advances `cached_*_version` after a successful commit. If `ClearDirty()` throws (it shouldn't realistically — `Clear()` is a struct reset — but the catch is defensive), the version cursors are NEVER advanced. The catch then sets `MarkStaleForReload()`, so the next handler reloads from DB via `LoadEventVersions` and gets the correct max → fine.

But there's a smaller window: if the cursor-advancement loop itself throws AFTER advancing some but not all aggregates' cursors (e.g., `dirty` reference becomes invalid mid-loop), the in-memory cursors are now PARTIALLY advanced. The `MarkStaleForReload` saves us only if `IsStale()` is checked before the next AppendEvent — which it IS via `GetLockedAccount`. So functionally correct, but worth a comment that this catch block is load-bearing for the "cursor in memory might be partially stale" invariant.

A more concerning interaction: the `MarkStaleForReload()` in the catch means the in-memory wallet/stats/collection state from the just-committed event is also discarded on next access — those values are correct in DB but the in-memory snapshot is "abandon and reload." That's the right call (DB is truth) but it does mean: any response payload built and returned to the client may reflect data that the in-memory cache then immediately forgets. Not a bug, just a worth-noting subtlety.

**Fix:** Add a comment at the catch block explaining that the cursor-advancement loop is the only source of truth for cursor monotonicity post-commit, and that MarkStaleForReload is the recovery contract for any partial failure within. Consider running the version-cursor advancement BEFORE `ClearDirty()` so a `Clear` throw doesn't cost us the cursor update (currently `Clear` runs first because it preserves cached_* fields, but the loop runs after — swap them and remove the preservation logic from `Clear`).

---

## Medium

### M1. `LoadEventVersions` runs inside the same `LoadByAccountId` work as a dozen other SELECTs, all without `FOR SHARE` — load-vs-commit race exists for VerifyCredentials reload
**Files:** `Server/Account/src/AccountRepository.hpp:339-401`, `Server/Account/src/AccountServer.hpp:235-261`
**Status:** NEW — exposed by H1 stripe-lock-on-VerifyCredentials fix

VerifyCredentials now takes the stripe lock before reloading (H1 fix at `AccountServer.hpp:235`). Good. But `LoadById` opens its own `pqxx::work` separate from any in-flight commit on a different connection — and stripe locks are PROCESS-LOCAL. If a future internal RPC ever runs without the stripe lock (or runs on a different instance), a `LoadEventVersions` MAX query could return a stale value, the loaded account gets `cached_wallet_version = N-1`, and its first commit attempts `version = N` colliding with the in-flight commit's `version = N`.

Single-instance stripe locks make this currently safe. Worth documenting and adding a test that exercises load-during-commit.

### M2. Post-commit `account_.ClearDirty()` resets `pity_slots` AFTER cursor advancement reads `dirty` — fragile ordering
**File:** `Server/Account/src/AccountTransaction.hpp:252-261`
**Status:** NEW — ordering hazard

The code:
```cpp
account_.ClearDirty();                  // resets every dirty field; preserves cached_*
auto& dirty = account_.MutableDirty();  // reference to the just-cleared struct
for (const auto& ev : events_) {
    switch (ev.aggregate_kind) {
        case events::AggregateKind::Wallet: dirty.cached_wallet_version = ev.version; break;
        // ...
    }
}
```

Currently safe because `DirtyState::Clear()` (`AccountDirty.hpp:52-64`) explicitly preserves all four `cached_*_version` fields. If anyone refactors `Clear()` to a plain `*this = DirtyState{};` (the obvious simplification a future reader will try) the cursor preservation breaks silently. The comment at `AccountDirty.hpp:53-54` and `AccountTransaction.hpp:249-251` warns about this but the invariant is enforced at runtime by ordering alone.

**Fix:** Reverse the order — advance cursors first, THEN ClearDirty. Then `Clear()` can be a plain reset and the cursor preservation becomes an explicit handoff rather than a hidden contract. (The lambda inside the loop would need to mutate `cached_*` BEFORE Clear blows it away, but you'd compute them into locals first then write after Clear — equivalent code with less coupling.)

### M3. `LoadEventVersions` doesn't filter `deleted_at` — soft-deleted accounts with retained events still hydrate cursors
**Files:** `Server/Account/src/AccountRepository.hpp:611-641`
**Status:** NEW (low-impact)

`LoadByAccountId` filters by `accounts.deleted_at IS NULL` so a soft-deleted account is correctly not returned. But the events table doesn't have a deleted-at filter and `LoadEventVersions` queries it directly. If the events partitions retain history past account soft-deletion (which they do — CASCADE only fires on hard delete), a future re-registration with the same account_id (BIGSERIAL won't reuse) is impossible, so this is theoretical. Worth a comment confirming the assumption.

### M4. `MarkStaleForReload()` does not invalidate the idempotency cache row that was just written
**Files:** `Server/Account/src/AccountTransaction.hpp:215-230,266` (cache write inside commit), `Server/Account/src/AccountRepository.hpp:289-320` (FindIdempotency)
**Status:** NEW — observation

When post-commit bookkeeping fails and the account is marked stale, the idempotency cache row HAS been committed atomically with the events. So the next client retry with the same scopedKey gets a cache hit → returns the cached response → no fresh execution. This is the CORRECT idempotent retry semantics (the action did succeed durably; the cached response is accurate). Just worth flagging that the "marked stale → reload" path does not extend to the cache.

### M5. Snapshot fields not captured: `m_stale`, identity fields. Identity correctly excluded; `m_stale` semantics is correct but commented inconsistently
**File:** `Server/Account/src/Account.hpp:64-66,73-98`
**Status:** NEW — doc inconsistency only

Comment at lines 64-66 says `m_stale` is NOT captured; the Snapshot struct confirms (no field). But the Rollback path at `AccountTransaction.hpp:319` sets `MarkStaleForReload()` AFTER `RestoreFrom`. If the snapshot WERE to preserve `m_stale=true` from a prior failed handler, it would survive across `RestoreFrom` (since `RestoreFrom` doesn't touch m_stale). That's consistent. Just worth a sentence in the comment block that explains why m_stale is sticky and the Restore can't clear it.

---

## Low / Observation

### L1. `EventStore::Append` (standalone, non-tx variant) is now only used by tests
**File:** `Server/Account/src/db/EventStore.hpp:73-78`
**Status:** OBSERVATION

After C6, the live commit path goes through `AppendInTx`. `Append` is a wrapper that acquires its own connection. Grep shows `Append` is also called by `AppendIdempotent` (line 92) and by tests. Worth a comment that Append is for the test surface + idempotent outbox replay; live RPC commits should always use AppendInTx.

### L2. `AppendIdempotent` acquires a separate connection for the dedup-check query (lines 95-99) — two lease round-trips on every idempotent append
**File:** `Server/Account/src/db/EventStore.hpp:90-101`
**Status:** OBSERVATION

The Append → conflict → second-lease lookup pattern doubles the pool cost of an idempotent-replay case. Single-call path is fine; replay path is rare. Worth a TODO to fold the dedup check into the original tx on conflict.

### L3. `LoadEventVersions` switch silently allows future aggregate addition to load cursor=0 — the `LOG_DATA_ERROR` lands but `cached_*_version` for the new aggregate remains 0
**File:** `Server/Account/src/AccountRepository.hpp:626-639`
**Status:** VERIFIED — M13 fix log is correct, but it's runtime-only

M13 added the loud error log. The comment is right that this is "impossible to miss after a schema bump that forgets to update this switch." But the FIRST commit on the new aggregate will still attempt `version=1`, which `AppendInTx`'s pre-check catches (returns MAX = N, ev.version=1 ≤ N → ConcurrencyConflict). So safety holds. Just noting: the log fires at LOAD time (well before any handler runs), so an operator sees the error in the boot/reload logs even before a user-facing failure. Good.

### L4. `IDEMPOTENCY_DEFAULT_TTL_SECONDS = 24 * 60 * 60` matches the session TTL but exceeds reasonable retry windows for cheap RPCs
**File:** `Server/Account/src/AccountTransaction.hpp:107`
**Status:** OBSERVATION

24h is generous and the comment justifies it for mobile-suspend cases. But for cheap RPCs (GetState, SetParty) where the client doesn't even send an idempotency key, none of this matters. For expensive RPCs (pull, claim) it's correct. No fix needed; just noting the constant is "long" by historical standards.

---

## Verified Closed

1. **C6** — `AccountTransaction::Commit` now routes through `EventStore::AppendInTx` (line 161-163); the in-tx `SELECT MAX` pre-check fires inside the atomic commit group. Multi-event-same-aggregate chains correctly because in-tx SELECTs see prior INSERTs in the same `pqxx::work` (read-your-writes). Single-instance race is closed; multi-instance is not (see H2 above).
2. **C7** — Snapshot capture + RestoreFrom are wired and called. Semantically broken (see C1) but mechanically the wiring is correct.
3. **M2** — Lazy lease/tx via `EnsureOpen()` + `State::Pending`. Empty transactions don't acquire a connection. `Commit()` short-circuits at line 136-142 if nothing buffered + nothing dirty.
4. **M19** — Post-commit catches at lines 262-271 swallow and log; client sees success. Recovery via `MarkStaleForReload`. Semantically correct (DB is committed; client retry of a succeeded action would double-execute).
5. **H8** — `AppendIdempotent` dedup query at line 97-99 includes `aggregate_kind` in the WHERE.
6. **H9** — Event-level `idempotency_key` derives from client `scopedKey` in pull (`GachaHandlers:228`), multi_pull (`GachaHandlers:562`), admin_grant (`AccountHandlers:386`), level_/ascend_ (`ProgressionHandlers:504`). QuestHandlers `claim:<questId>:<accountId>` already correct.
7. **H10** — `EffectDispatcher::wallet_version_cursor_` is a per-instance local seeded once at construction (line 34) and pre-incremented (line 130). Two GrantCurrencyEffect calls produce distinct versions. (Caveat in H1 above — coexistence with handler events is still broken.)
8. **M13** — `LoadEventVersions` logs ERROR on unknown aggregate_kind (`AccountRepository.hpp:634-639`).
9. **DirtyState::Clear cursor preservation** — verified at `AccountDirty.hpp:52-64`; the four `cached_*_version` fields are saved and restored across `*this = DirtyState{}`.
10. **Reducer purity** — `Server/Account/src/reducers/*.hpp` are clean; only place `chrono::now` appears is the `IClock` injection interface, never a free call. Snapshot/restore introduces no new non-determinism.

---

## Assurance — new guarantees from this remediation pass

1. **Event version monotonicity is now defended at the SQL layer for single-instance.** The in-tx `SELECT MAX(version)` plus stripe-lock guarantees no two commits on the same node, same aggregate, same account write colliding versions. The UNIQUE constraint's `created_at` weakness is no longer load-bearing for the single-instance case.
2. **Multi-event-same-aggregate commits chain naturally.** QuestHandlers's per-currency wallet event stream (4 wallet events from one claim) appends 4 sequential versions without the handler needing to read intermediate MAX state — the in-tx SELECT picks up prior INSERTs.
3. **Empty transactions cost nothing.** `GetQuestState`, `ReportQuestProgress`, `SetParty` (when nothing actually changed) no longer take a pool lease. Read-heavy workloads scale on the pool without speculative leasing.
4. **Post-commit failure → DB-truth recovery.** The catch block at `AccountTransaction.hpp:262-271` + the C1 stale-flag eviction means a partial in-memory-bookkeeping failure can never cause the next handler to read divergent state. DB is the recovery source of truth.
5. **Idempotent retries with a valid client key always return the byte-for-byte cached response.** H2 (cache-row refresh via `DO UPDATE`) closes the long-tail retry hole; H9 makes the events table's UNIQUE constraint actually do something for client-keyed flows; H10 prevents the dispatcher from emitting colliding versions in its own scope.
6. **Aggregate-kind drift is loud.** M13 logs an ERROR on unknown aggregate at load time, before any handler runs. A schema addition that forgets to update `LoadEventVersions` fails fast at the boot/reload of any affected account.
7. **Unknown-aggregate cursor=0 is caught by AppendInTx anyway.** Even without M13, a fresh aggregate's first commit (version=1) would trip AppendInTx's pre-check on the second commit attempt because MAX would now return 1 — so the M13 log is an early warning, not the sole guard.
