# v6 followup — Concurrency + Lifecycle

**Date:** 2026-06-04
**Auditor:** v6 (post-Scope-4 sweep)
**Scope:** Hybrid per-account lock model (AccountCache, HandlerContext, Account), TcpServerBase lifecycle, ServiceEndpoint lifecycle, OutboxRelay/SnapshotWriter worker thread ordering, AccountServer member-init dependency chain, thread-join safety, NEW lock-ordering inversions introduced by Scope 4, atomic memory-model claims.
**Method:** Read prior v5 synthesis + v5 concurrency + v5 networking followups; read Scope 4 spec + plan; verified each closed item against current HEAD (`47d16f0`); walked the per-account lock acquisition graph end-to-end (GetLockedAccount, UpdateCachedPasswordHash, CleanupIdleAccounts, SaveAllAndClear); chased the OutboxRelay worker-thread vs. AccountServer ctor-body race against the closed items list.

---

## Status of v5 items in this dimension

| Item | Status | Notes |
|---|---|---|
| **H-V5-3 networking** — m_cleanupThread spawn-after-Stop SIGABRT | CLOSED (commit `058a1d1`) | Spawn moved above `OnStarted()` at `TcpServerBase.hpp:109`. Start() now also returns false when OnStarted requested early shutdown (M-V5-5 networking closed in same commit). |
| **H-V5-4 networking** — ServiceEndpoint 10s drain timeout widens UAF | CLOSED (commit `63f7e54` for InvokeForTest; drain widened in same v5 arc) | `kDrainTimeoutSeconds` is now Debug 30s / Release 10s at `ServiceEndpoint.hpp:36-40`, sized to (longest handler runtime + server recv timeout). Drain-failure WARN now logs straggler count. The detached-thread UAF is narrowed but not eliminated; carry-forward as L-V6-1 below. |
| **H-V5-6 concurrency** — TcpServerBase::Stop() LOCK2 deadlock | CLOSED (commit `90f2a88`) | Two-phase collect-then-join at `TcpServerBase.hpp:260-274`: thread handles moved out of `m_clients` under lock, lock released, join outside. Mirrors AccountCache::SaveAllAndClear pattern. |
| **M-V5-1 event-sourcing** — AppendIdempotent recheck advisory lock | CLOSED (commit `a9bb510`) | Recheck tx now holds per-account advisory lock; tx.commit() releases promptly. |
| **M-V5-1 concurrency** — OutboxRelay worker starts in member-init before ctor body | STILL OPEN | `worker_(&OutboxRelay::Run, this)` at `OutboxRelay.hpp:39` still in member-init. AccountServer ctor body's Initialize* and `RegisterHandlers` run after the worker starts. Tied to deferred H-V5-2 (Register-never-called); revisit triggers documented inline at OutboxRelay.hpp:57-86. Zero impact today (PumpOnce skips rows for unregistered destinations + no handler emits). |
| **M-V5-3 concurrency** — per-IP counter decrement lag | CLOSED (commit `3c59b2d`) | Decrement now eager in HandleClient disconnect block at `TcpServerBase.hpp:563-589`; CleanupThread's redundant decrement removed with rationale. |
| **M-V5-4 concurrency** — Scope 4 per-account locks refactor | CLOSED (Scope 4 commits `bb11065..47d16f0`) | See verification section below. |

All v5 concurrency-dimension items have either closed or are explicitly deferred with documented revisit triggers. Only M-V5-1 concurrency (OutboxRelay member-init order) remains open as a known deferral.

---

## Scope 4 verification — verdict: **clean**

The hybrid lock model (stripe = brief Phase 1; per-Account = held for handler duration) is internally consistent at HEAD. Specific verifications:

### Lock-ordering across cache methods

Verified order: **stripe → map → release map → release stripe → per-Account** in `GetLockedAccount`. **stripe (caller) → map → release map → per-Account** in `UpdateCachedPasswordHash`. **map → per-Account (try_lock)** in `CleanupIdleAccounts`. **map → per-Account (lock_guard)** in `SaveAllAndClear`.

No site holds per-Account while trying to acquire map. No site holds map while trying to acquire stripe. No nested per-Account acquisitions. No handler in `Handlers/*.hpp` acquires any cache-owned mutex outside of the canonical `getLockedAccount` lambda call.

### m_stale atomicity (post-o-followup)

`m_stale` is `std::atomic<bool>` with relaxed loads/stores at `Account.hpp:83-85`. Writer holds m_handlerMutex; reader (GetLockedAccount Phase 1) holds m_mapMutex. Different mutexes. The audit-tag comment claims "writer-happens-before-reader ordering is provided by the application invariant." This is technically over-stated — there's no formal happens-before edge between writer's m_handlerMutex release and reader's m_mapMutex acquire. On x86/x64 (project's only target), the relaxed ops are visible-everywhere via cache coherence and the compiler can't reorder atomic ops across mutex operations, so the practical behavior is correct. Flagging as a Low only because the comment overclaims; the runtime is fine. See L-V6-2 below.

### LockedAccountRef field-order invariant

`account` (shared_ptr) declared FIRST, `accountLock` (unique_lock) declared SECOND at `HandlerContext.hpp:52-53`. Reverse-order destruction means accountLock releases mutex BEFORE shared_ptr potentially drops to ~Account. Sub-batch (r)'s destruction-order test pins this contract at `AccountCacheTest.cpp:309-374`. No way to verify at compile-time (offsetof requires standard-layout), as the comment block at lines 45-49 acknowledges. Acceptable.

### UpdateCachedPasswordHash Phase 1/Phase 2 correctness

`UpdateCachedPasswordHash` at `AccountCache.hpp:484-507` correctly: (1) copies shared_ptr out under map lock with IsStale fast-path check; (2) releases map lock; (3) acquires per-Account handler mutex; (4) re-checks IsStale (handles the race where Rollback set stale between map release and per-Account acquire); (5) calls SetPasswordHash. The `cached` shared_ptr keeps the Account alive through the per-Account lock acquisition even if the cleanup thread evicts the map entry concurrently. Field-order at function scope: `cached` declared before `handlerLock` → handlerLock destructs first → mutex released before shared_ptr drop. Same lifetime invariant as LockedAccountRef. ✓

### CleanupIdleAccounts try_lock + bookkeeping (post-p-followup)

Two bookkeeping bugs from the original batch (p) were caught in followup commit `f0e3d76` and the corrected code in HEAD now: (1) try_lock failure path erases m_pendingCleanup so Phase 1's entry guard doesn't permanently skip the candidate; (2) m_lastAccess.erase is gated on identity match so a stale-reload's NEW lastAccess isn't wiped. Both invariants verified against current `AccountCache.hpp:208-343`.

The eviction race-safety is sound: peek shared_ptr under map mutex → release → try_lock per-Account → if owned, re-check map under map mutex with identity check (`it->second == candidateAccount`) → erase → release per-Account → save outside lock. Handler held a LockedAccountRef from before cleanup's peek → cleanup's try_lock fails → cleanup skips. Handler completes, releases per-Account, releases its shared_ptr. Next cleanup tick re-considers and succeeds.

### SaveAllAndClear per-Account mutex (post-q)

`SaveAllAndClear` at `AccountCache.hpp:359-394` correctly drains m_accounts under map mutex into a local vector, releases map mutex, then iterates and saves under per-Account handlerLock. At shutdown the lock is uncontended (TcpServerBase::Stop joins client threads before OnStopped runs SaveAllAndClear). The only path where it would block is if a detached ServiceEndpoint connection thread survived drain — see L-V6-1.

### NEW lock-ordering inversions introduced by Scope 4

Searched. None. All paths consistently follow stripe → map → per-Account. No handler holds per-Account while trying to acquire map or stripe. The per-Account mutex is acquired in exactly four places (GetLockedAccount Phase 2, UpdateCachedPasswordHash Phase 2, CleanupIdleAccounts try_lock, SaveAllAndClear lock_guard); none nests another cache-owned mutex inside.

---

## NEW findings

### [Medium-V6-1] SaveAllAndClear can wedge indefinitely if internal-RPC handler survives drain
**File:** `Server/Account/src/Cache/AccountCache.hpp:384`
**Status:** NEW (carry-forward interaction surface from H-V5-4 + Scope 4's per-Account model)
**Finding:** TcpServerBase::Stop() drains the internal endpoint with a 30s (Debug) / 10s (Release) timeout. If a `HandleGetPartyData` thread is still mid-call when drain times out (rare today — handler is fast, ~ms), it holds the per-Account handler mutex through the detached-thread window. Stop() then runs OnStopped → SaveAllAndClear, which iterates each account and `std::lock_guard handlerLock(account->HandlerMutex())` at line 384. If the detached thread still holds the per-Account mutex, SaveAllAndClear's lock_guard blocks indefinitely (no timeout, no try_lock). Stop() never returns.
**Impact:** Shutdown wedge bounded by however long the detached thread takes to complete. Practical exposure low (HandleGetPartyData is fast); becomes a hazard if any future internal-RPC handler holds per-Account through a slow DB call.
**Fix sketch:** Replace `lock_guard` with `unique_lock + try_lock_for` (e.g., 5s wedge ceiling) and log a CRITICAL on miss. Saves the in-memory projection without the lock as a fallback (still consistent — the drain timeout was already a UAF-class violation per H-V5-4's accepted residual risk).

### [Low-V6-1] ServiceEndpoint detached-thread UAF window still exists post-drain (H-V5-4 carry-forward)
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:182-187, 213-232`
**Status:** REPEAT (v5 H-V5-4)
**Finding:** The drain budget was widened (Debug 30s / Release 10s) to bound the practical window. The WARN now surfaces straggler count. But: connection threads remain `detach()`-ed at line 187, so if drain times out the detached thread runs against the destroying ServiceEndpoint instance. Members destruct in reverse declaration order — `m_methods` map and `m_methodsMutex` survive until last, but a detached handler reading the captured `this` pointer would still UAF on the m_running atomic load and (for derived TcpServerBase contexts) on subclass-captured state.
**Impact:** Pre-launch surface is bounded — handlers are fast, drain budget is large enough for PBKDF2 (~18s Debug ≤ 30s drain). Post-launch any handler that exceeds the drain budget reopens the window.
**Fix sketch:** Two options remain: (a) unbounded wait + 60s LOG_CRITICAL watchdog, or (b) join-not-detach. (b) is structurally cleaner but requires tracking thread handles. Documented as accepted residual risk.

### [Low-V6-2] m_stale relaxed memory order claim overstates happens-before
**File:** `Server/Account/src/State/Account.hpp:55-82, 597-614`
**Status:** NEW
**Finding:** The audit-tag comment block at lines 55-82 claims "memory_order_relaxed is sufficient because the writer-happens-before-reader ordering is provided by the application invariant." The writer holds `m_handlerMutex`; the reader (`GetLockedAccount` Phase 1's IsStale check) holds `m_mapMutex`. Different mutexes provide no formal synchronizes-with edge — the C++ memory model doesn't establish happens-before between writer's m_handlerMutex release and reader's m_mapMutex acquire. The "application invariant" reasoning conflates wall-clock ordering with happens-before, which it isn't.
**Impact:** On x86/x64 (the only supported target), atomic-relaxed stores are visible via cache coherence and the compiler can't reorder relaxed atomics across mutex operations — so the practical runtime is correct. On weak-memory ARM (not a target), a reader could in principle observe a stale value. The bound is: one extra handler call on the stale Account before the next access sees stale; that handler's Commit would fail with ConcurrencyConflict, the next access would reload. Self-healing.
**Fix sketch:** Either (a) change to `memory_order_release` on stores and `memory_order_acquire` on loads — one-instruction cost on x86, establishes formal happens-before edge regardless of which mutexes the synchronizers hold; or (b) reword the comment to acknowledge that the relaxed-correctness rests on x86's strong memory model and the self-healing property of the stale flag. Either fix is comment-density-balanced.

### [Low-V6-3] m_outboxRelay::worker_ still starts in member-init list (M-V5-1 carry-forward)
**File:** `Server/Account/src/Db/OutboxRelay.hpp:39`
**Status:** REPEAT (v5 M-V5-1 concurrency — explicitly deferred)
**Finding:** OutboxRelay's constructor at line 39 starts the worker thread in the member-init list before the ctor body runs. Same for SnapshotWriter at `SnapshotWriter.hpp:97` (currently uninstantiated per C-V5-1). At AccountServer construction, m_outboxRelay is constructed before InitializeBanners/Templates/Progression/Quests run. The worker pumps the outbox table and runs sweeps; none of these touch any post-ctor-body state (m_banners, m_questLoader, m_cache, m_repository all uninitialized but unused by the worker). Zero impact today.
**Impact:** Zero today. Becomes relevant when (1) a handler invokes `txn.EmitToOutbox` (also tied to H-V5-2's Register-never-called concern), or (2) SnapshotWriter is wired up (C-V5-1) and its worker accesses Account state mid-ctor-body. The wiring spec mandates registering destinations before the worker pumps, which is structurally impossible with member-init-list start.
**Fix sketch:** Documented inline at OutboxRelay.hpp:57-86 — when first EmitToOutbox call site lands, split `Run` out into an explicit `Start()` called from AccountServer ctor body after RegisterHandlers. Same patch applies to SnapshotWriter at C-V5-1 wire-up time. Pre-launch defer is acceptable because EmitToOutbox is not yet called by any production handler.

### [Low-V6-4] Phase 1 reload path emplaces without bumping m_lastAccess
**File:** `Server/Account/src/Cache/AccountCache.hpp:159-172`
**Status:** NEW
**Finding:** When GetLockedAccount falls through to the DB-load path (account not in cache, or stale-evicted), the re-acquire-and-emplace at lines 159-172 inserts the freshly-loaded shared_ptr into m_accounts but does NOT bump m_lastAccess at that point. Phase 1's earlier entry section (line 98) DID bump m_lastAccess unconditionally before the find. If the DB load took unusually long (>300s — pathological), m_lastAccess would now be stale relative to the moment the entry actually entered the cache. CleanupIdleAccounts could evict immediately on the next tick.
**Impact:** Negligible. The next GetLockedAccount on this player bumps m_lastAccess. The DB load is ~5-50ms in practice. The pathological scenario (DB load >5 minutes blocking) would only happen on connection pool exhaustion or DB outage, in which case the freshly-loaded account being evicted is the least of the operator's concerns.
**Fix sketch:** Bump m_lastAccess inside the emplace block at line 162 (one line, under the already-held map mutex). Cleaner invariant: m_lastAccess reflects entry-into-cache time, not "first attempted-access" time.

---

## Observations / Lows

- **SnapshotWriter's worker_ pattern is identical to OutboxRelay.** When C-V5-1 is closed, the same member-init-list start concern applies. The audit-tag in AccountServer.hpp:356-366 already documents the recommended sibling placement (after m_outboxRelay, before m_repository).
- **The TcpServerBase H-V5-3 fix (cleanup-thread spawn before OnStarted) is the canonical pattern** for any future "worker thread that should be joinable from a hook that can call Stop()." Worth keeping in mind if any future subsystem grows a similar lifecycle hook.
- **CleanupIdleAccounts is a sweep over m_lastAccess** — for accounts that were stale-evicted but never re-accessed, m_lastAccess remains live but m_accounts has no entry. The Phase-2 peek at line 246 correctly handles this (`it == m_accounts.end()` → erases the orphan lastAccess + pendingCleanup). Good — but it took me a second read to verify. A comment could pin this orphan-cleanup invariant.
- **VerifyCredentials's stripe lock (`LockFor`) still serves a real role** — it serializes the lazy-rehash path against concurrent VerifyCredentials calls on the same player during the "Account doesn't exist in cache yet" window where the per-Account mutex isn't available. The post-q comment at AccountCache.hpp:411-422 documents this clearly.
- **The Scope 4 plan's "snapshot pattern" alternative was correctly abandoned.** The idempotency-atomicity invariant at GachaHandlers.hpp:270-273 makes the response-payload-snapshot pattern unsafe (a retried call must return the buffered byte sequence exactly). Per-account locks eliminate the false-contention class without compromising the retry contract. The choice was the right one.
- **Thread-join safety summary.** OutboxRelay (worker_ joined in ~Stop in dtor), SnapshotWriter (same pattern, but uninstantiated), TcpServerBase (m_cleanupThread joined in Stop, fixed in H-V5-3), TcpServerBase per-client threads (joined in Stop's two-phase pattern, fixed in H-V5-6), ServiceEndpoint::m_acceptThread (joined in Stop) — all paths joined or detached deterministically. The only joinable-but-not-joined path was H-V5-3 (closed). ServiceEndpoint's detached connection threads survive drain by design (carry-forward L-V6-1).
- **AccountServer member-init dependency chain is clean.** m_pool before m_outboxRelay; m_outboxRelay before m_repository; m_repository before m_cache; m_cache before m_internalRpcHandlers; m_authClient before m_sessionCache; m_ctx after all data sources; handlers after m_ctx. Reverse-destruction order joins the OutboxRelay worker before m_pool tears down. No cycles.

---

## Verdict

**Clean.** Scope 4's per-account locks refactor is internally consistent — no new lock-ordering inversions, no UAF surfaces, no deadlocks introduced. The two reviewer-caught bugs (m_stale data-race in batch o → fixed in o-followup; pendingCleanup leak + lastAccess wipe in batch p → fixed in p-followup) are properly closed. The v5 high-tier concurrency/lifecycle items (H-V5-3, H-V5-4, H-V5-6) are all closed. The OutboxRelay member-init order (M-V5-1) remains the only known concurrency deferral, and its wiring spec correctly identifies the trigger for revisiting.

The new findings in this pass are all Medium-or-lower and surface around the **shutdown lifecycle interaction with the per-Account lock** (M-V6-1: SaveAllAndClear's lock_guard can wedge if detached thread holds the mutex) and **doc-vs-actual happens-before claims on m_stale** (L-V6-2). Neither blocks deploy.
