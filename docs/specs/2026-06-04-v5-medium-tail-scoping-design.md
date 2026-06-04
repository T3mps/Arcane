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
| 4 | Pull / MultiPull stripe-lock duration | concurrency M-V5-4 | ~80-120 | Moderate | 1 |

**Implementation order is independent** — each scope touches disjoint files, no ordering constraint. Reasonable batch sequence: 2 → 3 → 1 → 4 (small to large LOC).

**Tests:** all four are paired with pinned regression tests where deterministically testable; the MultiPull lock-duration win is verified by review (no perf harness exists pre-launch).

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

## Scope 4 — Pull / MultiPull stripe-lock duration (concurrency M-V5-4)

### Problem

`HandleMultiPull` (`GachaHandlers.hpp:307-543`) holds the stripe lock through ~240 LOC including:

- **Must be locked** (mutates Account state or DB):
  - RNG advance (up to 10 pulls via `GachaRNG::Roll`)
  - Reducer apply (pity, guarantee, wallet)
  - `AccountTransaction::Commit` → events flush → outbox → audit_log
  - DB commit (~50-150ms under pq round-trip — the long pole)
- **Does NOT need the lock** (CPU work on already-resolved data):
  - Response payload build (JSON encoding of every pull result)
  - Outbox payload formatting (stringification of currency deltas)
  - String concatenation for log lines

At 1/64 stripe-collision probability, two simultaneous 10-pulls by players who hash to the same stripe serialize through the entire 240-LOC body. The "must be locked" part is ~80-120 LOC; the rest could move outside.

`HandlePull` (single-pull) carries the same shape with smaller numbers. The audit named only `HandleMultiPull` but both are restructured here — keeping their lock patterns symmetric prevents a future audit flagging the inconsistency.

### Approach — snapshot pattern

**Two-phase restructure within `HandleMultiPull` and `HandlePull`:**

```cpp
// === LOCKED PHASE (stripe lock held) ===
LockedAccountRef ref = cache.GetLockedAccount(playerId);
Account& account = *ref.account;

std::vector<PullResult> results;
results.reserve(count);
for (int i = 0; i < count; ++i) {
    results.push_back(SimulateOnePull(account, banner));  // mutates pity/RNG
}

auto txn = repo.Begin(account);
txn.AppendEvents(EventsFor(results));
txn.Commit();  // DB commit — stays under lock

// Snapshot post-mutation values for the response BEFORE releasing lock.
// By-value copies; the response builder consumes them without re-reading
// Account state.
auto walletSnapshot = account.GetWallet();
auto pitySnapshot   = account.GetPityState(slotId);
// ... (any other field the response payload reads from `account`)

// === LOCK RELEASES at end of scope ===
}

// === UNLOCKED PHASE: response prep + serialization ===
Json responseBody = BuildResponsePayload(results, walletSnapshot, pitySnapshot);
return CreateResponse(MsgType, responseBody.dump());
```

**Key invariant:** never capture `account` by reference into the unlocked phase. Every value the response payload reads from Account state must be snapshotted (by value) inside the lock. A compile-time guard: extract `BuildResponsePayload(results, walletSnapshot, pitySnapshot, ...)` into a free function whose signature takes the snapshot types directly — the function can't accidentally read `account` because it doesn't have one.

**Keep the DB commit inside the lock.** A more aggressive optimization would release the lock before `txn.Commit()`. Risk: if commit fails, in-memory Account state has been mutated and a concurrent reader could observe post-mutation state while DB still has pre-mutation state. Rollback-snapshot handles in-process recovery but cross-request visibility leaks briefly. Modest latency win not worth the correctness risk.

### Apply same restructure to HandlePull

`HandlePull` has the same lock-around-everything shape with `count=1`. Apply the same two-phase pattern:

- Locked phase: 1 SimulateOnePull + txn.AppendEvents + txn.Commit + snapshot
- Unlocked phase: BuildResponsePayload + serialize

Smaller absolute win (single-pull is faster than 10-pull), but keeps the lock pattern consistent across the two handlers. A future audit comparing the two would re-flag any asymmetry.

### Files touched

- `Server/Account/src/Handlers/GachaHandlers.hpp` — both `HandlePull` (single-pull) and `HandleMultiPull` (10-pull)
- Possibly extract a free function or static helper `BuildPullResponsePayload(results, walletSnapshot, pitySnapshot, ...)` into the same file or a sibling header if the body becomes substantial. Reuse between the two handlers if shapes are identical; separate functions if they diverge.

### Tests

- Existing `HandlePull` / `HandleMultiPull` integration tests stay green — observable behavior is unchanged.
- Add a focused regression test asserting the response payload's `wallet.tickets` value equals the post-pull wallet (regression guard for "snapshot captured the right field"). Pins snapshot correctness without needing concurrent threads.
- No perf test. The lock-duration win is verified by code review (the diff makes the change visible). A microbenchmark belongs in a perf harness when one exists.

### Risk

Moderate. The most invasive of the four — touching the highest-traffic handler in the gacha-loop hot path. Two specific risks:

1. **Snapshot completeness.** Forgetting to snapshot a field the response payload needs would either (a) compile-fail (good — caught at edit time) or (b) compile-pass and read live Account state via a captured reference (bad — torn response). Mitigation: extract `BuildResponsePayload` to a free function whose signature only accepts snapshot types — the captured-reference path becomes syntactically impossible.
2. **Symmetry maintenance.** `HandlePull` and `HandleMultiPull` restructured together; future edits must preserve symmetry. Cross-reference comments in both handlers naming the other.

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

Scopes are independent. Recommended sequence by ascending LOC / risk:

1. **Scope 2** (~15 LOC, low risk) — quick win, establishes the constant-pattern for Scope 1 to mirror
2. **Scope 3** (~5 LOC, low risk) — single-file change, low blast radius
3. **Scope 1** (~70 LOC, low risk) — touches 6 files but each change is small and consistent
4. **Scope 4** (~80-120 LOC, moderate risk) — most invasive; do last so the others' risk-budget is clear before touching the hot path

Each scope is its own commit (matching the `chore: v5 medium batch (X) — ...` pattern from prior batches a-i).

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
