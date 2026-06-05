# Follow-up Audit v3: Concurrency + Lifecycle
**Date:** 2026-06-03
**Auditor:** Auditor v3 (follow-up to 2026-06-02-server-persistence-audit + v2 + first follow-up-concurrency)
**Scope:** AccountCache (newly extracted M-V2-3 step 2), InternalRpcHandlers (newly extracted M-V2-3 step 3), AccountServer (residual), AccountTransaction, StripedMutex, SessionCache (M-V2-9), SessionManager (H-V2-8), ServiceEndpoint (thread-per-conn lifecycle), LruCache + RateLimiter (M-V2-10).

## Verdict

The M-V2-3 extraction is **mechanically clean** — the two-lock protocol (stripe → map) survives every relocation, and the v2 H/M concurrency items (H-V2-3 re-read, M-V2-1 save-failure log, M-V2-9 NoteAuthLost/NoteAuthRecovered split) are correctly implemented at the new home. One **CRITICAL latent regression** remains in C7-A's "Begin() must precede mutation" contract — two of the five event-sourced handlers still call `account.GetPity(...)` / `account.GetGuarantee(...)` BEFORE `Begin()`, and both accessors are mutating (lazy-insert into `m_pityBySlot` / `m_guaranteeBySlot` AND set `m_dirty.pity_slots`). The system stays safe today only because of C1's stale-flag fallback, identical to the v2 C7-A finding the v2 fix was supposed to close. Three **HIGH** items: the new InternalRpcHandlers escape-hatch methods carry no runtime enforcement of the "caller must hold LockFor" contract; a previously-flagged detached-thread / 10s-drain hazard in `ServiceEndpoint` is now substantially wider because `m_internalEndpoint` (in `TcpServerBase`) outlives `m_cache` (in `AccountServer`) in non-`Stop()` destruction paths; and `InsertIfAbsent` reintroduces the v2-M1 `operator[]`-can-strand-a-nullptr pattern. Six MEDIUM items and a handful of LOW/observations round out the surface.

---

## CRITICAL

### V3-C1. C7-A regression — `GetPity` / `GetGuarantee` lazy-insert + dirty-flag mutation runs BEFORE `Begin()` in pull handlers
**Files:** `Server/Account/src/GachaHandlers.hpp:117-118` (HandlePull), `:364-365` (HandleMultiPull); `Server/Account/src/Account.hpp:175-208` (GetPity/GetGuarantee)
**Flagged by:** v3 audit (matches the focus question #3 — confirmed)
**Status:** **NEW — undermines the v2 C7-A fix shipped in commit `9d2d221`.**

The v2 C7-A fix moved `m_ctx.repository->Begin(account)` above the explicit wallet / collection / pity mutations, AND the response was that the Memento snapshot captured in `AccountTransaction`'s ctor would reflect the genuine pre-mutation state. That fix held for the wallet / collection / RecordPull paths — but missed two mutating accessors that run earlier on the handler:

```cpp
// GachaHandlers.hpp:117-118 (HandlePull) and :364-365 (HandleMultiPull)
PityTracker& pity            = account.GetPity(slotId, *pityConfigPtr);
GuaranteeTracker& guarantee  = account.GetGuarantee(slotId);
// ... ~22 lines of read-only capture (pity5Before, walletPreTickets, …) ...
auto txn = m_ctx.repository->Begin(account);   // L140 / L403 — snapshot HERE
```

`Account::GetPity` (Account.hpp:175-191) on a first-pull-for-this-slot:
1. `m_pityBySlot[slotId] = std::move(t)` — inserts a new entry into the tracker map.
2. `m_dirty.pity_slots.insert(slotId)` — flips a dirty bit.

`Account::GetGuarantee` (Account.hpp:194-208) follows the same shape on first call: `m_guaranteeBySlot.emplace(...)` + `m_dirty.pity_slots.insert(slotId)`. Both maps AND the dirty set are captured by the snapshot (verified field-by-field against the X-macro at Account.hpp:96-116 — `pityBySlot`, `guaranteeBySlot`, and `dirty` are all present). So the snapshot captured at L140/L403 reflects the **post-lazy-insert** state, not the pre-handler state. On Rollback, `RestoreFrom` puts the post-insert map+dirty back, NOT the empty-map / clean-dirty pre-handler state.

**Why it's not catastrophic today.** Two converging guards:
1. The newly-inserted tracker is hydrated from `m_rawPity` (Account.hpp:181-186) — i.e. it's seeded with the same data that's in the `pity_state` table. So even if the rolled-back-and-restored state has a "fresh" tracker entry, that entry's contents match what a clean reload from DB would produce.
2. The `MarkStaleForReload` belt-and-suspenders in `AccountTransaction::Rollback` (line 357) forces the next `GetLockedAccount` to drop the cached Account entirely and reload. So the diverged state lives at most until the next cache lookup.

**But the C7-A contract is violated.** The whole point of v2 C7-A was that "by the time the throw unwinds the Account is back to its pre-transaction values" so the rolling-back handler could not return a "success" response built from speculative state. With `GetPity`/`GetGuarantee` running pre-`Begin()`, the snapshot is at-best-equivalent-to-the-DB but is NOT pre-handler. The same logical defect v2 C7-A was meant to close is reopened on this narrow surface.

**Fix.** Move `Begin()` BEFORE the GetPity/GetGuarantee accessors — i.e. above line 117 in HandlePull and above line 364 in HandleMultiPull. None of the values read between the current `Begin()` site and the accessors are mutating (BannerConfig::Instance(), GachaConfig::Instance(), CanAffordPullByType), so the move is a one-line shift with no observable side effect other than capturing the snapshot before the lazy-insert lands. The M2 lazy-lease optimization still kicks in (no DB connection until the first `AppendEvent`).

**Aside.** A more durable architectural fix would be to expose const `PeekPity` / `PeekGuarantee` accessors that do NOT lazy-insert, used for the read-only capture above; `GetPity`/`GetGuarantee` would then ONLY be called from inside the transaction window. This was effectively what v2 C7-A's third "architectural" fix-alternative suggested but the team chose the "move Begin() up" path. The same principle has to extend to these two accessors.

---

## HIGH

### V3-H1. Detached connection threads in `ServiceEndpoint` outlive AccountCache after 10s drain timeout — m_internalEndpoint destructs AFTER m_cache
**Files:** `Server/Common/src/ServiceEndpoint.hpp:78-84,104-111`, `Server/Common/src/TcpServerBase.hpp:48,252` (m_internalEndpoint lives in TcpServerBase base class), `Server/Account/src/AccountServer.hpp:323,330` (m_cache / m_internalRpcHandlers declared in derived class)
**Flagged by:** v3 audit (matches focus questions #5, #8 — confirmed)
**Status:** **NEW — exposed by M-V2-3's extraction. The pre-extraction inline form had the same hazard but the audit attention is now warranted.**

`ServiceEndpoint::HandleConnection` runs in a **detached** thread (line 84). `ServiceEndpoint::Stop()` waits up to **10 seconds** for `m_activeConnections == 0`, then returns regardless of whether detached threads are still alive. The detached threads continue to:
- Call into the captured Handler lambdas registered on `m_methods` (e.g., the lambda forwarding to `m_internalRpcHandlers.HandleVerifyCredentials`).
- Through that lambda, dereference `this` of `m_internalRpcHandlers`, which holds `AccountCache& m_cache`.
- Through `m_cache`, take `LockFor(playerId)` on the StripedMutex array inside m_cache.

When `AccountServer` destructs:
- AccountServer member destruction (reverse declaration order): `m_internalRpcHandlers` → `m_cache` → ... .
- `m_internalRpcHandlers` destructs FIRST (fine — it's just refs).
- Then `m_cache` destructs — the StripedMutex array and m_accounts unordered_map are gone.
- Detached connection threads that are still mid-handler now have:
  - A captured `this` pointer to a destroyed `InternalRpcHandlers`.
  - Through that ref to a destroyed `AccountCache`.
  - Their `std::unique_lock<std::mutex>` (returned by `LockFor`) is bound to a destroyed mutex.
- Eventually `TcpServerBase`'s `m_internalEndpoint` destructs (in `~TcpServerBase` after all `AccountServer` members), but by then the use-after-free has already fired.

This is a wider hazard than the v2-followup-concurrency L4 (which only covered the OnStopped Phase 1 / re-insert window). Worst case:
- A PBKDF2 verify in `HandleVerifyCredentials` is mid-200-iter compute when `Stop()` fires. `Stop()` waits 10s; if PBKDF2 (or a slow DB query) extends past that, `Stop()` returns, OnStopped runs (m_cache.SaveAllAndClear), AccountServer destructs.
- The detached thread finishes PBKDF2, calls `m_cache.LockFor(...)` on a now-destroyed mutex. Behavior is undefined.

The OnStopped Phase 2 stripe-lock pattern protects against concurrent **mutation** of an Account, but does NOTHING to wait for the detached thread to finish: SaveAllAndClear waits on the stripe lock by acquiring it, releasing, and then the cache destructs. The detached thread, if it acquires the stripe lock AFTER SaveAllAndClear releases but BEFORE m_cache destructs, gets a "valid" lock; if it acquires AFTER m_cache destructs, UB.

**Fix.**
1. **Preferred — make the drain bounded but uncapped on success.** Replace the 10s `wait_for` in `ServiceEndpoint::Stop()` with an unbounded `wait` (or a much larger watchdog window, e.g. 5 minutes, with a fatal log if exceeded). Set a flag that causes in-flight handlers to short-circuit (already partially in place via `m_running` check in the recv loop, but doesn't help once the handler is mid-call). The handler bodies need an interruption hook for long-running ops.
2. **Alternative — join, don't detach.** Track each connection's `std::thread` in a vector under a mutex; `Stop()` joins all of them. Forces the destructor path to wait. PBKDF2 still takes 200ms; the join window is bounded by the slowest in-flight handler, which is acceptable for graceful shutdown.
3. **Architectural — reorder destruction.** Move `ServiceEndpoint` out of `TcpServerBase` and into `AccountServer` BEFORE m_cache, so its destruction (which drains again) happens BEFORE m_cache's destruction. Even with the 10s timeout this narrows the UAF window because ServiceEndpoint isn't destroyed until after m_cache is gone in the current layout — reverse that and at least the ServiceEndpoint's `m_methods` map (and its bound lambdas) outlive m_cache.

### V3-H2. Escape-hatch contract on `AccountCache::InsertIfAbsent` / `UpdateCachedPasswordHash` is documented-only, no runtime enforcement
**Files:** `Server/Account/src/AccountCache.hpp:218-250`, `Server/Account/src/InternalRpcHandlers.hpp:108-130`
**Flagged by:** v3 audit (matches focus question #2 — confirmed)
**Status:** **NEW — exposed by M-V2-3 step 2's split between AccountCache (the lock-owner) and InternalRpcHandlers (the caller).**

The new API contract (AccountCache.hpp:222-225): "The caller must hold the lock returned by `LockFor` across any matching `Insert` / `Update` call." No assertion, no debug-mode `assert(m_playerLocks.GetLock(playerId).try_lock() == false)`-style check, no `[[nodiscard]]` on `LockFor` to make a silently-dropped return value visible. The contract is enforced by reviewer attention only.

Today the single call site (HandleVerifyCredentials) correctly takes `m_cache.LockFor(accountData->id)` at line 108 and holds it across the `UpdateCachedPasswordHash` / `InsertIfAbsent` calls at 125 / 129. So no live bug. But:
- A future engineer adding a fourth internal RPC (e.g., a `ResetPassword` handler) can call `UpdateCachedPasswordHash` without `LockFor` and silently break the stripe-ordering invariant. The `m_mapMutex` lock still keeps the map structurally sound, but a concurrent `GetLockedAccount` on the same player could see torn state (e.g., reload from DB with the OLD password hash, then `UpdateCachedPasswordHash` writes the NEW hash onto the just-reloaded Account — the in-memory state diverges from DB until the next save, and the dirty flag flush would write the NEW hash back, which is what we want — but the chain of events is fragile and depends on the dirty-flag-driven flush direction).
- The `LockFor` return is `std::unique_lock<std::mutex>` (movable, not `[[nodiscard]]`). A caller that writes `m_cache.LockFor(id);` (no binding) silently drops the lock at the end of the full-expression — a subtle bug that compiles cleanly.

**Fix.**
1. Mark `AccountCache::LockFor` with `[[nodiscard]]` so the unbound-lock pattern becomes a compile warning.
2. Add a debug-build runtime check inside `InsertIfAbsent` / `UpdateCachedPasswordHash` — e.g., `assert(!m_playerLocks.GetLock(playerId).try_lock())` followed by `unlock()` if it does succeed. `try_lock()` returning true means the caller did NOT hold it, which is the misuse pattern. This is debug-only because `try_lock` on a held mutex is undefined if held by the same thread (it's not a recursive mutex), so we'd need `std::recursive_mutex` or platform-specific thread-id checks. Simpler: a thread-local owner registry per stripe.
3. Type-system fix: change `InsertIfAbsent` / `UpdateCachedPasswordHash` to take `const std::unique_lock<std::mutex>&` as a first parameter (the lock token), making compilation impossible without first calling `LockFor`. The body asserts `lock.mutex() == &m_playerLocks.GetLock(playerId)`. This catches the entire misuse class at compile + first-debug-run time without recursive-mutex overhead.

### V3-H3. `InsertIfAbsent` uses `m_accounts[playerId] = std::move(account)` — strands a `nullptr` entry if hash-bucket allocation throws between the default-insert and the move-assign
**Files:** `Server/Account/src/AccountCache.hpp:238-239`
**Flagged by:** v3 audit (matches the v2-followup-concurrency M1 finding; regression — was supposed to be closed in M-V2-3 step 2 but the pattern persisted)
**Status:** **NEW (regression).**

The v2-followup-concurrency M1 finding flagged the exact same `m_accounts[id] = LoadAccountFromData(...)` pattern in the pre-extraction VerifyCredentials. The fix recommendation was: load first, then `emplace` under the lock. The M-V2-3 extraction relocated the code into `AccountCache::InsertIfAbsent` but kept the `operator[]` shape:

```cpp
void InsertIfAbsent(const std::string& playerId, std::unique_ptr<Account> account)
{
    std::lock_guard<std::mutex> mapLock(m_mapMutex);
    m_lastAccess[playerId] = std::chrono::steady_clock::now();
    if (m_accounts.find(playerId) == m_accounts.end())
        m_accounts[playerId] = std::move(account);   // <-- M1 pattern preserved
}
```

`unordered_map::operator[]` on a missing key:
1. Constructs a value-initialized mapped_type (here, `unique_ptr<Account>(nullptr)`) at the key — this can throw on bucket-allocation OOM.
2. Returns a reference.
3. The move-assign runs (noexcept for unique_ptr).

If step 1 throws partway (mid-bucket-allocation), the map may end up with the key inserted but without a valid value, OR not inserted at all — the standard doesn't guarantee strong exception safety on `operator[]`. The MSVC and libstdc++ implementations both pre-allocate the node, then insert — a throw during node allocation typically leaves the map untouched, but the standard doesn't require it.

The robust pattern is `try_emplace` (or the v2-followup recommendation: load outside lock, emplace under lock):
```cpp
m_accounts.try_emplace(playerId, std::move(account));
```

`try_emplace` is a single atomic insert — either the key exists (no-op, original value retained) or the key is inserted with the supplied value. No nullptr-stranded intermediate state.

The `find` ahead of the assign is also redundant once you switch to `try_emplace`. Today, with the `find != end` guard, the live insert path is "key not present → operator[] inserts default → move-assign overwrites". With `try_emplace` the same intent collapses to one call.

The functional impact today is identical to the v2 finding: a stranded nullptr causes a slow leak in `CleanupIdleAccounts` (the null-check skips eviction AND skips pending-cleanup erase, so the entry survives every sweep). `GetLockedAccount`'s null check at line 71 prevents a UAF on read but doesn't recover the stranded slot.

**Fix.** Switch to `try_emplace`. Cleanup the redundant `find` check.

---

## MEDIUM

### V3-M1. `UpdateCachedPasswordHash` doesn't check `IsStale()` — flips a dirty bit on a stale Account that's destined for reload
**Files:** `Server/Account/src/AccountCache.hpp:244-249`
**Status:** Cosmetic / defense-in-depth (v2-followup-concurrency L3 was the prior flag on the pre-extraction site; the issue carried over verbatim).

If the cached Account is stale (a prior Commit threw and the destructor called `MarkStaleForReload`), calling `it->second->SetPasswordHash(newHash)` flips `m_dirty.accounts_row = true` on a doomed Account. The next `GetLockedAccount` evicts it and reloads from DB — the DB has the new hash already (UpdatePasswordHash wrote it on the previous line in HandleVerifyCredentials), so the in-memory dirty bit and hash are both discarded. Functionally correct, but the invariant "stale accounts get reloaded, not mutated" is one read away from being self-documenting.

**Fix.** Add the `!IsStale()` guard:
```cpp
if (it != m_accounts.end() && it->second && !it->second->IsStale())
    it->second->SetPasswordHash(newHash);
```

### V3-M2. `LockedAccountRef` lifetime extends across the entire handler body — long handlers (PBKDF2-adjacent paths, multi-pull) hold the stripe for the full duration
**Files:** `Server/Account/src/HandlerContext.hpp` (LockedAccountRef), `Server/Account/src/GachaHandlers.hpp:394-403` (MultiPull which iterates 10× under lock)
**Status:** Pre-existing surface, magnified by M-V2-3's broader use of the cache.

The two-lock protocol (stripe → map) is correct, but `LockedAccountRef.stripeLock` is owned for the entire handler's body. A multi-pull does 10 iterations of pull logic, each touching collection / pity / wallet, plus a Commit at the end. A concurrent legitimate handler for a same-stripe-but-different-player (1/64 collision rate) gets blocked behind it. Combined with H-V2-3's "stripe lock held during PBKDF2 verify" (now fixed by stripe-lock-AFTER-PBKDF2), the false-contention surface is real for any unfortunate playerId pair.

**Fix.** Lower priority. Acceptable for current scale. Worth noting in CLAUDE.md so the next "what's the latency tail look like?" investigation has a starting point.

### V3-M3. `SessionCache::NoteAuthRecovered` iterates the cache under `m_mutex` — re-entry from `Validate` path would deadlock
**Files:** `Server/Common/src/SessionCache.hpp:201-223`
**Status:** Verified safe today; flagged for documentation.

The M-V2-9 split correctly enforces "NoteAuthRecovered must NOT be called while holding m_mutex". Audit of every call site:
- Line 76: `NoteAuthLost` (lock-free, safe inside lock). ✓
- Line 96: `NoteAuthLost` (called inside the lock — safe). ✓
- Line 111: `NoteAuthLost` (called outside the lock after `}` at line 102 — safe either way). ✓
- Line 123: `NoteAuthLost` (outside the lock). ✓
- Line 129: `NoteAuthRecovered` (outside the lock — required). ✓

All five sites match the contract. The split is correct. **But** the contract is enforced by a comment block at lines 197-200 — a future hand modifying `Validate` to factor common cleanup into a helper could easily move the `NoteAuthRecovered` call inside the lock and trip a deadlock. There's no test coverage for the recovery path that would catch this.

**Fix.** Either rename `NoteAuthRecovered` → `NoteAuthRecoveredLockFree` (its name advertising the contract) or add a `std::lock_guard<std::mutex>` ownership check in debug builds. A test that simulates Auth-down → Auth-up and asserts the cached-extended entries are evicted would also surface a regression.

### V3-M4. `SaveAllAndClear` calls `m_repository.Save` while still holding the per-player stripe lock, which can be the longest operation in shutdown
**Files:** `Server/Account/src/AccountCache.hpp:186-216`
**Status:** Trade-off — flagging because the v2 audit's stripe-ordering analysis didn't explicitly cover this.

Shutdown's `SaveAllAndClear` is two-phase: Phase 1 swaps out all unique_ptrs (map-mutex-only); Phase 2 iterates, takes a stripe lock per-player, and `Save()`s. The stripe lock is held across `m_repository.Save(*account)`, which can be 100+ ms (full Account → row writes across 13 tables for a heavy player). With 100+ accounts loaded this serializes 1-by-1 — shutdown takes seconds.

The stripe lock here is mostly defensive: nothing else CAN concurrently access the account in Phase 2 (the unique_ptr is already moved into the local vector, and m_accounts no longer contains the key). The only thing the stripe lock protects against is an in-flight client/internal RPC that's mid-handler holding the same stripe — but that handler's `LockedAccountRef.account` is the Account* whose unique_ptr we now own. If we Save concurrently with that handler still mutating, we'd capture half-mutated state.

So the stripe lock IS needed. But: Phase 2 could parallelize across stripes (the stripes are independent), saving N accounts concurrently up to the stripe count. Today the shutdown is serial; parallelizing would shorten shutdown wall-time for heavy services.

**Fix.** Low priority for current scale. Note the trade-off.

### V3-M5. `m_internalRpcHandlers` and `m_cache` declared in derived class but `m_internalEndpoint` in base — destruction order asymmetry
**Files:** `Server/Account/src/AccountServer.hpp:323,330` vs `Server/Common/src/TcpServerBase.hpp:252`
**Status:** Pre-existing layout, called out by V3-H1 above as the underlying mechanism.

The pattern that bit V3-H1 is structural: any time you have a "registry of callbacks bound to derived-class members" in a base class, the base outlives the derived. The current code works only because `Stop()` is called explicitly before destruction and the 10s drain "usually" completes. The whole shape is fragile.

**Fix.** Restructure so `m_internalEndpoint` is owned by the derived class and `TcpServerBase` only knows it as a virtual hook (e.g., `virtual void StartInternalEndpoint() = 0;`). Larger refactor; defer until V3-H1 forces the issue.

### V3-M6. AccountCache has no unit tests — every concurrency invariant is verified by reviewer attention only
**Files:** `Server/Account/tests/` (no `AccountCacheTest.cpp`)
**Status:** Test coverage gap. The v2-followup-test-coverage already covered the broader "no concurrency tests" issue, but the M-V2-3 extraction is a fresh opportunity that was missed.

Specific tests that would catch the issues flagged above:
- `GetLockedAccount` on a stale Account erases and reloads (V3-C1 backstop).
- `CleanupIdleAccounts` skips stale Accounts without leaking the entry.
- `SaveAllAndClear` Phase 2 blocks on a stripe lock held by a "concurrent handler" thread, then proceeds.
- `InsertIfAbsent` with a key that already exists is a no-op (no overwrite).
- `UpdateCachedPasswordHash` mutates the cached Account; absent Account = no-op.
- Two-thread test: thread A holds `LockFor(player1)`; thread B `GetLockedAccount(player1)` blocks; A releases; B proceeds.

---

## LOW / OBSERVATION

### V3-L1. `LockFor` returns `std::unique_lock<std::mutex>` without `[[nodiscard]]` — see V3-H2 fix #1.
Mentioned in V3-H2; minimal one-line fix.

### V3-L2. `m_lastAccess` is updated by `InsertIfAbsent` even when no insert happens
**Files:** `Server/Account/src/AccountCache.hpp:237`
The unconditional `m_lastAccess[playerId] = now()` means a VerifyCredentials call on a player whose Account is already cached refreshes their idle timer. The bump is reasonable (VerifyCredentials = the player just touched the system), but document the semantic — a future "why doesn't player X idle out?" investigation would find this surprising.

### V3-L3. `GetLockedAccount`'s emplace loop ignores the `inserted` flag
**Files:** `Server/Account/src/AccountCache.hpp:112-114`
```cpp
auto [it, inserted] = m_accounts.emplace(playerId, std::move(account));
accountPtr = it->second.get();
```
`inserted` should always be true under the stripe lock (no other thread can have inserted a same-key Account between our erase-stale-or-find-end check and this emplace). If `inserted == false`, we have a real bug (two threads bypassed the stripe ordering). Worth asserting:
```cpp
assert(inserted && "AccountCache::GetLockedAccount: emplace lost a race — stripe ordering broken");
```

### V3-L4. The "freshly hydrated `ClearStale()` is a no-op" observation from v2-followup L2 carries over verbatim to AccountCache
**Files:** `Server/Account/src/AccountCache.hpp:106`
`AccountHydrator::FromData` returns an Account whose default-constructed `m_stale = false`. The explicit `account->ClearStale()` is documentation, not state mutation. Either drop it or comment why it's defense-in-depth.

### V3-L5. `m_stale` access invariant — comment on Account.hpp:47-49 still missing from v2-followup L1
**Files:** `Server/Account/src/Account.hpp:47-49,~532`
Verified: every read/write of `m_stale` is under the stripe lock (see v2-followup-concurrency L1 audit table). A one-line comment near the member declaration ("invariant: read/written only under the per-player stripe lock; no `std::atomic` needed") still hasn't been added.

### V3-L6. `LruCache` thread-safety contract is well-documented; the only consumer (`RateLimiter`) wraps with `m_mutex`
**Files:** `Server/Common/src/LruCache.hpp:13-15`, `Server/Common/src/RateLimiter.hpp:65,121,141,154`
Verified: `LruCache` is NOT thread-safe (explicit in the header comment). The only consumer is `RateLimiter`. Every `RateLimiter` method that touches `m_records` takes `m_mutex` first (`Allow`, `GetCooldownRemaining`, `Reset`, `CleanupExpired` is private and only called from `Allow` under the same lock). Match. Grep verified `LruCache` is referenced only by `RateLimiter.hpp` + the test file. Good shape; the v2-followup's M-V2-10 fix landed cleanly.

### V3-L7. `InternalRpcAuth::GetSharedSecret` static-local init — C++11 thread-safe, verified
**Files:** `Server/Common/src/InternalRpcAuth.hpp:53-66`
Verified per focus question #9. The static-local initialization is thread-safe per C++11 [stmt.dcl]/4 — concurrent threads racing the init will block until exactly one thread completes the initializer, then all see the same value. The lambda has no side effects beyond reading `std::getenv` (which is technically not thread-safe relative to `setenv`, but no code path here calls `setenv`). No torn read possible on cold start. M-V2-11 fix is correct.

### V3-L8. CleanupIdleAccounts two-phase preserved through M-V2-3, with one subtle behavioral preservation
**Files:** `Server/Account/src/AccountCache.hpp:124-182`
Verified per focus question #7. The two-phase pattern (snapshot candidates under map-only lock → take stripe lock per candidate → re-check `pendingCleanup` under map lock → move-out under map lock → save outside both locks) is preserved verbatim from the pre-extraction inline form. The re-check at line 149 (`if (m_pendingCleanup.count(playerId) == 0) continue;`) handles the race where a `GetLockedAccount` between Phase 1's mark and Phase 2's stripe-acquire erased the player from `pendingCleanup` — that handler "stole" the eviction. Correct.

### V3-L9. Detached connection threads consume connection-pool leases — a slow DB can pile up beyond the pool size
**Files:** `Server/Common/src/ServiceEndpoint.hpp:78-84` + `Server/Account/src/db/ConnectionPool.hpp`
Not in scope of M-V2-3 but worth noting as part of the V3-H1 hazard area: detached threads holding a lease through a slow DB will eventually exhaust the pool. H5's `wait_for` timeout throws `PoolExhausted`, which unwinds the handler cleanly, but the detached thread itself outlives the unwind. Combined with V3-H1's drain timeout, a slow DB at shutdown is the most likely real-world trigger for the use-after-free.

---

## Verified Closed / Confirmed Healthy

1. **M-V2-3 lock ordering preserved everywhere** (focus question #1). `GetLockedAccount`, `CleanupIdleAccounts`, `SaveAllAndClear` all acquire stripe → map. `LockFor` + `InsertIfAbsent` + `UpdateCachedPasswordHash` rely on the caller-holds-LockFor contract (V3-H2 flags the contract not being enforced, but the single live caller in InternalRpcHandlers honors it). No lock-order inversion possible on any traced path.

2. **H-V2-3 re-read under stripe lock** (focus question #2). `InternalRpcHandlers::HandleVerifyCredentials:108` takes `LockFor` after PBKDF2 verify; line 115 re-reads `LoadByUsername` under the stripe lock; rehash / cache-insert sees fresh state. The 2× DB read overhead (~5ms) is acceptable against the PBKDF2 200ms — math checks out.

3. **M-V2-1 CleanupIdleAccounts two-phase preserved** (focus question #7). See V3-L8 above. Phase 1 snapshots candidates under map-only lock, Phase 2 re-checks pending-cleanup under map lock after stripe acquisition, save outside both locks. Verbatim relocation.

4. **M-V2-9 NoteAuthLost / NoteAuthRecovered split** (focus question #4). Verified at all 5 call sites (see V3-M3 above for the table). No site holds m_mutex when calling NoteAuthRecovered; NoteAuthLost is called both inside and outside the lock and is documented as lock-free-safe. Split is correct.

5. **M-V2-11 static-local secret cache** (focus question #9). See V3-L7. C++11 thread-safe init guarantee makes a torn read impossible on cold start.

6. **LruCache thread-safety boundary** (focus question #6). See V3-L6. Single consumer (RateLimiter) wraps every access with m_mutex. No other consumers in the tree.

7. **OnStopped path post-M-V2-3** (focus question #8). The `m_cache.SaveAllAndClear()` call is correct relative to in-flight handlers IF the ServiceEndpoint drain completes within 10s. The detached-thread / 10s-timeout hazard (V3-H1) is the only soft spot.

8. **C7 Memento mechanics on the non-pity-accessor paths.** AddCurrency (AccountHandlers.hpp:368), CommitProgressionScrapSpend (ProgressionHandlers.hpp:184/273/365/455), HandleClaimQuestReward (QuestHandlers.hpp:549), HandleReportQuestProgress (QuestHandlers.hpp:287), HandleCompleteQuest (QuestHandlers.hpp:686) all call `Begin()` before any explicit mutation. The pity-accessor reopening is isolated to HandlePull / HandleMultiPull (V3-C1).

---

## Suggested triage order

**Today / immediate:**
1. **V3-C1** — move `Begin()` above the `GetPity` / `GetGuarantee` accessors in HandlePull (GachaHandlers.hpp:117) and HandleMultiPull (GachaHandlers.hpp:364). ~2 line moves. Restores C7-A guarantee on these two paths.
2. **V3-H3** — switch `InsertIfAbsent` to `try_emplace`. One-line change; closes the stranded-nullptr trap reintroduced from v2-followup-M1.

**This week:**
3. **V3-H2** — `[[nodiscard]]` on `LockFor` + lock-token parameter on `InsertIfAbsent` / `UpdateCachedPasswordHash`. Type-system enforcement of the contract that's currently documented-only.
4. **V3-H1** — pick one of the three fix options (join-not-detach is the most conservative). The current 10s timeout + detached threads + base/derived destruction asymmetry is a UAF waiting for the right shutdown timing.

**Before launch:**
5. V3-M1 — `!IsStale()` guard in `UpdateCachedPasswordHash`.
6. V3-M6 — AccountCache test suite (the six tests listed). Covers the new extraction and would catch a future regression of any of the above.
7. V3-M5 — restructure `TcpServerBase` so `m_internalEndpoint` lives in the derived class. Larger refactor; pair with the V3-H1 fix.

**Eventually:**
8. V3-M2 / V3-M3 / V3-M4 + L-class items as polish.
