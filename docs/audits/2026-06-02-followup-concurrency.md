# Follow-up Audit: Concurrency + Lifecycle
**Date:** 2026-06-02
**Auditor:** Auditor #3 (follow-up to 2026-06-02-server-persistence-audit)
**Scope:** Verify the two-lock protocol (stripe → map) is honored on every path that mutates `m_accounts` or `Account` state after the C1/C2/C7/M1/H1/H5/L1/L8 fixes. Audit the Memento snapshot/restore for completeness, lifetime hazards, and test coverage. Trace OnStopped / CleanupIdleAccounts / VerifyCredentials lifecycle corners for in-flight races. PoolExhausted unwind path traced end-to-end.

## Summary

The two-lock protocol holds on every path I traced — `GetLockedAccount`, `CleanupIdleAccounts`, `OnStopped`, and `VerifyCredentials` all acquire stripe → map in that order, and `m_stale` is touched exclusively under the stripe lock so the bool read/write needs no atomicity. The PoolExhausted unwind through `EnsureOpen → ~AccountTransaction → Rollback` is clean: `lease_` stays default, the destructor is a no-op on the empty Lease, and the stale-flag fallback fires. **However, three concurrency/lifecycle issues remain.** A pre-cache-lock data-freshness race in `VerifyCredentials` allows a stale `AccountData` (read before the stripe lock) to be inserted into the cache if no other handler has populated it yet. A failure of `LoadAccountFromData` from inside `VerifyCredentials`'s `m_accounts[id] = ...` leaves a permanent `nullptr` entry in the map (slow memory leak). And the Memento snapshot is captured for **every** transaction including read-mostly handlers like `GetQuestState`, so the M2 lazy-connection win is partially undermined by an eager deep-copy of every collection / map / world-flag store the player owns. Test coverage for the snapshot restore is also far thinner than C7's promise: pity, RNG, guarantee, collection, party, and equipment maps are NOT exercised by `[c7]`, even though those are the ones whose interdependence with `banner.Pull()` was the design rationale for picking Memento over UoW.

## Critical

_None._

## High

### H1. `VerifyCredentials` reads `AccountData` BEFORE acquiring the stripe lock — stale insertion race
**Files:** `Server/Account/src/AccountServer.hpp:211-261`
**Status:** NEW — exposed by the H1 fix that added the stripe lock without reordering the load

The H1 fix moved the stripe-lock acquisition past PBKDF2 verify (line 235) so concurrent legit handlers don't wait on hash derivation. Correct intent. But `LoadByUsername` at line 211 runs BEFORE any lock — it's a fresh DB read that returns an `AccountData` snapshot for the matched username. Between that read and the stripe-lock acquisition, a concurrent handler can run an event-sourced commit on this player AND idle-evict the cache:

```text
T0: VerifyCredentials(alice)  LoadByUsername → AccountData{credits=500, version=7}  (no lock)
T1: GetLockedAccount(alice)   stripe + map lock → wallet -100 → Commit → version=8.  Releases.
T2: CleanupIdleAccounts tick  evicts alice from m_accounts.
T3: VerifyCredentials(alice)  takes stripe + map lock → m_accounts.find == end →
                              m_accounts[alice] = LoadAccountFromData(stale AccountData credits=500, cursor=7)
```

Now the cache has an Account whose wallet says 500 (truth: 400) and whose `cached_wallet_version=7` (truth: 8). The next handler runs `wallet.AddCredits(-50)` → builds an event with `version=cached_+1 = 8` → `AppendInTx`'s SELECT MAX returns 8 from the existing event → `ConcurrencyConflict` throws → Rollback → stale → reload. **The system converges**, but only via the C7/C1 stale-flag fallback. In the meantime the response that VerifyCredentials returns to Auth (line 263-267) is at least benign — it only echoes `playerId` and `playerName`, not balances.

The hazard widens if a future VerifyCredentials response carries any balance/state field (and the comment block at line 225-234 suggests the cache-insertion has wider semantic intent than the response alone).

**Fix:** Re-`LoadByUsername` (or `LoadById(accountData->id)`) AFTER taking the stripe lock, before the `m_accounts[id] = LoadAccountFromData(...)` insertion. Cheap — single SELECT chain. Alternative: split the API so the cache-insert path is opt-in and only fires when we know nobody else can populate the cache concurrently (which we can't know from this RPC's vantage point).

### H2. Memento snapshot is captured for every transaction including read-only ones — undermines M2's pool win with a deep-copy
**Files:** `Server/Account/src/AccountTransaction.hpp:49`, `Server/Account/src/Account.hpp:100-126` (CaptureSnapshot), `Server/Account/src/QuestHandlers.hpp:102` (GetQuestState empty txn)
**Status:** NEW — perf regression introduced by C7

M2 made the DB connection lease lazy so read-mostly handlers don't speculatively lease. But C7 added an eager deep-copy of every mutable Account field into `preTxSnapshot_` in `AccountTransaction`'s ctor, BEFORE the lazy `EnsureOpen()` check:

```cpp
AccountTransaction(...)
    : pool_(pool), store_(store), account_(account)
    , preTxSnapshot_(account.CaptureSnapshot())   // <-- runs every Begin(), unconditionally
{}
```

`CaptureSnapshot()` copies `m_wallet`, `m_pityBySlot` (map<string, PityTracker>), `m_guaranteeBySlot`, `m_rawPity`, `m_stats`, `m_collection.GetState()` (CollectionState with characters / weapons / gear maps — potentially hundreds of entries for an endgame account), `m_questStates`, `m_worldFlags`, `m_materials`, `m_party`, `m_weaponEquipment`, `m_gearEquipment`, `m_passwordHash`, plus all scalars. For an endgame player this is a multi-KB copy on a hot path.

`GetQuestState` opens a txn for the sole purpose of flushing whatever dirty state the load-time `TickQuests::Apply` left behind (`QuestHandlers.hpp:98-103`). It's read-mostly. Yet every call pays the snapshot cost. Same for `SetParty` when the party didn't actually change (M2 short-circuits the Commit but the ctor already copied).

The eagerness is also semantically unnecessary: the snapshot is only consulted on Rollback, which requires a buffered mutation OR a Commit failure path. Pending → Committed-without-flush goes straight through without ever needing the snapshot.

**Fix:** Make snapshot capture lazy in the same way the lease is lazy — defer it to the first mutation (`AppendEvent`, etc.) or to the first dirty-bit transition. Either:
1. Move `preTxSnapshot_ = account.CaptureSnapshot()` into a `EnsureSnapshot()` helper called from `AppendEvent`/`RecordAudit`/`StoreIdempotency` and from `Commit` only if `hasDirty`. Skip it for the no-op Pending→Committed path.
2. Or, capture into a `std::optional<Snapshot>` and check `if (preTxSnapshot_) account_.RestoreFrom(...)` in Rollback. The optional check is one branch; the deep-copy is several allocations and many node moves.

Either fix preserves the C7 correctness contract while restoring M2's read-path savings.

## Medium

### M1. `VerifyCredentials` strands a `nullptr` entry in `m_accounts` if `LoadAccountFromData` throws
**Files:** `Server/Account/src/AccountServer.hpp:256-261`
**Status:** NEW — exposed by the H1 fix's `operator[]` insert pattern

```cpp
std::lock_guard<std::mutex> mapLock(m_accountsMapMutex);
m_accountLastAccess[accountData->id] = std::chrono::steady_clock::now();
if (m_accounts.find(accountData->id) == m_accounts.end())
    m_accounts[accountData->id] = LoadAccountFromData(*accountData);
```

`m_accounts[id] = ...` default-constructs a `unique_ptr<Account>(nullptr)` at the key, THEN evaluates the RHS, THEN move-assigns. If `LoadAccountFromData` throws partway (e.g., `TickQuests::Apply` allocates and OOMs, a setter throws, etc.) during construction, the LHS slot is already inserted with a nullptr value. Exception unwinds out of VerifyCredentials, map lock released, nullptr entry persists.

Downstream:
- `GetLockedAccount` line 450: `if (it != m_accounts.end() && it->second)` — null check prevents UAF, falls through to LoadById. Good.
- `CleanupIdleAccounts` line 414: same null check, but it never erases the null entry — the `if (it != m_accounts.end() && it->second)` is false, the entire eviction block is skipped, AND `m_accountsPendingCleanup` doesn't get cleaned because we returned before reaching the erase. So a null entry sits forever, with `m_accountLastAccess[id]` and `m_accountsPendingCleanup` re-marked each cleanup tick. Slow memory leak + unbounded set growth on the pendingCleanup side.
- `OnStopped` line 152: `if (account)` null check, skips the save and the emplace_back. Good for shutdown.

Functional impact today is small (LoadAccountFromData isn't documented to throw), but the trap is set. The same pattern is NOT used in `GetLockedAccount` at line 491 — there it does `m_accounts.emplace(playerId, std::move(account))` AFTER the load succeeds, so a throw would never insert.

**Fix:** Mirror GetLockedAccount's pattern — load first, then emplace under the lock:

```cpp
auto loaded = LoadAccountFromData(*accountData);   // throws BEFORE map mutation
{
    std::lock_guard<std::mutex> mapLock(m_accountsMapMutex);
    m_accountLastAccess[accountData->id] = std::chrono::steady_clock::now();
    if (m_accounts.find(accountData->id) == m_accounts.end())
        m_accounts.emplace(accountData->id, std::move(loaded));
}
```

### M2. Snapshot field coverage is correct today but has no compile-time / runtime safety against drift
**Files:** `Server/Account/src/Account.hpp:73-152`
**Status:** NEW — maintainability hazard

`Snapshot` covers every mutable field on `Account` (verified field-by-field against the member list at lines 472-532). But there's no `static_assert(sizeof(Snapshot) == ...)` and no `[c7]`-style test that exhaustively round-trips every field. The next engineer who adds e.g. a `std::vector<UnlockedTitle> m_titles` to `Account` will:
1. Add the field below the existing members.
2. Add a setter that flips `m_dirty.accounts_row`.
3. Run the handler tests; they pass.
4. Ship the regression: any Rollback now silently DOES NOT restore `m_titles`. The C7 promise is silently broken for the new field, and the stale-flag fallback covers it only on next access (which is fine for correctness but defeats the in-handler-readability contract C7 was trying to provide).

The current `[c7]` test only covers wallet/storyLevel/storyXp/loginStreak/materials/worldFlags — leaving 14 other Snapshot fields unverified.

**Fix (pick one):**
1. Expand `[c7]` to also mutate + verify rollback of: RNG state (advance, then assert state matches), pity (record a pull, then assert tracker state), guarantee (set guaranteed=true, then assert), collection (AddCharacter, then assert HasCharacter==false), party, weaponEquipment, gearEquipment, stats, name, passwordHash, createdAt, difficultyTier, dirty bits.
2. Add a reflection-helper that walks both `Account` and `Snapshot` field-by-field and asserts a deep-equality after `CaptureSnapshot/RestoreFrom`. (Heavy for solo-dev pre-launch but eliminates the drift class entirely.)
3. Minimal: add a doc-comment on `Account` private members "if you add a new mutable field, also add it to `Snapshot` + `[c7]` test" and grep-bait — call it out in CLAUDE.md too.

### M3. `CleanupIdleAccounts` ignores `Save()` return value (parallel of L8 OnStopped fix, missed)
**Files:** `Server/Account/src/AccountServer.hpp:426-430`
**Status:** NEW — symmetry gap. Already flagged in `followup-error-handling.md` M1; flagging here from the lifecycle angle.

```cpp
for (auto& [playerId, account] : accountsToSave)
{
    if (account) m_repository.Save(*account);   // <-- bool ignored
    LOG_DATA_DEBUG("Unloaded idle account: {}", playerId);
}
```

L8 added the check-and-log to OnStopped. CleanupIdleAccounts was the symmetry partner and missed it. Concurrency relevance: a stale-flagged account (per C2 fix) returns `false` from Save and is silently dropped during idle-evict. The account state IS still authoritative in DB (the Commit failure that set stale also rolled back the DB tx), so semantic loss is zero. But you lose the diagnostic signal that "an account got idle-evicted in stale state" — which is the only outward indicator that a Commit recently threw on this player and recovery is in progress.

**Fix:** Mirror OnStopped's check-and-log:
```cpp
if (account)
{
    bool ok = m_repository.Save(*account);
    if (!ok)
        LOG_DATA_WARN("CleanupIdleAccounts: Save() returned false (stale or transient) for {}", playerId);
}
```

### M4. Lock-ordering documentation only lives in `StripedMutex.hpp` and one comment in `AccountServer.hpp`
**Files:** `Server/Common/src/StripedMutex.hpp:14-19`, `Server/Account/src/AccountServer.hpp:591`
**Status:** NEW — maintainability

The lock-ordering protocol (stripe BEFORE map) is enforced ad-hoc on six sites:
- `GetLockedAccount` (442 stripe, 446 map) ✓
- `CleanupIdleAccounts` phase 2 (408 stripe, 411 map) ✓
- `VerifyCredentials` C5 rehash branch (235 stripe, 249 map) ✓
- `VerifyCredentials` cache-insert branch (235 stripe, 257 map) ✓
- `OnStopped` phase 2 (163 stripe, no map needed — local vector) ✓
- `OnStopped` phase 1 takes ONLY the map lock (148) — no stripe lock needed because phase 1 only transfers ownership; in-flight handlers holding stripes are then blocked by phase 2's per-player stripe acquisition. Correct, but the asymmetry is non-obvious.

There's no compile-time enforcement, no startup assert, no lock-ordering linter. The single doc-comment at `AccountServer.hpp:591` ("always acquire m_playerLocks stripe BEFORE m_accountsMapMutex") is the only authoritative statement. A future engineer adding a 7th lock site (e.g., a `BatchExportAccounts` admin method) can easily flip the order and introduce a deadlock that only manifests under load.

**Fix:** Either (a) move the lock-ordering doc into a dedicated section in CLAUDE.md so it's discoverable from above the file, or (b) wrap the two locks in a single `LockedAccountCache` helper class that hands out RAII guards in only-the-correct order (turning the ordering into a type-system invariant). (b) is the durable fix; (a) is the cheap one.

## Low / Observation

### L1. `m_stale` is a plain `bool` and is read/written from multiple threads, but every access is under the stripe lock — no atomic needed
**Files:** `Server/Account/src/Account.hpp:47-49,532`
**Status:** VERIFIED — flagging only because it's the kind of thing a future skim might "fix" with a needless `std::atomic<bool>`.

Audit of every read site:
- `AccountServer::GetLockedAccount:458` — stripe lock + map lock held.
- `AccountRepository::Save:142` — called from CleanupIdleAccounts (stripe held), OnStopped phase 2 (stripe held).

Audit of every write site:
- `AccountTransaction::Rollback:319` — destructor unwinds on the handler stack which owns the stripe lock.
- `AccountTransaction::Commit:266,270` — same stack.
- `AccountServer::GetLockedAccount:485` — `ClearStale()` on a freshly-loaded, not-yet-cached Account (no other thread can see it).

So m_stale is mutex-protected by the stripe lock everywhere. No torn read, no missed write. A future change that switches to `std::atomic<bool>` would relax that to a happens-before guarantee that the current invariant ALREADY has via mutex acquire/release semantics, with no correctness gain.

**Action:** Add a one-line comment at `Account.hpp:532` to head off the future "should this be atomic?" question with the stripe-lock invariant documented.

### L2. `GetLockedAccount` reload path: `ClearStale()` on freshly-loaded account is redundant
**Files:** `Server/Account/src/AccountServer.hpp:485`
**Status:** OBSERVATION

`LoadAccountFromData` returns a `std::make_unique<Account>(...)` — a freshly default-constructed Account whose `m_stale = false` from the default member initializer. The explicit `account->ClearStale()` at line 485 is a no-op. Code is correct but the comment ("freshly hydrated; the rollback divergence is gone") implies a state transition that doesn't actually exist.

**Action:** Either drop the call OR add a comment clarifying it's defense against a future change that could make `LoadAccountFromData` preserve stale state from elsewhere (e.g., caching `AccountData` and reusing it). Today, ineffective.

### L3. `VerifyCredentials`' C5 rehash path mutates a possibly-stale cached Account without checking
**Files:** `Server/Account/src/AccountServer.hpp:249-253`
**Status:** OBSERVATION

```cpp
std::lock_guard<std::mutex> mapLock(m_accountsMapMutex);
auto it = m_accounts.find(accountData->id);
if (it != m_accounts.end() && it->second)
    it->second->SetPasswordHash(newHash);
```

If the cached `it->second` is stale (from a prior Rollback), this calls `SetPasswordHash` on a stale Account, flipping its `m_dirty.accounts_row` bit. The dirty bit is meaningless because:
- C2's `Save()` early-returns on stale, so no flush ever fires.
- The next `GetLockedAccount` detects stale, erases the cached Account, and reloads from DB. The DB has the new hash (UpdatePasswordHash already wrote it at line 246). Stale Account's dirty bit + in-memory hash are both discarded.

So functionally correct. But the code is one read+if away from being self-documenting about the stale case:

```cpp
if (it != m_accounts.end() && it->second && !it->second->IsStale())
    it->second->SetPasswordHash(newHash);
```

**Action:** Add the `!IsStale()` guard, mostly as a "make the invariant visible at the call site" change.

### L4. OnStopped Phase 1 + concurrent ServiceEndpoint RPCs: in-flight handler can repopulate `m_accounts` AFTER the clear
**Files:** `Server/Account/src/AccountServer.hpp:135-182`, `Server/Common/src/ServiceEndpoint.hpp:107-111`
**Status:** OBSERVATION — bounded by the 10s ServiceEndpoint drain timeout

`TcpServerBase::Stop` joins all client threads before calling `OnStopped` (line 162 of TcpServerBase.hpp). But internal RPC threads (m_internalEndpoint) are drained with a **10-second timeout** (ServiceEndpoint.hpp:108). If a long-running internal RPC (e.g., a slow `VerifyCredentials` chain) hasn't finished within 10s, `Stop()` returns and `OnStopped` runs WHILE the RPC is still in flight.

Concrete race:
1. Long internal RPC (VerifyCredentials) holds stripe lock + map lock at line 257, about to call `m_accounts[id] = LoadAccountFromData(...)`.
2. OnStopped Phase 1 waits on the map mutex.
3. The RPC finishes the insert, releases both locks.
4. OnStopped Phase 1 takes map lock, clears m_accounts (including the just-inserted one). Good.
5. ...but if step 4 had run BEFORE step 3 (i.e., another RPC between Phase 1 and Phase 2 inserts), the new Account survives Phase 1's clear, never gets Save'd, never gets destroyed because m_accounts is now empty and the new entry was added back AFTER the clear.

Actually re-tracing: Phase 1 takes map lock, drains m_accounts into a local vector and CLEARs both maps and the pending-cleanup set. AFTER Phase 1 releases the map lock, an internal RPC that races in here can re-insert into m_accounts. Then Phase 2 takes stripe locks and saves the drained accounts — but the newly-inserted entry is NOT in the drained vector. It will be leaked when `AccountServer` is destructed (the `unordered_map<...,unique_ptr<Account>>` member destructor cleans up the memory, but nothing flushed the dirty state to disk).

**Bound:** The ServiceEndpoint drain timeout is 10s, the cleanup pattern is fragile but not actively broken under normal operation. The only state-loss vector is "internal RPC that runs past the 10s drain" — i.e., a VerifyCredentials whose DB Load is hanging.

**Fix:** Either (a) tighten the ServiceEndpoint drain so OnStopped doesn't start while internal RPCs are alive, or (b) loop OnStopped's two-phase drain until phase 1 picks up zero accounts (drains the "leaked-by-rescheduling" case). (a) is the architecturally correct fix.

---

## Verified Closed

1. **C1 (stale flag honored in GetLockedAccount):** `AccountServer.hpp:458-462`. Stripe + map lock both held when checking; erase happens under map lock. Reload path emplaces under map lock after stripe-protected load. No concurrent insert possible for the same playerId (stripe lock serializes), so the erase→load→emplace window is single-threaded for that key.
2. **C2 (Save early-returns on stale):** `AccountRepository.hpp:142-145`. Both call sites (CleanupIdleAccounts, OnStopped phase 2) hold the stripe lock when calling.
3. **C7 (Memento mechanics):** Snapshot struct mirrors every mutable Account field; CaptureSnapshot / RestoreFrom are correctly wired. Mechanism is correct in isolation. Two known caveats: snapshot is captured too late relative to handler mutations (logged in `followup-event-sourcing.md` C1 and `followup-error-handling.md` C1 — out of this audit's scope), and snapshot capture is eagerly performed even on read-mostly txns (see H2 above).
4. **M1 (OnStopped two-phase pattern):** `AccountServer.hpp:135-182`. Phase 1 transfers ownership under map lock only; phase 2 takes per-player stripe locks before each Save. No torn writes against in-flight handlers (modulo L4 above, which is a ServiceEndpoint-drain issue, not an OnStopped issue).
5. **H1 (VerifyCredentials stripe lock):** `AccountServer.hpp:235`. Stripe → map ordering is consistent with `GetLockedAccount`. The lock is taken after PBKDF2 to avoid serializing the expensive hash on the stripe. Lock-ordering correct; data-freshness gap noted in H1 above.
6. **H5 (Pool exhaustion bounded):** `ConnectionPool.hpp:75-84`. `cv_.wait_for(acquire_timeout_)` throws `PoolExhausted` on timeout. Unwind through `EnsureOpen` → `~AccountTransaction` → `Rollback` is clean: `lease_` is in default state (acquire() is strongly exception-safe — the throw is from `wait_for`'s predicate-failure path, BEFORE `std::move(free_.front())`), so `~Lease` is a no-op (`if (pool_ && conn_)` is false). `tx_` is null so `Rollback` skips the abort branch. State machine goes Pending → RolledBack, RestoreFrom runs, MarkStaleForReload fires. End-to-end traced, no double-throw, no leak.
7. **L1 (tx_->abort logs):** `AccountTransaction.hpp:288-294`. Split between `std::exception` (with `e.what()`) and `...`.
8. **L8 (OnStopped logs save failures):** `AccountServer.hpp:169-174`. (CleanupIdleAccounts's parallel was missed — see M3.)

---

## Assurance — new guarantees from the remediation pass

1. **Single-player serialization is now end-to-end.** Every code path that mutates the cached Account or its `m_stale` bit goes through the stripe lock. Two RPCs for the same player on different worker threads cannot interleave. This is the contract C7 + C1 + C2 were collectively designed to enforce, and it holds.

2. **Stale-flag → DB reload recovery converges from every failure path.** Commit-throw, EnsureOpen-throw (PoolExhausted), post-commit-bookkeeping-throw, and explicit Rollback all funnel into `MarkStaleForReload`. The next `GetLockedAccount` for that player drops the cached Account and reloads from DB. No path leaves the cache permanently divergent from DB truth.

3. **The shutdown drain pattern is non-blocking on in-flight handlers up to ServiceEndpoint's 10s drain.** OnStopped's two-phase pattern means a TCP-client handler mid-RPC keeps its stripe lock until release; OnStopped's phase 2 acquires the lock in turn and serializes against it. No torn half-write of an in-progress player.

4. **Pool exhaustion no longer cascades into stripe-lock starvation.** H5's bounded acquire means a slow / hung DB blocks one handler at a time for at most 5 seconds; the handler unwinds with `PoolExhausted`, releases the stripe lock, and other RPCs for that player can proceed (they'll see stale and reload from DB once it recovers).

5. **The Lease move-assignment + default-empty pattern composes correctly with PoolExhausted.** `Lease()` defaults to empty; `acquire()` is strongly exception-safe (throws before any state mutation); a throw out of `EnsureOpen` leaves `lease_` empty and `~Lease` is a no-op. No connection leak on the timeout path.
