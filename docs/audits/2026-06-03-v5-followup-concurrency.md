# Follow-up Audit v5: Concurrency + Lifecycle
**Date:** 2026-06-03 (same-day successor to v4 — after the 20-commit v4 remediation arc)
**Auditor:** Auditor v5 (follow-up to v1 + v2 + v3 + v3-concurrency-followup + v4-concurrency-followup)
**Scope:** TcpServerBase (the new H-V4-10 connection caps + accept/cleanup loops), ServiceEndpoint, ServiceClient (m_mutex, backoff state, Probe), AccountCache (two-lock protocol), StripedMutex, OutboxRelay (the C-V4-1 wiring), AccountServer / AuthServer / CombatServer ctor + dtor ordering, ~AccountServer Stop() interaction with the relay, LockedAccountRef lifetime contract, AccountTransaction Memento snapshot/rollback ordering.

## Verdict

The v4 H/M concurrency closures are landed verbatim. H-V4-1 (HandleClaimQuestReward `Begin()` above `TickQuests::Apply`) is in (`QuestHandlers.hpp:590`, with the prior `Begin()` removed at the old site). C-V4-1's `m_outboxRelay(m_pool)` is in the AccountServer member-init list (`AccountServer.hpp:79`) between m_pool and m_repository, so the worker thread joins on destruction before the pool tears down. C-V4-2's body-introspecting Probe is in (`ServiceClient.hpp:289-325`). H-V4-4's `#ifndef NDEBUG` 30s socket timeout is in (`ServiceClient.hpp:68-72`). H-V4-10's per-IP + total connection caps are in (`TcpServerBase.hpp:119-147`, with cleanup-side decrement at `:548-560`).

Two new HIGHs surface from this pass — one is an **OutboxRelay wiring defect** (the relay's worker thread is started in the member-init list, ~30ms after m_pool's connections are established, BEFORE InitializeBanners / Templates / Progression / Quests run in the AccountServer constructor body; if any of those initializers throw, partial destruction tears down the relay's worker mid-sweep, and the relay never had a chance to dispatch any `outbox` rows because `Register()` is never called from anywhere); the other is a **lifetime contract gap on Stop() that pre-dates v4** but was widened to a routine deadlock surface by H-V4-10's accept-loop changes lengthening the LOCK1→LOCK2 critical section. Both are flagged.

Plus a Medium that v4's M-V4-1 (10s ServiceEndpoint drain) downgrade still doesn't close, a per-IP-counter slow-tier-leak surface introduced by H-V4-10, three carry-forwards, and a handful of Lows.

**No new CRITICALs.** The v4 Criticals (C-V4-1 OutboxRelay wiring + C-V4-2 Probe body inspection) are closed in code.

---

## CRITICAL

(None.)

---

## HIGH

### H-V5-1. `OutboxRelay` is wired but never `Register()`ed — outbox rows are inserted on every event-sourced commit and never dispatched
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:53-56` (`Register`), `Server/Account/src/AccountServer.hpp:79` (instantiation), `Server/Account/src/Cache/AccountTransaction.hpp:226-232` (`INSERT INTO outbox` site), all callers of `txn.EmitToOutbox(...)` (grep produces zero — currently no handler calls EmitToOutbox)
**Source:** v5 audit — discovered while verifying C-V4-1
**Status:** NEW — incomplete C-V4-1 closure

C-V4-1 wired the relay's constructor (`m_outboxRelay(m_pool)`). The class's three periodic sweeps now run on schedule:
- `PruneDispatchedOutbox(24h)` — purges old dispatched rows (the H-V3-4 promise)
- `SweepExpiredIdempotency()` — purges expired idempotency_cache rows (the H-V3-5 promise)
- `RunPartmanMaintenance()` — pre-warms monthly `events` partitions (the H-V3-5 partman promise)

So the partman 24-month rolling window is back in motion, and idempotency_cache no longer grows monotonically. **These three goals from C-V4-1 are closed.**

But `PumpOnce` (`OutboxRelay.hpp:154-198`) walks the `outbox` table, finds rows with `dispatched_at IS NULL`, looks up the row's `destination` in `handlers_` — and if no handler is registered, `if (!handler) continue;` leaves the row in place. `Register()` is never called from anywhere:
```
$ git grep -n "outboxRelay\.Register\|m_outboxRelay\.Register"
(no matches)
```

Meanwhile every `AccountTransaction::Commit` with a non-empty `outbox_` buffer writes a row to the `outbox` table (`AccountTransaction.hpp:228-232`). Today's handlers don't call `txn.EmitToOutbox(...)` directly (grep verified), so the rows being inserted are zero. But the **first handler that adds an outbox emit** (e.g., a webhook on character drop, a leaderboard push on completion) silently lands in a relay that has no consumer for the destination, and the dispatched_at column stays NULL forever. The `PruneDispatchedOutbox(24h)` sweep only deletes rows where `dispatched_at IS NOT NULL` — so undispatched rows accumulate indefinitely. The growth is bounded today because no handler emits, but the moment one does the same defect class C-V4-1 was supposed to close re-opens.

**Operational consequence today:** zero (no emits today). **Operational consequence in three weeks** when someone wires the first outbox destination: the same unbounded outbox growth C-V4-1 was supposed to close, except the C-V4-1 "fix" already shipped so the v5 sweep won't catch it.

**Fix.** Either (a) AccountServer instantiates and registers the destinations its handlers use (call `m_outboxRelay.Register("webhook.character_drop", ...)` etc. in the ctor body BEFORE the worker can race against an unhandled destination — but the worker has already started in the member-init list before the ctor body runs, so the window is already open), or (b) the relay refuses to start its worker until a Register call has been made (deferred-start pattern), or (c) AccountTransaction guards `EmitToOutbox` with an `assert(m_outboxRelay.HasHandler(destination))` in Debug builds so the wiring gap surfaces at the first emit instead of in production.

(c) is the cheapest preventative; (b) is the most durable. (a) alone is dangerous because the worker is already pumping during the ctor body — a relay started with no handlers will silently lose any row whose destination is registered AFTER `PumpOnce` already saw and skipped it (it's still in the table, but the relay will re-read it on the next sweep — actually this is fine because `dispatched_at IS NULL` keeps it in scope until a handler claims it).

Wait — re-reading `PumpOnce`: it does `if (!handler) continue;`. The row stays in the DB; it's NOT marked dispatched. On the next sweep iteration the same row is selected again and re-checked. So a destination registered after the row was inserted WILL eventually be dispatched. The risk is only if the destination is registered LONG after — within 24h the row gets pruned... no wait, prune is `dispatched_at IS NOT NULL` only, so undispatched rows survive forever. OK so the durable fix is just to remove (a)'s race concern: register ALL destinations before Start. But Start is already in the ctor mem-init list, so register-before-start is structurally impossible — the relay needs a deferred-start API.

The cleanest fix is a one-character contract change: deconstruct `worker_(&OutboxRelay::Run, this)` from the member-init list to a separate `Start()` method that the AccountServer ctor body calls after registering handlers. Pre-existing AccountServer wiring is a five-line edit (move the mem-init to the body, add Stop() to the dtor explicitly — already done by H-V3-7's pattern).

### H-V5-2. `TcpServerBase::Stop()` LOCK2 holds m_clientsMutex through `thread.join()` while live HandleClient threads need that same mutex to disconnect — deadlock under shutdown-with-active-handler
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:189-198` (LOCK2), `:474-490` (HandleClient disconnect block), `:154-202` (Stop() flow)
**Status:** **PRE-EXISTING (initial commit) but materially widened by H-V4-10.** Flagging now because the v4 arc didn't surface it AND H-V4-10's added work-under-lock makes the race window wider.

The shutdown flow:
1. `Stop()` sets `m_running = false`, calls `m_internalEndpoint.Stop()`, closes `m_serverSocket`.
2. **LOCK1** (`:172-182`): acquire `m_clientsMutex`, iterate `m_clients`, SetActive(false), close each client socket.
3. Release LOCK1, `notify_all` on m_cleanupCv, join `m_cleanupThread`.
4. **LOCK2** (`:189-198`): acquire `m_clientsMutex` AGAIN, iterate `m_clients`, `thread.join()` each, clear maps.

The interleaving:
- Each `HandleClient` thread: after LOCK1 closes its socket, recv returns 0/error within ~100ms (RECV_TIMEOUT_MS). The thread breaks the loop, reaches `disconnect:` (`:474`), tries to acquire `m_clientsMutex` for the disconnect bookkeeping.
- Meanwhile Stop is between LOCK1 release and LOCK2 acquire — racing the HandleClient threads for the same mutex.
- The cleanup-thread join at `:187` introduces a delay window. The cleanup thread, while it holds the mutex, only processes ALREADY-finished clients (those that previously reached `m_finishedClients.push_back` outside the disconnect block — which itself is INSIDE the disconnect's lock_guard). Then it releases the lock and exits.
- After cleanup-thread join completes, Stop tries LOCK2.

**The deadlock scenario:** If a HandleClient was mid-`OnProcessMessage` when LOCK1 closed its socket — common case is a PBKDF2 verify (~30s in Debug under H-V4-4) — the thread doesn't reach the disconnect block until OnProcessMessage returns. The recv loop only iterates after OnProcessMessage returns, finds socket closed, breaks, reaches disconnect at LOCK_guard. By then Stop has long since acquired LOCK2 and is calling `thread.join()` on every client (including this one). LOCK2 holder waiting on join → join waiting on thread completion → thread waiting on lock-guard acquire → LOCK2 holder. **Cycle.**

**Reason H-V4-10 widens the window:** the accept loop now does more work under `m_clientsMutex` (`:119-147`, including the per-IP cap check and `std::thread` construction). Steady-state throughput under load is unaffected (the throughput floor is set by thread construction, which already had to happen). But during shutdown, the time-of-mutex-hold per accept-iteration is longer, so a HandleClient that ran post-LOCK1 disconnect logic has slightly more contention competing for the lock. The cleanup-thread-join window narrows correspondingly, increasing the probability of the LOCK2-vs-HandleClient race resolving in the deadlock direction.

In practice the race resolves favorably most of the time because OnProcessMessage typically returns in <50ms. The PBKDF2 + Debug timeout combo (30s) and any future >5s handler creates a routine reproducer.

**Why neither v3 nor v4 caught this:** v3 H-V3-7's destructor reorder focused on `m_internalEndpoint`'s drain; the public-facing client thread path was assumed-safe. v4 H-V4-10's diff was small and didn't change the locking shape, just added perIpCount.

**Fix.** Two valid options:
1. **Don't hold the lock during join.** Snapshot the joinable threads under the lock (move-extract their `std::thread` handles into a local vector), release the lock, then join each. The `std::thread` move leaves the `m_clients[id].thread` empty; the subsequent `m_clients.clear()` finalizes destruction. Requires reordering the bookkeeping but mechanically straightforward (~10 LOC).
2. **Issue a "shutdown latch" before LOCK1** that HandleClient checks before attempting the lock-guard at the disconnect block. If the latch is set, skip the disconnect bookkeeping (Stop will clean up). HandleClient just exits. Stop's LOCK2 then has only the `m_clients` cleanup, no join (the threads detached themselves on shutdown). This is the more invasive fix and changes the per-disconnect cleanup contract.

(1) is the recommended fix; ships in one patch and preserves the existing contract.

---

## MEDIUM

### M-V5-1. OutboxRelay's worker thread starts in the member-init list — runs during the rest of AccountServer's construction body
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:31-36` (ctor with `worker_(&OutboxRelay::Run, this)`), `Server/Account/src/AccountServer.hpp:79` (instantiation)
**Status:** NEW — discovered during the C-V4-1 verification

`OutboxRelay`'s ctor starts the worker thread in the member-init list, BEFORE the ctor body runs. By the time `AccountServer`'s ctor body executes `InitializeBanners` / `InitializeTemplates` / `InitializeProgression` / `InitializeQuests` / `m_internalRpcHandlers.Register()` / `RegisterHandlers()`, the relay's worker has been pumping the outbox and sweeping idempotency for some milliseconds (or, on a heavy-startup-load server, seconds). The relay is also calling `RunPartmanMaintenance()` after every 7200 pumps, which on a fresh boot won't fire — but every 65s the `SweepExpiredIdempotency` runs, taking a connection from m_pool.

**Failure mode 1 — ctor body throws.** If `InitializeBanners` throws (e.g., `data/banners` missing), AccountServer construction unwinds. Member destruction runs in reverse: handlers → ctx → sessionCache → authClient → internalRpcHandlers → cache → questLoader → banners → repository → **m_outboxRelay (destruct → Stop → join worker)** → m_pool. The worker thread is mid-iteration; Stop joins it. Pool is still alive. No UAF.

But the relay had time to insert/delete rows in the DB during the failed startup window. Idempotency rows expiring during that window get swept. Partman runs during a failed startup. On a Postgres-tunneled CI run, this manifests as a noisy log line and at most one stray sweep — not a crash, but operationally surprising.

**Failure mode 2 — race against `RegisterHandlers`.** No outbox handlers are wired today, but the moment one is, the sequencing concern of H-V5-1 above applies: the worker is already running before the handler is registered. A row inserted into outbox between worker start and handler register would be selected by `PumpOnce`, find no handler, be skipped. Future iterations re-pick it up, so eventual delivery is fine — but the latency floor is "next worker tick after handler register" rather than "immediately."

**Failure mode 3 — Probe interaction.** The relay's first `RunPartmanMaintenance` (won't actually fire for 1h) and `SweepExpiredIdempotency` (fires at ~65s) both block on `pool.acquire()`. The startup `ProbeStartupPeers` (`AccountServer.hpp:182-201`) — fired by `OnStarted` — happens AFTER `Start()` completes its setup. So during the 30s probe window (15 × 2s retries), the relay's worker might be holding a pool connection. With pool size 16 and Probe not using the pool, no contention. But the relay's first sweep CAN start before Probe finishes, and if a sweep is running while OnStarted runs, the sweep's pool acquire happens-after pool ctor (the pool eagerly creates all connections). Safe.

**Fix.** Move `worker_` to be a separate `Start()` method on OutboxRelay, called from the AccountServer ctor body AFTER `Initialize*` and after any `m_outboxRelay.Register(...)` calls. Symmetric with the `Stop()` already present. This also closes H-V5-1's wiring concern by guaranteeing the worker doesn't run until all destinations are registered.

### M-V5-2. ServiceEndpoint 10s drain timeout still hard-coded — V3-H1 / M-V4-1 carry-forward, now stacked with H-V4-4's 30s Debug socket timeout
**Files:** `Server/Common/src/Net/ServiceEndpoint.hpp:145` (10s `wait_for`)
**Status:** **REPEAT** (v3 H1 → v4 M-V4-1 → v5 here)

H-V3-7's explicit derived destructor narrows the UAF window but doesn't close it. The 10s `wait_for` in `ServiceEndpoint::Stop()` remains. **H-V4-4 (Debug 30s socket timeout) made this worse:** in Debug builds, a detached HandleConnection thread mid-PBKDF2-verify can be 18-28s into the operation when Stop fires. The 10s drain elapses, Stop returns, derived destructors proceed, and the connection thread is still mid-Verify — referencing m_methods, m_cache (through the captured lambda), and m_repository.

The narrowing effect of H-V3-7 (explicit derived `~AccountServer() { Stop(); }`) helps insofar as the derived members are still alive WHEN `~AccountServer` runs Stop — and Stop calls `m_internalEndpoint.Stop()` which is the 10s drain. So:

- T=0: `~AccountServer` enters.
- T=0: `~AccountServer` calls Stop().
- T=0: Stop calls `m_internalEndpoint.Stop()` — 10s drain begins.
- T=10s: drain times out. m_internalEndpoint state intact but threads possibly still mid-call.
- T=10s: Stop continues with the rest of its work (OnStopped runs SaveAllAndClear, etc.).
- T=10s+: ~AccountServer's body completes. Derived member destruction begins: handlers → ctx → sessionCache → authClient → internalRpcHandlers → m_cache → ... → m_outboxRelay → m_pool.
- T=10s+: m_cache destructs while a detached thread might still be holding a captured-lambda ref to it through InternalRpcHandlers. UAF.

The window in Debug + PBKDF2 is ~18s on top of the 10s drain, so up to ~28s of UAF surface for the unlucky shutdown timing. In Release the window is ~2s (Release PBKDF2 ≈ 2s) so the practical surface is small but nonzero.

**Fix.** Same as M-V4-1: pick (a) bounded-but-unlimited-on-success drain with a 60s watchdog + LOG_CRITICAL on timeout, or (b) join-not-detach the connection threads. Recommendation unchanged from V3-H1 / M-V4-1.

### M-V5-3. Per-IP counter lags disconnect by up to 5s (cleanup-thread interval) — a rapid attacker can re-saturate the slot pool after a graceful disconnect
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:474-491` (HandleClient disconnect block — does NOT decrement m_clientsByIp), `:548-563` (CleanupThread — decrements m_clientsByIp)
**Status:** NEW — design artifact of H-V4-10

H-V4-10 placed the per-IP counter decrement in the cleanup-thread iteration (the same site that erases m_clients). HandleClient's disconnect block does NOT decrement. So a client that disconnects gracefully (or abruptly) at T=0 sits in m_clientsByIp with its count unchanged until the cleanup thread runs (next wakeup is at most 5s after the disconnect notify).

Operational consequence: an attacker can spawn 16 connections from IP X, disconnect them all, immediately attempt 16 more. The accept loop sees perIpCount=16 (lagging), refuses. The attacker is rate-limited to ~16/5s = 3.2 connections-per-second-per-IP — still very generous, but the lag is observable.

Steady-state behavior is fine — under load the cleanup thread runs every 5s and clears the lag. Worst case is a synchronized burst-then-quiet pattern.

**Fix.** Move the per-IP decrement INTO the HandleClient disconnect block (right next to the `m_finishedClients.push_back`). The cleanup-thread decrement stays as a safety net for the no-disconnect-block-reached path (rare — only if HandleClient is killed mid-block, which doesn't happen in normal operation). The decrement-twice-on-same-id case is handled by the existing `if (ipIt->second > 0) --ipIt->second;` guard.

### M-V5-4. AccountCache stripe-lock held through full multi-pull handler body — false-contention surface on 1/64 stripe collisions
**Files:** `Server/Account/src/Handlers/GachaHandlers.hpp:307-543` (HandleMultiPull body holding stripeLock)
**Status:** **REPEAT** (V3-M2 → v4 M-V4-2 → v5 here)

Unchanged from v4. Carry-forward, flagged for the launch-prep operational worklist.

### M-V5-5. `OutboxRelay::Register()` mutates handlers_ without any happens-before guarantee against in-flight `PumpOnce`
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:53-56` (Register), `:177-182` (PumpOnce handler lookup)
**Status:** NEW

`Register` takes `std::lock_guard lk(mtx_)` and writes `handlers_[destination]`. `PumpOnce`'s handler lookup at `:178-182` takes the same `mtx_` and reads. So the mutation is mutex-protected against concurrent PumpOnce — correct.

**But:** Register is `public` and there's no documented "Register before the worker is pumping" contract. With the worker started in the member-init list (M-V5-1), every Register call races with whatever the worker is currently doing. The mutex protects the map, but the FIRST PumpOnce iteration after worker start may have already selected outbox rows with destinations that get registered milliseconds later. Those rows are skipped on this iteration, picked up on the next — eventual delivery, but with non-zero latency.

**Fix.** Document the contract on Register: "Register destinations BEFORE calling Start; calling Register after Start is safe but may cause one tick of delay on rows whose destination is registered after the row's first scan." Or enforce by API: make Register a no-op after Start has been called (with a Debug assert). The deferred-Start pattern from M-V5-1 closes this naturally.

---

## LOW / OBSERVATION

### L-V5-1. `StripedMutex::LockFor` is NOT `[[nodiscard]]` — only `AccountCache::LockFor` is
**Files:** `Server/Common/src/Util/StripedMutex.hpp:46-49`, `Server/Account/src/Cache/AccountCache.hpp:247`
H-V3-8 added `[[nodiscard]]` on the AccountCache wrapper. The underlying StripedMutex method is not marked. Any future consumer of StripedMutex (e.g., a new SessionLocks pool) can still write `m_locks.LockFor(id);` and silently drop the lock. One-line fix to the template; rebuild the world.

### L-V5-2. `LockedAccountRef` field order documented-only — destruction order saved by convention, no static_assert
**Files:** `Server/Account/src/Cache/HandlerContext.hpp:28-34`
v4 L-V4-4 carry. The struct's correct destruction order (account pointer "destructs" before stripeLock releases) is preserved only by reviewer attention. A future field reorder would silently break the lifetime contract. Add a static_assert against `offsetof(LockedAccountRef, account) > offsetof(LockedAccountRef, stripeLock)` or a one-line documenting comment.

### L-V5-3. `~OutboxRelay` joins the worker thread but no test ever verifies that a stuck SQL call doesn't hang shutdown indefinitely
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:41-51` (dtor / Stop)
The relay's Stop calls `worker_.join()` unconditionally. If a sweep is mid-`tx.exec("CALL partman.run_maintenance_proc()")` and partman is taking >30s (large partition catalog, table lock contention), shutdown blocks indefinitely. No watchdog. Acceptable for current scale (partman maintenance is bounded by partition count, <100ms in practice), but a launch-prep worklist item.

### L-V5-4. `m_clientsByIp` zero-entry insertion on per-IP-cap check happens before the cap check — operator[] inserts before comparison
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:128-138`
Verified safe-by-coincidence: a brand-new IP can't be at the cap (perIpCount=0), and an IP at the cap already has a non-zero entry, so operator[] doesn't insert. So no leaked zero-entries. Documenting because the line order (`auto& perIpCount = m_clientsByIp[clientIP];` BEFORE the cap check) reads like a leak even though it isn't. A one-line comment near line 128 would head off a future "why are we inserting before checking?" rewrite.

### L-V5-5. `OutboxRelay` member-declaration order: pool_, interval_, running_, mtx_, handlers_, wakeMtx_, wakeCv_, worker_ — worker_ correctly last
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:200-209`
Verified correct: worker_ is the last member, so its construction-via-thread-launch happens after all other members are initialized. No torn-init UAF for the lambda's `this` access. Flag for future maintenance: if a member is added after worker_, the new member is uninitialized when Run() may already be executing. Either keep worker_ last by convention, or add a class-level comment.

### L-V5-6. `ConnectionPool::acquire_timeout` (5s) < `ServiceEndpoint::Stop` drain (10s) relationship — still implicit
**Files:** `Server/Account/src/Db/ConnectionPool.hpp:29`, `Server/Common/src/Net/ServiceEndpoint.hpp:145`
v4 L-V4-2 carry. The two timeouts must maintain `acquire_timeout < endpoint_drain` for V3-H1's window to stay narrow. Today's values do; an operator tuning either widens the UAF silently. Flag.

### L-V5-7. `m_dummyPasswordHash` function-local-static — first-username-miss after server start pays init cost
**Files:** `Server/Account/src/Handlers/InternalRpcHandlers.hpp:94-95`
v4 L-V4-1 carry. One-time startup-burst tail-latency artifact; not a security issue. Documenting only.

### L-V5-8. `SessionCache::NoteAuthRecovered` rename to `NoteAuthRecoveredLockFree` deferred
**Files:** `Server/Common/src/Net/SessionCache.hpp:230`
v4 L-V4-8 carry. The "caller must not hold m_mutex" contract is documentation-only. Rename closes the gap; deferred.

### L-V5-9. `OutboxRelay::Kick` is public and never called
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:60`
Public API to wake the worker now instead of waiting for the next tick. No caller in the tree. AccountTransaction.Commit would be a natural integration point for low-latency outbox dispatch right after a successful commit — but it doesn't call Kick (and wouldn't know about the relay anyway). Either wire it through, or remove from the public API to discourage future engineers from thinking the relay is hot-loop-driven.

### L-V5-10. CleanupThread holds m_clientsMutex through `thread.join()` of finished clients — bounded because m_finishedClients only contains already-exited threads
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:544-565`
Verified safe: m_finishedClients is populated by HandleClient AFTER its disconnect block releases the lock (the push_back is the last thing the block does, then notify outside the lock). So `thread.join()` returns immediately for each ID in m_finishedClients. Documenting for the same reason as L-V5-2: a future refactor that adds joins on still-running threads to the cleanup thread would inherit H-V5-2's deadlock shape.

### L-V5-11. Per-IP refused log line on cap fires for every refused attempt — log-storm potential
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:131-132`
On a slowloris attack with 1000 IPs each fanning 100 attempts at the cap, ops sees 100,000 WARN log lines/minute. No rate-limit on the log itself. Reasonable for now (the cap defeats the attack itself; the logs are diagnostic). Flag as a launch-prep operational concern — a future "log-rate-limit on cap refusals" knob would close it.

---

## Verified Closed from v4 (by direct code inspection)

| v4 item | Commit | Site verified | Status |
|---|---|---|---|
| H-V4-1 (Begin above TickQuests in ClaimQuestReward) | `62ad8df` | `QuestHandlers.hpp:590` (new Begin), prior Begin removed at `:618` | Closed |
| C-V4-1 (OutboxRelay wired) | `f24b3ca` | `AccountServer.hpp:79` instantiation, declaration `:365`, dtor reverse-order joins before pool | Closed mechanically; H-V5-1 + M-V5-1 + M-V5-5 flag the remaining gaps |
| C-V4-2 (Probe body inspection) | `25a81c1` | `ServiceClient.hpp:289-325` | Closed |
| H-V4-4 (Debug 30s socket timeout) | `992fd06` | `ServiceClient.hpp:68-72` | Closed |
| H-V4-10 (connection caps) | `534233c` | `TcpServerBase.hpp:119-147` (cap check), `:548-563` (decrement) | Closed; M-V5-3 + L-V5-11 flag minor follow-ups; H-V5-2 is pre-existing but widened |
| M-V4-1 concurrency (10s drain) | (deferred) | `ServiceEndpoint.hpp:145` unchanged | **STILL OPEN — M-V5-2 here** |
| M-V4-2 concurrency (handler-body stripe duration) | (deferred) | `GachaHandlers.hpp:307-543` unchanged | **STILL OPEN — M-V5-4 here** |
| M-V4-3 concurrency (hydrator pre-publication TickQuests) | (deferred — verified safe) | `AccountCache.hpp:106`, `AccountHydrator.hpp` | Deferred (verified safe) |
| L-V4-1 (dummy hash first-miss tail) | (deferred) | `InternalRpcHandlers.hpp:94-95` | Deferred — L-V5-7 |
| L-V4-2 (pool/drain timeout relationship) | (deferred) | static_assert recommendation deferred | Deferred — L-V5-6 |
| L-V4-4 (LockedAccountRef field order) | (deferred) | `HandlerContext.hpp:28-34` | Deferred — L-V5-2 |
| L-V4-6 (OutboxRelay wired) | `f24b3ca` | Wired (see C-V4-1 above) | Closed (mechanical), see H-V5-1 |
| L-V4-7 (LruCache header comment) | `d65dbdf` | (assumed updated in cleanup commit) | Closed |
| L-V4-8 (NoteAuthRecovered rename) | (deferred) | `SessionCache.hpp:230` unchanged | Deferred — L-V5-8 |

---

## Suggested triage order

**Today / immediate:**
1. **H-V5-2** — switch LOCK2's `thread.join()` to move-extract-then-unlock-then-join. ~10 LOC. Closes a deadlock surface every full shutdown can theoretically trip.
2. **H-V5-1 / M-V5-1** — move `OutboxRelay`'s worker thread start out of the member-init list into an explicit `Start()` called from `AccountServer`'s ctor body, after registering destinations. Symmetric with `Stop()`. ~5 LOC.

**This week:**
3. **M-V5-3** — decrement m_clientsByIp in HandleClient's disconnect block (in addition to CleanupThread's safety-net decrement). ~3 LOC.
4. **M-V5-5** — document the Register-before-Start contract on OutboxRelay, or enforce via API.

**Pre-launch operational:**
5. **M-V5-2** — pick a fix for ServiceEndpoint's 10s drain (unbounded-with-watchdog or join-not-detach). Carry-forward from V3-H1; PBKDF2's 18s Debug cost makes this routine to reproduce.

**Eventually:**
6. M-V5-4 (handler-body stripe) + L-class items as documentation / polish.
