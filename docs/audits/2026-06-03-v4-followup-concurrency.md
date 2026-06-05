# Follow-up Audit v4: Concurrency + Lifecycle
**Date:** 2026-06-03
**Auditor:** Auditor v4 (follow-up to v1 + v2 + v3 + v3-concurrency-followup)
**Scope:** AccountCache, AccountTransaction, StripedMutex, TcpServerBase, ServiceEndpoint, ServiceClient, SessionCache, SessionManager, RateLimiter, LruCache, ConnectionPool, InternalRpcHandlers, all four handler domains (Account/Gacha/Progression/Quest), AuthServer + CombatServer + AccountServer destructors.

## Verdict

The v3 concurrency remediation arc is **fully landed in code** — every v3 H/M/L item in the focus list has a corresponding commit between `96f0893` (C-V3-2) and `0f6e12c` (concurrency Lows). C-V3-2's `Begin()` repositioning is verbatim where v3 said it should be (GachaHandlers.hpp:124 + :369); H-V3-1's scope expansion is verbatim (AccountHandlers.hpp:305, QuestHandlers.hpp:244, :725); H-V3-7 explicit `~Derived() { Stop(); }` is present on **all three** TcpServerBase subclasses (AccountServer.hpp:126, AuthServer.hpp:56, CombatServer.hpp:37); H-V3-8 `[[nodiscard]]` on `LockFor` (AccountCache.hpp:247); H-V3-9 `try_emplace` in InsertIfAbsent (AccountCache.hpp:273); H-V3-11 RateLimiter LRU inversion fixed via `Peek` (RateLimiter.hpp:77, :134); H-V3-2 advisory lock moved to `EnsureOpen` once-per-commit (AccountTransaction.hpp:396); M-V3-1 `!IsStale()` guard on UpdateCachedPasswordHash (AccountCache.hpp:289); L2/L3/L4/L5 documentation/assert additions all present.

**One NEW HIGH emerged: H-V3-1's C7-A scope-expansion missed `HandleClaimQuestReward`.** The handler calls `TickQuests::Apply` at QuestHandlers.hpp:579 — a mutating call that touches `m_questStates` (via Upsert / state transitions) and flips `m_dirty.quest_ids` — BEFORE `Begin()` at line 605 captures the Memento snapshot. The other three quest-mutating handlers (HandleReportQuestProgress :244, HandleCompleteQuest :725, plus HandleSetParty :305) correctly moved `Begin()` above the TickQuests / state-transition calls in commit `15f4330`. ClaimQuestReward was missed — same defect class as C-V3-2/H-V3-1 (rollback restores to post-mutation state, the stale-flag fallback masks it).

Plus the **detached-thread / 10s drain hazard from V3-H1 is now narrowed but not closed** — H-V3-7's explicit Derived destructor narrows the UAF window enormously (cache + RPC handlers + DB pool are still alive when the endpoint drains), but the 10s hard timeout in `ServiceEndpoint::Stop()` (ServiceEndpoint.hpp:125) still exists. A PBKDF2 verify that runs past 10s during shutdown still races derived destruction. Status: **partial fix; downgrade V3-H1 to M-V4-3**.

Three other MEDIUMS and a handful of LOWs round out the surface. **No CRITICAL items.**

---

## CRITICAL

(None.)

---

## HIGH

### H-V4-1. `HandleClaimQuestReward` calls `TickQuests::Apply` BEFORE `Begin()` — C7-A bypass missed by H-V3-1
**Files:** `Server/Account/src/Handlers/QuestHandlers.hpp:579` (TickQuests call), `:605` (Begin); `Server/Account/src/Quests/TickQuests.hpp:46-130` (Apply mutates m_questStates + m_dirty.quest_ids)
**Status:** **NEW — H-V3-1 scope miss.**

The H-V3-1 fix shipped in commit `15f4330` expanded C7-A's pre-mutation discipline to HandleSetParty, HandleReportQuestProgress, and HandleCompleteQuest. All three correctly relocated `Begin()` above `TickQuests::Apply` / SetParty / quest state transitions. HandleClaimQuestReward — the largest of the quest handlers — was missed:

```cpp
// QuestHandlers.hpp:579
TickQuests::Apply(account, m_ctx.questLoader, std::time(nullptr), playerId);   // mutates!

// ...26 lines of read-only validation + ClaimPreState capture...

// QuestHandlers.hpp:605
auto txn = m_ctx.repository->Begin(account);   // snapshot HERE
```

`TickQuests::Apply` (TickQuests.hpp:46-130):
1. `states.Upsert(def.MakeInitialState())` on newly-eligible quests — mutates m_questStates.
2. `account.MutableDirty().quest_ids.insert(questId)` (line 59) — flips dirty.
3. `states.ExpireStale(now)` + `states.RecycleClaimed(now)` — mutates quest state slots.
4. Reset-stale-active-quests loop — mutates `q->state`, `q->startedAt`, `q->completedAt`, `q->resetAt`, `q->objectives`.
5. Auto-complete loop — mutates `q->state` to Completed.
6. Dirty mark loop (line 126-130) — inserts EVERY quest into `m_dirty.quest_ids` on any tick activity.

All five mutation surfaces land BEFORE the snapshot. The Memento captures the post-tick state; Rollback's `RestoreFrom` restores to the same post-tick state. Identical mechanism to the original C-V3-2 defect.

**Why it's not catastrophic:** the same two converging guards as V3-C1:
1. `TickQuests::Apply` is documented idempotent (TickQuests.hpp:18) — re-running it on a freshly-reloaded Account produces the same state, so the rolled-back-and-restored quest state matches what a clean reload would compute anyway.
2. `MarkStaleForReload` belt-and-suspenders forces a DB reload on next `GetLockedAccount`.

But the C7-A contract is violated. The whole point of the snapshot is that a Commit failure unwinds the in-memory Account to pre-handler state so an unwinding error response can't be built from speculative mutations. On the claim path, the response is built from `q->ToJson()` at handler end — if Commit throws, the destructor's Rollback restores to the post-TickQuests state, the handler-level catch (in `MessageDispatcher::Dispatch`) returns an error to the client, but the in-memory Account has the post-TickQuests state visible to the next handler until stale eviction fires.

**Fix.** Move `Begin()` above the `TickQuests::Apply` call. The intervening code (questStates.Has, q->IsClaimed / IsCompleted / IsExpired, idempotency lookup) is pure read; no transaction leak. Same 4-line mechanical relocation as the H-V3-1 sibling fixes.

---

## MEDIUM

### M-V4-1. ServiceEndpoint detached-thread 10s drain timeout still exists — H-V3-7 narrows the UAF window but doesn't close it
**Files:** `Server/Common/src/Net/ServiceEndpoint.hpp:101` (detach), `:125` (10s `wait_for`), `Server/Account/src/AccountServer.hpp:126` (~AccountServer calls Stop)
**Status:** **Downgrade from V3-H1.** H-V3-7's explicit derived destructor materially narrows the hazard.

V3-H1 flagged: detached connection threads can outlive `AccountCache` because `m_internalEndpoint` lives in `TcpServerBase` (base) and `m_cache` lives in `AccountServer` (derived). H-V3-7's fix — explicit `~AccountServer() { Stop(); }` (verified at AccountServer.hpp:126, AuthServer.hpp:56, CombatServer.hpp:37) — means the derived destructor runs Stop() while every derived member (m_cache, m_repository, m_pool, m_internalRpcHandlers) is still alive. Stop drains the endpoint, OnStopped runs SaveAllAndClear under stripe locks, and only then does derived-class member destruction proceed.

What's still there:
- `ServiceEndpoint::Stop()` waits at most **10 seconds** for `m_activeConnections == 0` (ServiceEndpoint.hpp:125). After the wait_for times out, Stop returns regardless of in-flight detached threads.
- A handler stuck on a slow DB (worst case: ConnectionPool::PoolExhausted throws after the 5s acquire timeout, then the handler unwinds — but PBKDF2 verify can run 200-400ms and a multi-pull holds the stripe lock through its whole body), or stuck in PBKDF2 verify on a cold dummy hash, could outrun the 10s window.
- If that happens, the detached thread eventually returns, decrements `m_activeConnections`, and signals `m_connDrainCv` — but Stop has already returned, the derived destructor proceeds, and `m_cache` / `m_repository` / `m_pool` get destroyed while the detached thread is still mid-handler.

H-V3-7 narrows the practical UAF window from "10s + base-class destruction" to "10s + derived-class destruction." In the common case (PBKDF2 verify ≤ 400ms, DB ≤ 100ms) the drain completes well before the 10s timeout and the window is zero. In the pathological case (a deadlocked or otherwise stuck handler) the UAF still exists.

**Fix.** Two layered options:
1. **Bounded but-uncapped-on-success drain.** Replace the 10s `wait_for` with an unbounded `wait` plus a watchdog timer (e.g. 60s) that LOG_CRITICALs and aborts the process if a stuck handler refuses to exit. Forces operators to investigate stuck handlers rather than masking them.
2. **Join-not-detach.** Track each connection thread in a vector under a mutex; Stop joins them all. Inherits the watchdog requirement (a single stuck handler now blocks the whole shutdown), but eliminates the UAF surface unambiguously.

Either fix is non-trivial and the post-H-V3-7 hazard is narrow. Defer to launch-prep operational work.

### M-V4-2. `LockedAccountRef` stripe held through full multi-pull handler body — false-contention surface on 1/64 stripe collisions
**Files:** `Server/Account/src/Handlers/GachaHandlers.hpp:307-543` (HandleMultiPull body, ~250 lines holding stripeLock)
**Status:** Carry-forward from V3-M2; no fix landed; reaffirmed.

The two-lock protocol is correct, but `LockedAccountRef.stripeLock` is owned for the entire handler body. HandleMultiPull does up to 10 iterations of pull logic (each touching collection / pity / wallet) plus a Commit that now also takes the per-account advisory lock (H-V3-2). A concurrent handler for a same-stripe-but-different-player (1/64 collision rate) blocks behind the whole 200-400ms handler.

The same shape applies to HandleClaimQuestReward (~120 lines under stripe), HandleReportQuestProgress (~80 lines under stripe), and HandlePull (~190 lines under stripe).

**Fix.** Lower priority. Acceptable for current scale (~dozens of concurrent players). Worth a CLAUDE.md note so the next p99-latency investigation has a starting point. A longer-term fix would split the handler body into "pre-commit read-mostly" (no lock needed) and "commit + cache update" (lock held briefly), but C-V3-2's correctness requires the snapshot capture under the stripe lock, so a real fix is architectural.

### M-V4-3. `m_dirty.quest_ids` lazy-tick in idle-evict path can race AccountCache reload — verified safe but contract-only
**Files:** `Server/Account/src/Cache/AccountHydrator.hpp` (load-time TickQuests::Apply), `Server/Account/src/Quests/TickQuests.hpp:46`
**Status:** Verified safe; flagging because the invariant rests on the stripe-lock-during-hydration contract.

AccountHydrator::FromData runs TickQuests::Apply during load (per TickQuests.hpp comment, line 23-26). This mutates m_questStates + m_dirty.quest_ids. AccountCache.hpp:106 calls `AccountHydrator::FromData` OUTSIDE the m_mapMutex but INSIDE the stripe lock (LockedAccountRef holds it). The mutation is visible only to the calling thread until the new Account is inserted into m_accounts under m_mapMutex (line 119).

This is safe because:
- The newly-constructed Account exists only as a local unique_ptr until line 119's emplace.
- No other thread can race a read of this Account until line 119 publishes it under m_mapMutex.
- Once published, future readers see the post-tick state, which is consistent with the post-tick DB state (TickQuests::Apply is idempotent on equivalent input).

But the safety rests on "FromData mutates state only the constructing thread can see, prior to publication via m_accounts.emplace." A future refactor that publishes Account before completing hydration would silently break it.

**Fix.** Add a one-line comment to AccountHydrator::FromData asserting "the returned Account is unpublished until the caller stores it under m_mapMutex; TickQuests's in-place mutation is safe ONLY for this single-thread / pre-publication window."

---

## LOW / OBSERVATION

### L-V4-1. `m_dummyPasswordHash` function-local static is initialized on first call, not at server start
**File:** `Server/Account/src/Handlers/InternalRpcHandlers.hpp:94-95`
The H-V3-10 fix made the dummy hash a function-local static — correct for timing-oracle defense AND init-order independence. Side effect: the FIRST username-miss after server start pays one extra PBKDF2 derivation (~200ms) on top of the dummy compute (~200ms) because the static itself is being initialized. Subsequent misses pay only the dummy compute. The first-miss latency is ~400ms instead of ~200ms; not a security issue (the timing oracle is closed in both cases), but it's a one-time startup-burst tail latency artifact worth a comment.

### L-V4-2. `ConnectionPool::acquire` timeout is 5s; `ServiceEndpoint::Stop` drain timeout is 10s — relationship is implicit
**Files:** `Server/Account/src/Db/ConnectionPool.hpp:29` (5s default), `Server/Common/src/Net/ServiceEndpoint.hpp:125` (10s)
A handler that blocks on `pool.acquire()` will throw PoolExhausted at the 5s mark and unwind; the connection thread will be drained well within ServiceEndpoint::Stop()'s 10s window. The two timeouts have to maintain `acquire_timeout < endpoint_drain` for V3-H1 / M-V4-1 to stay narrow; today they do (5s < 10s). If either is changed (operator tuning the pool timeout up, for example), the V3-H1 hazard widens silently. Add a `static_assert` or runtime check at AccountServer construction that asserts the ordering.

### L-V4-3. `g_rateLimitingEnabled` / `g_authIPBindingEnabled` snapshot semantics inconsistent
**Files:** `Server/Common/src/Net/RateLimiter.hpp:24,62,125` (live atomic read on every call), `Server/Auth/src/AuthServer.hpp:215` (read ONCE at SessionManager construction)
`g_rateLimitingEnabled` is read on every Allow / GetCooldownRemaining call (live), so a runtime toggle propagates instantly. `g_authIPBindingEnabled` is read ONCE at SessionManager construction time and copied into SessionConfig (verified at AuthServer.hpp:215 + comment at L24-30), so a runtime toggle does NOT propagate. The V3-L5 security comment documents this correctly for the IP-binding flag, but the two globals having different semantics is itself a footgun for an operator wiring an admin-toggle endpoint later. No fix today; one-line comment at the rate-limit global stating "live-read; runtime toggle is observed by the next Allow call" would balance the documentation.

### L-V4-4. `LockedAccountRef.stripeLock` is implicitly assumed to outlive `LockedAccountRef.account`
**Files:** `Server/Account/src/Cache/HandlerContext.hpp` (LockedAccountRef struct)
The struct holds a `std::unique_lock<std::mutex> stripeLock` and an `Account* account`. The Account pointer is valid only while the stripe lock is held (no other thread can evict the Account from m_accounts). Member destruction order is reverse declaration order — if a future refactor reorders the fields so `stripeLock` is declared after `account`, the lock would be released before the pointer is "released," leaving a small window where the pointer is dangling. Add a static_assert or comment locking the field order. (Current order is correct.)

### L-V4-5. `AccountCache::SaveAllAndClear` save-while-holding-stripe is serial; parallel-save across stripes could shorten shutdown
**File:** `Server/Account/src/Cache/AccountCache.hpp:217-230`
Carry-forward from V3-M4. Phase 2 holds each stripe lock through the full `m_repository.Save(*account)` call (~100ms / heavy account), serially across all 100+ cached accounts at shutdown — wall-clock 10+ seconds. Could parallelize: stripes are independent, so up to 64 concurrent saves are safe. Low priority for current scale; flag for launch-prep operational work.

### L-V4-6. `OutboxRelay` isn't wired into AccountServer — its existence in the tree is dead code
**File:** `Server/Account/src/Db/OutboxRelay.hpp` (no consumer in AccountServer or main.cpp)
Grep shows zero references outside the file itself. The class is correctly written (joins worker on Stop, uses FOR UPDATE SKIP LOCKED for horizontal-scale safety) but isn't constructed by any service. Either remove from the tree or wire it up — currently it's a maintenance trap for a future engineer who assumes the outbox is being drained.

### L-V4-7. `LruCache` ratelimiter-relationship documentation
**File:** `Server/Common/src/Util/LruCache.hpp` (header comment)
The H-V3-11 fix split LruCache into Peek (no-touch) / Get (touches) / Touch (explicit). The header comment still describes the pre-split API. Reviewer attention is required to spot the inversion-risk on a future consumer; an updated header comment ("Peek for diagnostic reads; Get/Put/Touch update recency; consumer of a rate-limiter shape MUST Peek on rejection") would close the gap.

### L-V4-8. `SessionCache::NoteAuthRecovered` rename
**File:** `Server/Common/src/Net/SessionCache.hpp:209`
Carry-forward from V3-M3. The contract is "caller must not hold m_mutex"; the name doesn't advertise it. Rename to `NoteAuthRecoveredLockFree` to make a misuse a compile-time refactor instead of a production deadlock. Low priority — current call sites are correct and have docstring coverage.

---

## Verified Closed from v3 (by direct code inspection)

| v3 item | Commit | Site verified | Status |
|---|---|---|---|
| C-V3-2 (Begin before GetPity/Guarantee) | `96f0893` | GachaHandlers.hpp:124 + :369 | Closed |
| H-V3-1 (C7-A scope, 3 handlers) | `15f4330` | AccountHandlers.hpp:305 (SetParty), QuestHandlers.hpp:244 (Report), :725 (Complete) | Closed (but missed ClaimQuestReward — see H-V4-1) |
| H-V3-2 (advisory lock in EnsureOpen) | `8a73914` | AccountTransaction.hpp:396 | Closed |
| H-V3-7 (explicit Derived destructor) | `d6d4e12` | AccountServer.hpp:126, AuthServer.hpp:56, CombatServer.hpp:37 | Closed on all 3 subclasses |
| H-V3-8 ([[nodiscard]] LockFor) | `d6d4e12` | AccountCache.hpp:247 | Closed |
| H-V3-9 (try_emplace) | `d6d4e12` | AccountCache.hpp:273 | Closed |
| H-V3-10 (dummy hash function-local static) | `0079c17` | InternalRpcHandlers.hpp:94 | Closed |
| H-V3-11 (RateLimiter LRU inversion → Peek) | `82f8051` | RateLimiter.hpp:77,134 | Closed |
| M-V3-1 (!IsStale guard) | `4b452b0` | AccountCache.hpp:289 | Closed |
| V3-L2 concurrency (m_lastAccess bump doc) | `0f6e12c` | AccountCache.hpp:262-268 | Closed (comment present) |
| V3-L3 concurrency (assert(inserted)) | `0f6e12c` | AccountCache.hpp:126 | Closed (assert present) |
| V3-L4 concurrency (ClearStale defense doc) | `0f6e12c` | AccountCache.hpp:107-113 | Closed (comment present) |
| V3-L5 concurrency (m_stale invariant comment) | `0f6e12c` | Account.hpp:48-53 | Closed (comment present) |
| V3-L7 (static-local secret cache) | `(v2 carry)` | InternalRpcAuth.hpp | Re-verified healthy |
| V3-H1 (detached-thread / 10s drain UAF) | `d6d4e12` (partial) | ServiceEndpoint.hpp:125 unchanged | **Partially closed** — see M-V4-1 |

---

## Suggested triage order

**Today / immediate:**
1. **H-V4-1** — move `Begin()` above `TickQuests::Apply` in HandleClaimQuestReward. ~2-line mechanical relocation; same shape as the H-V3-1 sibling fixes. Closes the last C7-A scope-creep miss.

**Pre-launch operational:**
2. **M-V4-1** — pick one of the two ServiceEndpoint drain fixes (unbounded wait + watchdog, or join-not-detach). Either closes V3-H1's residual UAF window.
3. **L-V4-6** — wire OutboxRelay into AccountServer (and add the H-V3-4 dispatched-row cleanup the relay was designed for) OR delete it from the tree.

**Eventually:**
4. M-V4-2 (handler-body stripe duration) + L-V4-1 through L-V4-8 as documentation / polish.
