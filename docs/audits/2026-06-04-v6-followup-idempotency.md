# v6 followup — Idempotency + Outbox

**Date:** 2026-06-04 (same-day successor to v5 medium-tail remediation arc)
**Scope:** `Server/Common/src/Idempotency/IdempotencyKey.hpp`, `Server/Account/src/Cache/AccountTransaction.hpp` (StoreIdempotency + Commit/Rollback), `Server/Account/src/Cache/AccountRepository.hpp::FindIdempotency`, `Server/Account/src/Db/OutboxRelay.hpp` (PumpOnce / sweeps / Register), `Server/Account/src/Db/EventStore.hpp::AppendIdempotent`, all 8 handler call sites, `schema.sql` idempotency_cache + outbox + audit_log.
**Method:** Single-agent read-only sweep. Read v5 synthesis + v5 idempotency dimension + v5 event-sourcing dimension. Verified every v5 carry-forward item against current HEAD (`47d16f0`), traced batch (k) closure of M-V5-1 event-sourcing, batch (h) closure of M-V5-1 idempotency, and batches (b)/(g) for outbox lifecycle. Focused on the user-named substance items (silent double-execution, lost outbox rows, retry storms, dispatch contract gaps).

---

## Status of v5 items in this dimension

| Item | Status | Evidence |
|---|---|---|
| **M-V5-1 event-sourcing (AppendIdempotent advisory-lock recheck)** | **CLOSED** (batch k, commit `a9bb510`) | `EventStore.hpp:147-159` — recheck tx now calls `AcquireAdvisoryLockInTx` before the SELECT and `tx.commit()` after. Test-only caller (production routes through `AppendInTx`), but the pattern-equivalence note is pinned by `IdempotencyMachineryTest.cpp:18-27`. |
| **H-V5-2 OutboxRelay::Register never called** | **STILL OPEN** (documented as DEFER) | Grep `m_outboxRelay.Register` / `outboxRelay.Register` across `Server/` returns zero production hits — only `OutboxRelayTest.cpp:53,94` (test usage). The DEFER block at `OutboxRelay.hpp:57-86` explicitly annotates this: "dispatch half (PumpOnce → handler) is intentionally dead-by-omission". No startup self-check landed. EffectDispatcher (the only `EmitToOutbox` caller) remains unwired from production handlers — confirmed by grep against `Server/Account/src/Handlers/`. |
| M-V5-4 idempotency (cache-hit emits no audit_log row) | **STILL OPEN** (documented as DEFER) | All 8 cache-hit short-circuits still only `LOG_DATA_DEBUG` (`GachaHandlers.hpp:86,329`; `AccountHandlers.hpp:360`; `QuestHandlers.hpp:573`; `ProgressionHandlers.hpp:148,243,335,425`). DEFER comment at `AccountTransaction.hpp:165-187` documents the forensic-blindness consequence and the ~15 LOC fix sketch. Carry-forward acknowledged. |
| M-V5-2 idempotency (AddCurrency cached payload size) | **STILL OPEN** (documented as DEFER) | `AccountHandlers.hpp:432-449` carries a DEFER block enumerating the ~24-field BuildStatePayload surface. Cached body is still the full StatePayload at `:450`. Acknowledged as zero-impact-pre-launch (only caller is debug hotkey). |
| **M-V5-1 persistence (60 NULL outbox rows)** | **CLOSED** (batch b, commit `635560f`) | Schema ALTERed to NOT NULL (`schema.sql:413-428`); commit message confirms live-DB rows deleted. Outbox tests updated to seed fixture accounts (`OutboxRelayTest.cpp:26-31`). |
| **M-V5-1 idempotency (client `or uuid4()` fallback)** | **CLOSED** (batch h, commit `2d79bf5`) | All 7 NetworkTCP wrappers drop `or uuid4()`; nil = uncached execution server-side. `GachaService.lua:359-369` mints once per gesture at `doPull` entry. |
| L-V5-1 idempotency (schema "every minute" doc drift) | **CLOSED** (batch b) | `schema.sql:476-481` now says "every ~65s" with correct arithmetic + cross-reference to OutboxRelay cadence. |
| M-V5-3 idempotency (single-instance ON CONFLICT contract) | **CLOSED** (batch b) | `AccountTransaction.hpp:317-331` doc-comment now names the single-instance assumption + the `WHERE expires_at < now()` multi-instance fix path. |
| M-V5-3 persistence (created_at refresh on ON CONFLICT) | **CLOSED** (batch b) | Removed `created_at = now()` from the DO UPDATE clause at `AccountTransaction.hpp:332-346`. |

---

## NEW findings

### [H6-1] OutboxRelay startup never validates registered-destination set against expected
**File:** `Server/Account/src/Db/OutboxRelay.hpp:87-90` (Register) — no caller; `Server/Account/src/AccountServer.hpp:236-239` (RegisterHandlers block) — no `m_outboxRelay.Register(...)` line
**Status:** NEW — refinement / promotion of H-V5-2

**Finding:** The v5 audit already flagged H-V5-2 (no Register calls). Today's `OutboxRelay.hpp` carries a DEFER comment explicitly stating the dispatch half is intentionally dead, but the **forward guard suggested in the comment itself** ("a startup self-check in AccountServer::OnStarted that asserts `m_outboxRelay.HandlerCount() == ExpectedDestinations()` would catch a deploy where the Register wasn't kept in lockstep with the EmitToOutbox addition") was **not implemented**.

The DEFER stance is reasonable while EffectDispatcher remains dead-coded and no handler emits, but the contract surface for future handler authors is currently:
- `txn.EmitToOutbox("destination_x", payload)` writes silently
- if no handler is registered, the row sits with `dispatched_at IS NULL` forever
- `PruneDispatchedOutbox` only deletes `dispatched_at IS NOT NULL` rows — undispatched rows accumulate unbounded

There is no in-code mechanism that catches the foot-gun. Three plausible paths land it:
1. A future commit that wires EffectDispatcher into HandlePull/HandleClaimQuestReward (already on the v2/v3 roadmap per audit cross-ref).
2. A telemetry handler that adds `txn.EmitToOutbox("telemetry.event", ...)` and forgets the Register pair.
3. A material-grant handler that wires `inventory.grant_material` and forgets the consumer.

**Impact:** Silent unbounded outbox table growth on the first `EmitToOutbox` after a Register-less deploy. Pre-launch zero impact, but the failure mode is exactly the "lost outbox rows" / "retry-storm storage growth" the directive flagged as substance.

**Fix sketch:** Add a `HandlerCount()` accessor and a first-PumpOnce-iteration log warning when `handlers_.empty() && rows.size() > 0`:
```cpp
// at top of PumpOnce, after the SELECT
if (!rows.empty()) {
    std::lock_guard lk(mtx_);
    if (handlers_.empty()) {
        static std::atomic<bool> warned{false};
        bool expected = false;
        if (warned.compare_exchange_strong(expected, true))
            LOG_DATA_WARN("OutboxRelay: {} pending outbox rows but zero registered destinations — rows will accumulate", rows.size());
    }
}
```
~10 LOC. Logs once per process to avoid log spam if the condition persists.

This is the "dispatch contract gap" the directive named explicitly. Promoting from L/M to H because the substance directive listed dispatch contract gaps as in-scope and the existing DEFER is the only defense.

### [M6-1] Handler-side response-payload cache write order is unsafe-by-convention, not invariant-enforced
**File:** `Server/Account/src/Handlers/AccountHandlers.hpp:450-456` (and the 7 other handlers with the same shape)
**Status:** NEW

**Finding:** The current pattern at every handler is:
```cpp
const std::string responseBody = BuildStatePayload(account);  // L450
txn.AppendEvent(std::move(ev));                                // L455
if (!scopedKey.empty()) txn.StoreIdempotency(scopedKey, responseBody);  // L456
txn.Commit();
```

`BuildStatePayload(account)` reads the post-mutation account state and serializes it into the cached body — this is correct, since the cached payload must reflect the committed state. But the order is convention-only. A future refactor that introduces a `responseBody = Build...(account)` line **above** the wallet mutation (e.g., to share a builder between success and validation-fail paths) would silently cache pre-mutation state. The cached payload would then return T0 (pre-grant) state on retry, but the events table would carry the T1 grant — replay vs. cache divergence.

Today's correctness depends on every handler author reading the doc comment at `AccountTransaction.hpp:114-163` and putting `BuildXPayload` *after* the mutation. There is no structural enforcement.

**Impact:** Future refactor risk. Today: zero (all 8 handlers correctly build payload after mutation). Tomorrow: a junior dev sees the lambda body and "factors out" BuildStatePayload to the top of the handler for readability, and the cached payload now reflects pre-mutation state on retry. The audit log shows T1 (correct), the events table shows T1 (correct), the wallet column shows T1 (correct), but the player's *cached* response on retry shows T0 — client UI displays the pre-grant balance after a network burp.

**Fix sketch:** Either (a) annotate the doc comment with a STRUCT-VIOLATION example showing what NOT to do, OR (b) provide a `txn.StoreIdempotencyDeferred(scoped_key, [&account]{ return BuildStatePayload(account); })` API that captures a lambda evaluated at Commit time after all events are applied. (b) eliminates the foot-gun mechanically. (a) is ~10 LOC of comment; (b) is ~30 LOC of API + capture path. Defer until the second `StorePayload` consumer lands (currently AddCurrency is the only handler that caches a *non-trivial* payload — Pull caches `PullResultToJson` which is built once at handler entry and never re-read).

Lower-stakes than H6-1 because the contract is documented at every site; rated M because the directive listed silent double-execution and this is the cousin (silent T0/T1 divergence on retry).

### [L6-1] AppendIdempotent's recheck still lacks race-driven test coverage
**File:** `Server/Account/src/Db/EventStore.hpp:133-161`; `Server/Account/tests/Integration/IdempotencyMachineryTest.cpp:18-27`
**Status:** NEW — followup to M-V5-1 event-sourcing closure

**Finding:** The batch (k) closure is structurally correct (advisory lock + commit, mirror of `Append`). The test file pins the closure with a NOTE block stating "race-driven coverage of the recheck would require concurrent writers issuing same-key retries in a tight loop — not deterministic in a unit test." This is a fair compromise, but the closure note explicitly defers to "a future soak harness" that does not exist yet. Today: pattern-equivalence review is the only coverage.

**Impact:** If a future refactor of `Append` or the advisory-lock acquisition silently breaks the recheck path's lock semantics, no test catches it. Production impact is bounded because `AppendIdempotent` has no production callers — but if a future caller adopts it, the regression window opens.

**Fix sketch:** Not blocking. When a soak harness lands (cross-cutting test backlog), add a deterministic-enough probe: spawn 4 threads each calling `AppendIdempotent` with the same key, assert exactly one event row lands and the other 3 return without throwing. Loose timing tolerance is acceptable since the contract is "no spurious throws under concurrent same-key retries", not "lock acquired at moment X".

---

## Observations / Lows

- **Audit log on cache-hit (M-V5-4 carry-forward).** Still unactioned. The forensic-blindness DEFER comment at `AccountTransaction.hpp:165-187` is the documented stance; the fix sketch is in-place. Per directive, not elevating — but noting that the first production retry-investigation ticket trips it.
- **AddCurrency cached payload size (M-V5-2 carry-forward).** Still ~24-field BuildStatePayload. The DEFER comment at `AccountHandlers.hpp:432-449` correctly bounds the risk to "after IAP/quest payouts wire through AddCurrency". Per directive, not elevating.
- **Sibling-key collision test for the 8 prefixes** is well-pinned at `IdempotencyMachineryTest.cpp:142-164`. Adding a 9th handler is mechanically forced through that fixture — strong forward guard.
- **Commit failure semantics.** Verified: if `Commit()` throws before `tx_->commit()`, the destructor's Rollback path discards the snapshot's pre-tx state and marks the account stale (`AccountTransaction.hpp:396-446`). The `idempotency_cache` row is inside the same `pqxx::work` as the events + audit + outbox, so a failed commit leaves no cache row — a retry of a failed commit correctly executes again. This is the correct safety property; cache-vs-events durability is atomic.
- **Different-idempotency-key retry on a successful original.** Handler executes again, events table gets a second row, audit_log gets a second row, wallet gets a second grant. Correct per contract (client said "this is a new intent"). No silent double-execution because the client is the source of truth on intent disambiguation.
- **24h TTL vs mobile suspend (L-V4-2 carry-forward).** Still appropriate. Sweeper actually runs now (C-V4-1 + M-V5-1 ES closures); the table doesn't grow unboundedly.
- **Sweep cadence verification.** `OutboxRelay.hpp:164-166` constants: prune=120 pumps (60s), idempotency=130 pumps (65s), partman=7200 pumps (1h). Schema doc and audit-doc agree. Anti-stampede drift is intentional. All three sweeps are now public + directly testable per H-V5-2 test-coverage closure.

---

## Substance summary (per directive)

Substance items the directive named:
- **Silent double-execution:** No new surface found. Cache-hit short-circuit is structurally correct, idempotency_cache insert is atomic with events. M6-1 is the only conventionally-enforced surface (post-mutation payload build).
- **Lost outbox rows:** H6-1 is the live concern — Register-less deploys with EmitToOutbox callers would silently accumulate rows. DEFER comment is the only defense.
- **Retry-storm storage growth:** Both sweeps (`PruneDispatchedOutbox`, `SweepExpiredIdempotency`) are running, public, and correctly index-backed. The undispatched-outbox case is the only unbounded surface (H6-1).
- **Dispatch contract gaps:** H6-1 directly. The contract is documented in OutboxRelay.hpp's DEFER block but not enforced at deploy time.

Pre-launch operational items, style nits, "should test more" items: explicitly skipped per directive.
