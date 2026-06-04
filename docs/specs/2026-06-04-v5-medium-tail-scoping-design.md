# v5 Medium-Tier Tail — Deployment-Readiness Scoping

**Date:** 2026-06-04
**Status:** Design (pre-implementation)
**Author:** v5 audit-arc collaboration session (Ethan Temprovich + Claude Opus 4.7)

## Background

The v5 medium-tier sweep (audit `2026-06-03-server-persistence-audit-v5.md` and its 10 per-dimension followups) closed 38 medium items across the v4 remediation arc. After eight committed batches (a-i, commits `34c72c9..8375f6c`), four items remain that all share a "pre-launch ops surface" thesis — they don't change feature behavior but determine how the system holds up at deploy time and under early load. Each was deferred from earlier batches because it required either a deployment-topology decision (NAT, loopback) or a non-trivial restructure of an existing correct-but-suboptimal path (advisory lock, stripe-lock duration).

This design specifies the four work items, the rationale for the chosen approach, and what's explicitly out of scope.

## Scope Overview

| # | Item | Audit ID | LOC | Risk | Files |
|---|---|---|---|---|---|
| 1 | NAT cap configurability + log throttle | M-V5-6 net / M-V5-1 sec | ~70 | Low | 6 |
| 2 | ServiceEndpoint loopback caps | M-V5-7 net | ~15 | Low | 1 |
| 3 | Advisory lock in AppendIdempotent recheck | event-sourcing M-V5-1 | ~5 | Low | 1 |
| 4 | Per-account locks (replace 1/64 stripe false-contention) | concurrency M-V5-4 | ~150-250 | **High** | 4-6 |

**Implementation order is independent for 1-3** — each touches disjoint files. **Scope 4 is structurally separate** and the largest-risk item in this design; recommended to ship in its own dedicated batch after 1-3 are merged.

**Tests:** scopes 1-3 paired with pinned regression tests where deterministically testable. Scope 4 needs both new unit tests for the cache machinery AND a regression-guard pass against every existing handler test (any path through the cache must still produce a consistent Account view).

**Spec self-review surfaced:** the original Scope 4 design (snapshot pattern to move response build outside the stripe lock) contradicts the documented idempotency-atomicity invariant at `GachaHandlers.hpp:270-273` — the response payload IS what gets buffered into `idempotency_cache.response_payload` during Commit, and a retried call must return that exact buffered byte sequence. The "obvious" lock-duration optimization is therefore wrong. The real fix to the 1/64 false-contention is to eliminate stripe collisions entirely via per-account locks.

---

## Scope 1 — NAT cap configurability + log throttle (M-V5-6 networking / M-V5-1 security)

### Problem

`ServerConfig::MAX_CONNECTIONS_PER_IP=16` and `MAX_CONNECTIONS_TOTAL=2048` are `constexpr` in `Server/Common/src/Net/TcpSocket.hpp:43-49`. Three forward-compatibility hazards:

1. **CGNAT / shared-NAT collision.** A 1000-player school, dorm, or carrier CGNAT egress shares one source IP. At per-IP cap 16, the 17th legit user is refused indistinguishably from a slow-loris attacker. The cap is correct for the slow-loris threat model but misfires under shared-NAT.
2. **Reverse-proxy collapse.** If Aphelyon ever sits behind a TLS-terminating proxy (Nginx, Envoy, CloudFront), 100% of accept calls return the proxy's IP. The cap immediately fires for everyone.
3. **Log storm on refusal.** `LOG_NET_WARN("Accept refused: per-IP cap {} reached for {}", ...)` fires once per refused TCP SYN. Under a CGNAT-blocked population, that's one WARN per refused connect, sustained at the network's natural retry cadence (browsers, mobile clients, Steam-style relaunchers).

The launch-day topology is not pinned. The fix should leave the topology choice open.

### Approach — three pieces

**1. Configurable caps via `protocol.json` settings.**

`Server/data/protocol.json` already carries a `settings` block (e.g., `max_message_size`). Add:

```json
"settings": {
    "max_message_size": 8192,
    "max_connections_per_ip": 16,
    "max_connections_total": 2048
}
```

`Server/Common/src/Net/Protocol.hpp`'s `Settings` struct gains the two fields; `ProtocolLoader::GetSettings()` exposes them. `TcpServerBase`'s ctor takes optional `maxConnPerIp` / `maxConnTotal` args defaulting to `ServerConfig::MAX_CONNECTIONS_PER_IP` / `MAX_CONNECTIONS_TOTAL` for backward compat. Each service's `main.cpp` reads from the loader and passes through:

```cpp
const auto& settings = ProtocolLoader::Instance().GetSettings();
AccountServer server(clientPort, /*authInternalPort=*/7770,
                     /*internalPort=*/7773, dbConn,
                     /*maxConnPerIp=*/settings.maxConnectionsPerIp,
                     /*maxConnTotal=*/settings.maxConnectionsTotal);
```

A `protocol.json` without the new keys uses the existing constants — no behavior change at default values.

**2. Per-IP refusal log throttle.**

Add `std::unordered_map<std::string, steady_clock::time_point> m_lastRefusalLog` guarded by `m_clientsMutex`. Emit the WARN only when `now - last >= 1s` for that IP. Drop suppressed messages — the rate-limit IS the signal that the IP is hitting cap; counting suppressed lines adds noise. Erase the map entry when `m_clientsByIp` erases (already happens in HandleClient's disconnect block per M-V5-3).

**3. Doc the proxy upgrade path.**

A comment block in `TcpSocket.hpp` adjacent to the constants enumerates the topology assumptions (no reverse proxy, no CDN, no PROXY-protocol) and names the upgrade approach: "If a TLS-terminating front lands, parse PROXY-protocol v1 in the accept handshake before perIpCount is incremented; the `clientIP` variable becomes the X-Forwarded-For value, not the immediate peer."

### Files touched

- `Server/Common/src/Net/Protocol.hpp` — Settings struct fields + loader accessor
- `Server/Common/src/Net/TcpServerBase.hpp` — ctor args, log throttle, member additions
- `Server/Common/src/Net/TcpSocket.hpp` — doc comment near constants
- `Server/data/protocol.json` — explicit defaults for the two keys
- `Server/Auth/src/main.cpp`, `Server/Account/src/main.cpp`, `Server/Combat/src/main.cpp` — read settings → pass to server ctor

### Tests

- Extend `Server/Common/tests/TcpServerCapsTest.cpp` with a constructor-supplied small per-IP cap (e.g., 3) and verify the 4th refused. Pins the configurable path.
- Existing 2 cap tests stay green (defaults preserved).

### Risk

Tiny. Defaults reproduce current behavior bit-for-bit. The only behavior change at default values is the throttled WARN (fires once per IP per second instead of once per SYN), which is unambiguously an improvement.

---

## Scope 2 — ServiceEndpoint loopback caps (M-V5-7 networking)

### Problem

`ServiceEndpoint::Start`'s accept loop (`ServiceEndpoint.hpp:120-166`) spawns one detached thread per accepted loopback connection. No total cap, no per-IP cap (degenerate on loopback — every peer is 127.0.0.1). A Debug-build dev running a tool that connect-and-drops to `127.0.0.1:<internal_port>` in a tight loop spawns unbounded ~1.1MB-stack threads until OS thread limits or RAM exhaustion. Production exposure is bounded by the loopback-only mitigation (the listener binds to 127.0.0.1, not 0.0.0.0), but the inconsistency with `TcpServerBase`'s capped accept loop is the kind of "why is internal less hardened than external?" finding that surfaces in security review.

### Approach

Single counter + single ceiling. Add `static constexpr int kMaxInternalConnections = 64;` next to the existing `kDrainTimeoutSeconds`. Before incrementing `m_activeConnections` and spawning the detached connection thread, check:

```cpp
if (m_activeConnections.load(std::memory_order_relaxed) >= kMaxInternalConnections) {
    LOG_NET_WARN("ServiceEndpoint: refused loopback connect — cap {} reached (port {})",
                 kMaxInternalConnections, m_port);
    CloseSocket(clientSocket);
    continue;
}
```

The check is *outside* the `fetch_add`, so a brief race between two simultaneous accepts could let through one extra. Acceptable — 64+1 is still bounded.

No per-IP tracking (every loopback IP is 127.0.0.1). No log throttle (loopback refusal volume is dev-scoped, not user-scoped — a flood IS the signal a tool is misbehaving).

### Files touched

- `Server/Common/src/Net/ServiceEndpoint.hpp` — constant + check in accept loop

### Tests

- New `Server/Common/tests/ServiceEndpointCapsTest.cpp` in CommonTests. Stub a `ServiceEndpoint` on `127.0.0.1:0`, open `kMaxInternalConnections` connections, verify the (cap+1)th gets refused via recv returning 0 within ~1s. Same shape as `TcpServerCapsTest`'s per-IP test.

### Risk

Tiny. 64 is generous for the current 1-Auth ↔ 1-Account ↔ 1-Combat topology where steady-state internal connection count is ≤ 6. The cap fires only on pathological load.

### Future note

If/when metrics infra lands, export `m_activeConnections` so an operational dashboard shows internal-RPC saturation. Pre-launch this is design-doc-only.

---

## Scope 3 — Advisory lock in AppendIdempotent recheck (event-sourcing M-V5-1)

### Problem

`EventStore::AppendIdempotent` (`EventStore.hpp:133-145`) catches `ConcurrencyConflict` from the inner `Append(ev)` call and opens a **second** `pqxx::work` to check whether the conflict was caused by an idempotency-key duplicate (the safe outcome — swallow and return) vs a genuine version race (re-throw). The original `Append` acquires the per-account advisory lock at `EventStore.hpp:118` before `AppendInTx`, so its tx is serialized. The recheck tx does NOT — `pqxx::work tx(*lease)` at L139 with no `AcquireAdvisoryLockInTx` call.

**Race window:**

```
Writer A: Append(ev{key=K, version=v}) → ConcurrencyConflict (lock released on throw)
                                          ↓
Writer B: Append(ev{key=K, version=v'}) → succeeds (no lock contention)
                                          ↓
Writer A: catch → opens 2nd tx → SELECT 1 WHERE idempotency_key=K → finds B's row
                                          ↓
                                          ↓ swallowed as "successful idempotency hit"
                                          ↓ return without re-throw
```

A's caller now believes A's event landed when actually B's event landed. The bug surfaces post-hoc as "the wallet flush succeeded but the event was a duplicate of someone else's retry." Pre-launch we have no concurrent traffic; post-launch this is "1-in-N retries silently writes wrong state" with N depending on retry rate.

### Approach

Wrap the recheck tx in the same `AcquireAdvisoryLockInTx(tx, ev.account_id)` call the original `Append` uses. ~5 LOC:

```cpp
catch (const ConcurrencyConflict&) {
    auto lease = pool_.acquire();
    pqxx::work tx(*lease);
    // Audit M-V5-1 event-sourcing (2026-06-04): the recheck must hold
    // the per-account advisory lock so a concurrent writer can't land
    // a row with the same idempotency_key between the throw and the
    // SELECT 1. Without the lock, a genuine version-collision retry
    // race could be silently swallowed as an idempotency hit.
    AcquireAdvisoryLockInTx(tx, ev.account_id);
    auto r = tx.exec("SELECT 1 FROM events WHERE ...", ...);
    if (r.empty()) throw;
    tx.commit();  // release the advisory lock cleanly
}
```

The `tx.commit()` is new — the existing code didn't commit because the SELECT was a pure read. Adding the commit releases the advisory lock promptly; without it the lock would hold until the connection returns to the pool and pqxx destructs the work, which is correct but unnecessarily long.

### Files touched

- `Server/Account/src/Db/EventStore.hpp` — the catch block

### Tests

Race testing isn't deterministic in unit tests. The change is structurally equivalent to `Append`'s already-audited lock+commit pattern; an `// audit:M-V5-1 event-sourcing` comment-tag in the catch block + a NOTE in `IdempotencyMachineryTest.cpp` flagging the recheck as covered-by-pattern-equivalence is the correct level of coverage. A load-driven concurrent-retry test belongs in a future soak harness, not here.

### Risk

Low. Single lock-acquire + single commit on a path that's already on a fresh tx. The only behavior change is "we now wait for the lock when previously we did a lock-free read" — but that's the bug.

---

## Scope 4 — Per-account locks (replace stripe-lock 1/64 false-contention) — concurrency M-V5-4

### Problem

`AccountCache` uses `StripedMutex<64>` (`StripedMutex.hpp`) for per-player serialization: handlers acquire the stripe via `m_playerLocks.LockFor(playerId)` and hold it for the entire handler duration (`AccountCache.hpp:64`, `HandlerContext.hpp:28-34`). Two players whose `hash(playerId) % 64` collides serialize their handlers even though they're touching unrelated state. At any given moment, ~1.6% of concurrent player pairs experience false contention.

In `HandleMultiPull` (`GachaHandlers.hpp:307-543`), the stripe lock is held through ~240 LOC including the DB commit (~50-150ms under pq round-trip). At meaningful concurrent load, the false-contention rate × handler duration becomes throughput-limiting.

**Why the "snapshot pattern" alternative was abandoned:** `GachaHandlers.hpp:270-273` carries a documented invariant that the response payload IS the value buffered into `idempotency_cache.response_payload` during `txn.StoreIdempotency` + `txn.Commit`. A retried call must return that exact byte sequence. Moving response build outside the lock would either cache a stale projection (breaks retry contract) or store a different value than is returned (breaks retry contract differently). The lock is wide for a correctness reason; the only honest fix to the false-contention class is to **eliminate stripe collisions entirely**.

### Approach — hybrid stripe-for-load + per-account-for-handler

Two-phase locking with a stripe lock for the get-or-load critical section and a per-account mutex for the long-held handler-scope lock.

**1. Account gains a handler-scope mutex.**

```cpp
class Account {
    // Audit M-V5-4 concurrency (2026-06-04): per-account mutex
    // replaces the per-stripe mutex for handler-scope serialization.
    // Held by LockedAccountRef for the full handler duration; only
    // GetLockedAccount's brief get-or-load critical section touches
    // the stripe lock.
    mutable std::mutex m_handlerMutex;
    ...
};
```

**2. AccountCache map promotes to `shared_ptr<Account>`.**

Today: `std::unordered_map<std::string, std::unique_ptr<Account>> m_accounts;`
After: `std::unordered_map<std::string, std::shared_ptr<Account>> m_accounts;`

Reason: with per-account locks, a handler holds the lock through I/O while the cleanup thread may evict the entry from the map. The `shared_ptr` keeps the Account alive (and its embedded mutex valid) until the handler's `LockedAccountRef` destructs. Without this, the cleanup thread could erase a unique_ptr<Account> while the handler still holds the lock → use-after-free.

**3. LockedAccountRef changes shape (field order matters).**

```cpp
struct LockedAccountRef {
    // Audit M-V5-4 concurrency (2026-06-04): field order is load-bearing.
    // accountLock declared SECOND, so it destructs FIRST (reverse-order
    // destruction). The lock releases before the shared_ptr's refcount
    // decrement, so the Account is guaranteed alive while another thread
    // could acquire the same mutex. If the shared_ptr destructs first
    // and triggers ~Account, a racing acquirer of the (now-dangling)
    // mutex would crash.
    std::shared_ptr<Account>     account;
    std::unique_lock<std::mutex> accountLock;
    std::string                  error;
};
```

Add a `static_assert` (or compile-time check) verifying the field order at compile time.

**4. GetLockedAccount refactored.**

```cpp
LockedAccountRef GetLockedAccount(const std::string& playerId) {
    LockedAccountRef ref;
    
    // Phase 1: get-or-load under stripe lock (brief).
    std::shared_ptr<Account> acctPtr;
    {
        auto stripeLock = m_playerLocks.LockFor(playerId);  // RAII, released at scope end
        std::lock_guard<std::mutex> mapLock(m_mapMutex);
        m_pendingCleanup.erase(playerId);
        m_lastAccess[playerId] = std::chrono::steady_clock::now();
        auto it = m_accounts.find(playerId);
        if (it != m_accounts.end() && it->second && !it->second->IsStale()) {
            acctPtr = it->second;  // shared_ptr copy bumps refcount
        } else {
            // load-from-DB path; same shape as today, but emplaces
            // shared_ptr<Account> instead of unique_ptr<Account>
            ...
        }
    }  // stripe lock + map lock release here
    
    if (!acctPtr) { ref.error = "Account not found"; return ref; }
    
    // Phase 2: acquire per-account handler lock (held for handler scope).
    ref.accountLock = std::unique_lock<std::mutex>(acctPtr->m_handlerMutex);
    ref.account     = std::move(acctPtr);
    return ref;
}
```

**Stripe lock is now only for "find-or-load-then-bump-refcount"** — typically nanoseconds. The per-account lock is what serializes concurrent same-player handlers. Different-player handlers never serialize.

**5. CleanupIdleAccounts coordinates with per-account locks.**

The existing two-phase snapshot+evict pattern stays, with one change: the per-candidate eviction must acquire the per-account `m_handlerMutex` via `try_lock` to detect "a handler is mid-call" — if contended, skip this iteration; the candidate gets re-considered next sweep. The `shared_ptr` keeps the Account alive even if the map entry is erased while a handler holds the lock — the handler's local `shared_ptr<Account>` keeps the object live until its `LockedAccountRef` destructs.

### Why this preserves the stripe-lock's safety invariants

The original stripe-lock design defends against two distinct races:
1. **Same-player handler serialization** — two concurrent requests for the same playerId must serialize so the wallet/pity/reducer state isn't torn.
2. **Get-or-load atomicity** — a fresh request for an unloaded player triggers a DB load + insert; a racing concurrent request must not load+insert a *second* instance.

The hybrid design covers both:
- (1) by per-account lock — handlers for the same account always serialize on the *same* mutex (since both `GetLockedAccount` calls find the same `shared_ptr<Account>`).
- (2) by the stripe lock held during the get-or-load phase — two racing first-loads for the same player hash to the same stripe, so the second waits, finds the first's insert, copies the shared_ptr, and acquires the same per-account lock the first one is holding.

### Files touched

- `Server/Account/src/State/Account.hpp` — add `m_handlerMutex` member; thread through `Snapshot` mechanism (the mutex shouldn't participate in snapshot/rollback; verify the X-macro doesn't include it)
- `Server/Account/src/Cache/AccountCache.hpp` — map type change + `GetLockedAccount` restructure + `CleanupIdleAccounts` coordination
- `Server/Account/src/Cache/HandlerContext.hpp` — `LockedAccountRef` field order + type change + static_assert
- `Server/Account/src/Cache/AccountHydrator.hpp` — return type may change from `std::unique_ptr<Account>` to `std::shared_ptr<Account>` depending on cache's emplace pattern
- `Server/Account/src/Cache/AccountRepository.hpp` — `Save` takes `Account&` today, so no change unless we change the call shape
- Existing handler call sites all use `lockedRef.account->Foo()` or `*lockedRef.account` — both work transparently with `shared_ptr<Account>` (operator-> and operator* match the raw-pointer pattern). No handler changes expected.

### Tests

- **New AccountCache unit tests:** explicit two-thread tests asserting that (a) two concurrent `GetLockedAccount` calls for the *same* playerId serialize on the per-account lock (the second waits for the first to release), and (b) two concurrent calls for *different* playerIds DON'T serialize (the second proceeds immediately). The first test is structural — pins the per-player serialization invariant. The second is the actual win — pins no-cross-account-contention.
- **Eviction race test:** thread A acquires LockedAccountRef, thread B triggers CleanupIdleAccounts. Assert B skips A's account, A's handler completes successfully, A's account is no longer in the cache after A releases (or, if A's release happens during B's sweep, the next sweep evicts cleanly).
- **All existing handler integration tests stay green** — observable behavior is unchanged.
- **Existing `AccountCacheTest.cpp` cases** (stale-flag round-trip, etc.) need a careful re-read; some may be coupled to the unique_ptr/stripe-lock shape.

### Risk — High

This is the most invasive change of the four. Specific risks:

1. **UAF on field-order regression.** A future edit that flips the `LockedAccountRef` field order causes ~Account to run while another thread acquires the just-released lock. Mitigation: `static_assert` on the field offsets at the struct definition (or a unit test that asserts destruction order via instrumentation).
2. **Stale-flag interaction.** The current `IsStale()` check at `AccountCache.hpp:80-89` causes a stale Account to be evicted under the stripe lock and reloaded. With per-account locks, if thread A holds the handler lock on a stale Account, thread B's `GetLockedAccount` sees the stale flag — but A still has the shared_ptr. B's reload creates a fresh Account, and B's `GetLockedAccount` returns the NEW account. Now A is mid-handler on the OLD account, holding a lock on a mutex that's about to be destroyed when A's LockedAccountRef destructs. Need to verify: does the old Account's handlerMutex destruct cleanly because A holds the only remaining shared_ptr? Yes — A's lock is on the OLD mutex, B's lock is on the NEW mutex, they're independent. The OLD Account dies when A's shared_ptr decrements. Safe but subtle; should be explicitly tested.
3. **Cache map mutation invariants.** `m_accounts` is now `unordered_map<string, shared_ptr<Account>>`. The shared_ptr type carries thread-safe refcount, but the map itself is not thread-safe; `m_mapMutex` still guards iteration/insert/erase. The handler-held `shared_ptr<Account>` is its own pinning mechanism, independent of `m_accounts`.
4. **AccountTransaction interaction with the new lock shape.** AccountTransaction reads/writes from `Account&` references. As long as the LockedAccountRef stays alive through `txn.Commit()` (which it does today — the handler keeps it in scope), the transaction sees consistent state. Verify no path destructs LockedAccountRef before the transaction completes.
5. **Snapshot mechanism (X-macro).** The X-macro at `Account.hpp:117-148` captures Account's mutable state for Rollback. The new `m_handlerMutex` MUST NOT be part of the snapshot. Verify the X-macro doesn't include it; if needed, mark it `mutable` and document it as "not snapshotted; lifetime is handler-scope, not Account-scope."

### Implementation order within Scope 4

Sub-batched to bound the diff per commit:

1. **Promote `m_accounts` to `shared_ptr<Account>` with stripe-lock still active.** Backward-compatible: handler shape unchanged, cache shape changed but lock pattern same. Tests stay green.
2. **Add `m_handlerMutex` to Account, threaded through hydration without yet using it.** Account compiles, mutex exists but unused.
3. **Refactor `GetLockedAccount` to the hybrid pattern (stripe brief, per-account held).** Handler lock acquisition shifts; this is the behavior change.
4. **Refactor `CleanupIdleAccounts` to try-lock the per-account mutex.** Eviction race-safe under the new lock pattern.
5. **Remove the stripe-lock-as-handler-duration code path.** `StripedMutex<64>`'s remaining role is just brief load-path serialization.
6. **Add the new unit tests** (concurrent-same-player, concurrent-different-player, eviction-race).

Each sub-batch is its own commit; the entire Scope 4 series is ~5-6 commits.

---

## Out of scope (explicit defer rationale)

These v5 mediums are **not** in this design and the rationale for deferral is pinned here so future audits don't re-discover them:

| Item | Why deferred |
|---|---|
| **Persistence M-V5-4** (events.idempotency_key UNIQUE partition-scoped) | By design. The cross-partition dedup is `idempotency_cache (account_id, scoped_key)`, not the events UNIQUE. The partition-scoped UNIQUE is the inside-partition fast path; the cache backs cross-partition uniqueness. No fix needed; existing documentation suffices. |
| **Persistence M-V5-5** (public_uid_seq overflow monitoring) | Post-launch metric. Pre-launch player count is 0; the 100M MAXVALUE is years off. Add to launch-readiness checklist; out of code scope. |
| **Networking proxy plumbing** (PROXY-protocol v1, X-Forwarded-For) | Topology not chosen; building plumbing for a guess risks rewriting when the actual proxy is picked. Scope 1's config knob plus the doc-comment upgrade path leave the option open without committing code. |
| **Networking M-V5-1 / M-V5-3** (server-side timeout / probe classification completeness) | Already deferred in `bc05aee` annotation pass with rationale at the call sites; revisit triggers documented inline. |
| **Concurrency M-V5-1** (OutboxRelay worker thread member-init order) | Tied to deferred H-V5-1 OutboxRelay::Register contract (annotated in `bc05aee`). Same wiring decision; fix together when first `txn.EmitToOutbox(...)` site lands. |
| **Idempotency M-V5-2** (AddCurrency BuildStatePayload caching) | Behavioral change; deferred to IAP integration spec where the AddCurrency consumer roadmap firms up. |

## Implementation order

**Scopes 1-3 are independent** (touch disjoint files); ship in ascending LOC / risk order:

1. **Scope 2** (~15 LOC, low risk) — quick win, establishes the constant-pattern for Scope 1 to mirror
2. **Scope 3** (~5 LOC, low risk) — single-file change, low blast radius
3. **Scope 1** (~70 LOC, low risk) — touches 6 files but each change is small and consistent

**Scope 4 ships separately** as its own batch — substantially higher risk than 1-3, larger LOC, and 5-6 sub-commits internally. Do *after* 1-3 are merged so the risk budget is clear before touching the cache foundation.

Each scope (and each Scope 4 sub-batch) is its own commit (matching the `chore: v5 medium batch (X) — ...` pattern from prior batches a-i).

## Open questions / future work

None blocking. Future-work parking lot:

- **Metrics export** for `m_activeConnections`, `m_clientsByIp.size()`, advisory-lock acquire latency — pre-launch we have no metrics infra; rebuild on instrumentation later.
- **Soak / perf harness** for the lock-duration win and concurrent-retry advisory-lock test — both want a load harness that doesn't exist yet. File items in the launch-readiness checklist.
- **Proxy-mode (PROXY-protocol v1)** — defer until topology is chosen.

---

## Audit references

- `docs/superpowers/audits/2026-06-03-server-persistence-audit-v5.md` (synthesis)
- `docs/superpowers/audits/2026-06-03-v5-followup-networking.md` (M-V5-6, M-V5-7)
- `docs/superpowers/audits/2026-06-03-v5-followup-security.md` (M-V5-1)
- `docs/superpowers/audits/2026-06-03-v5-followup-event-sourcing.md` (M-V5-1)
- `docs/superpowers/audits/2026-06-03-v5-followup-concurrency.md` (M-V5-4)
