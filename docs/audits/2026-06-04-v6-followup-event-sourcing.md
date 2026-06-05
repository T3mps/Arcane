# v6 followup — Event Sourcing + Replay

**Date:** 2026-06-04
**Dimension:** Event sourcing + replay
**Branch:** main (post v5 medium-tail batches a–r)
**Scope verified:** `Server/Account/src/Db/EventStore.hpp`, `Server/Account/src/Db/SnapshotWriter.hpp`, `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/Cache/AccountRepository.hpp`, `Server/Account/src/Reducers/*.hpp`, `Server/Account/src/Events/*.hpp`, all eight `event_type =` emit sites under `Server/Account/src/Handlers/`, `Server/Account/schema.sql` events partition, idempotency_cache interaction sites.

---

## Status of v5 items in this dimension

| v5 item | Status | Notes |
|---|---|---|
| **M-V5-1** AppendIdempotent recheck must hold advisory lock | **CLOSED** (commit `a9bb510` + followup `1644d92`) — verified at `Server/Account/src/Db/EventStore.hpp:147-160`. The catch block now acquires `AcquireAdvisoryLockInTx(tx, ev.account_id)` before the `SELECT 1` recheck and `tx.commit()`s so the lock releases promptly. Pattern parity with `Append` (lines 115-121). |
| **M-V5-2** ProgressionReducer doesn't validate `evt.from_level` | **STILL OPEN** — `Server/Account/src/Reducers/ProgressionReducer.hpp:42-62` unchanged. The DEFER block at lines 13-41 explicitly notes the gap and tracks the revisit triggers (SnapshotWriter wire-up, compensating-event path, horizontal scale). Carry-forward to v6. |
| **C-V5-1** SnapshotWriter never instantiated | **STILL OPEN** — `Server/Account/src/AccountServer.hpp:356-366` carries a DEFER block where the member would live. `grep` confirms zero `SnapshotWriter` instantiations outside `Server/Account/src/Db/SnapshotWriter.hpp` and its test. `snapshots` table still 0 rows. The deferral now has a documented four-piece scaffolding gap (REDUCER_VERSION constant, ToJson/FromJson for the 4 reducer states, per-aggregate cached_snapshot_version, projection from Account → ReducerState). Carry-forward to v6. |
| **L-V5-1** progression sibling key direction-blind | STILL OPEN (no commit touched it; bounded-cardinality keeps urgency low). |
| **L-V5-2** OutboxRelay::Register never called (= H-V5-2 cross-listed) | **STILL OPEN** — confirmed; no `m_outboxRelay.Register(` calls anywhere. Today zero-impact since no handler emits to outbox; M-V4-2 EffectDispatcher remains dead-coded. The dispatch half re-opens the moment the first handler emits. |
| **L-V5-3** pre-state read after TickQuests::Apply in claim | STILL OPEN — `Server/Account/src/Handlers/QuestHandlers.hpp:593, 604-613` unchanged. |
| **L-V5-4** GetPity / GetGuarantee lazy-flip dirty | STILL OPEN — carry-forward unchanged. |

**One v5 closure verified clean (M-V5-1) + one new defect surfaced in this dimension.** No regressions introduced by the v5 medium batches.

---

## NEW findings

### M-V6-1. Event payload `FromStr` parsers silently coerce unknown strings to a default — replay corruption on first vocabulary drift
**File:** `Server/Account/src/Events/WalletEvents.hpp:42-49` (`CurrencyFromStr`), `Server/Account/src/Events/QuestClaimEvents.hpp:71-81` (`RewardKindFromStr`)
**Status:** NEW

```cpp
// WalletEvents.hpp:42-49
inline Currency CurrencyFromStr(const std::string& s) {
    if (s == "credits")           return Currency::Credits;
    if (s == "universal_credits") return Currency::UniversalCredits;
    if (s == "tickets")           return Currency::Tickets;
    if (s == "limited_tickets")   return Currency::LimitedTickets;
    if (s == "scrap")             return Currency::Scrap;
    return Currency::Credits;   // <-- silent fallback
}
```

`RewardKindFromStr` has the same shape — unknown string returns `RewardKind::Credits`.

**Impact:** any replay path through `CurrencyDeltaFromJson` / `RewardFromJson` that encounters an unknown vocabulary string silently mutates the event into a Credits delta / Credits reward. Once `SnapshotWriter` wires up (C-V5-1) and cold-start replay starts firing reducers over historical events, a future vocabulary addition (`stellar_dust`, `compensation_token`, anything) emitted before the consumer rebuild would replay as **Credits**, corrupting the wallet projection. Today the read path is dead (no replay), so impact is zero — but the failure mode is silent state divergence the moment replay is reachable, which is the exact reason snapshots/replay exist in the first place. Same defect family as M-V4-3 cross-cutting (`event_type` is comment-coupled, not code-coupled).

**Fix sketch:** make `*FromStr` return `std::optional<T>` or throw on unknown. Reducer dispatch path treats the parse failure as `WalletInvariantViolation` / `QuestClaimInvariantViolation` — symmetric with the on-emit-invariant violations the other branches throw. Cheap (~10 LOC each), pins the contract so a future vocabulary string can't silently land as Credits on replay.

### M-V6-2. Event payload `*FromJson` silently zero-fills missing required fields — same replay-corruption shape
**File:** `Server/Account/src/Events/WalletEvents.hpp:51-61` (`CurrencyDeltaFromJson`), `Server/Account/src/Events/ProgressionEvents.hpp:27-37` (`StoryLevelAdvancedFromJson`), `Server/Account/src/Events/PullEvents.hpp:92-113` (`PullPerformedFromJson`), `Server/Account/src/Events/QuestClaimEvents.hpp:91-100` (`QuestRewardClaimedFromJson`)
**Status:** NEW

Every `FromJson` parser uses `j.value("field", default)` for every field. A missing `balance_after` parses as `0`; a missing `to_level` parses as `0`; a missing `pity_5_before` parses as `0`. The reducer then either accepts the zero-value (Progression — silently overwrites story_level to 0), or throws on a coincidental mismatch (Wallet — `next != 0` triggers `WalletInvariantViolation`).

The Progression case is the bad one: replay an event with a stripped/missing `to_level` and `xp_carried_over` and the reducer happily sets `story_level = 0, story_xp = 0` (M-V5-2's missing baseline check compounds this — even with `from_level` validation, the absent `to_level` defaults to 0 and `out.state.story_level = evt.to_level` clobbers).

**Impact:** any path that materializes an `Event::data` JSON object from a less-trusted source (test-fixture round-trip, future admin replay tool, future cross-service event subscriber) and feeds it through the `FromJson` shim into a reducer will silently corrupt the projection on missing required fields. Today the only consumer of the FromJson path is the SchemaMigrationTest golden fixtures, which pin all required fields — zero production impact. But the moment SnapshotWriter wires up (C-V5-1) and `LoadStream` results feed a reducer fold, a partial/malformed historical row corrupts the projection.

**Fix sketch:** introduce a strict-parse alternative `*FromJsonStrict` that throws on missing required fields, or audit the existing `j.value(...)` defaults at each call site and add per-field presence checks where the field is load-bearing (`balance_after`, `to_level`, `xp_carried_over`, `pity_5_after`, `pity_4_after`, `guarantee_5_after`). Cheap per field but high-leverage for replay determinism.

### L-V6-1. `event_type` is a wire-format string declared at 8 emit sites with no central registry
**Files:** `Server/Account/src/Handlers/AccountHandlers.hpp:418`, `Server/Account/src/Handlers/GachaHandlers.hpp:229, 261, 576, 602`, `Server/Account/src/Handlers/ProgressionHandlers.hpp:528`, `Server/Account/src/Handlers/QuestHandlers.hpp:456, 477, 533`
**Status:** REPEAT (M-V4-3 cross-cutting; cross-listed as M-V5-2 cross-cutting) — re-flagged from event-sourcing lens because golden fixtures depend on the exact string

```cpp
ev.event_type      = "credits_added";                                        // AccountHandlers.hpp:418
pullEvent.event_type      = "pull_performed";                               // GachaHandlers.hpp:229, 576
walletEvent.event_type    = "credits_spent";                                 // GachaHandlers.hpp:261, 602; ProgressionHandlers.hpp:528
bundle.claim.event_type   = "quest_reward_claimed";                          // QuestHandlers.hpp:456
e.event_type      = (after >= before) ? "credits_added" : "credits_spent";  // QuestHandlers.hpp:477
pe.event_type      = (postStoryLevel != pre.preStoryLevel) ? "story_level_advanced" : "story_xp_gained";  // QuestHandlers.hpp:533
```

Six distinct emit sites for `credits_spent`; two for `credits_added`; one each for the rest. No `constexpr const char*` declarations in `Events/Event.hpp` or anywhere else; vocabulary lives in `SchemaMigrationTest` golden fixtures and handler literals only. A typo at any one site (`"credit_spent"`, `"credits-added"`) silently lands an unrecognized event_type in the events table. Today nothing dispatches on event_type so impact is zero; the moment a reducer or analytics subscriber switches on it (likely first migration, M-V5-2 cross-cutting), the typo becomes a silent miss.

**Fix sketch:** declare `namespace aphelyon::events { constexpr const char* kEventTypeCreditsSpent = "credits_spent"; ... }` (6 strings — same shape as `AggregateKindToStr`'s enum-keyed dispatch). All 8 emit sites reference the constants. The fix is structural — closes the typo-silently-lands surface and gives future reducer dispatches a constant to switch on.

### L-V6-2. `PullsReducer` doesn't store `rng_state_after` on replay — RNG projection can't be rebuilt
**File:** `Server/Account/src/Reducers/PullsReducer.hpp:62-66`, `Server/Account/src/Events/PullEvents.hpp` (no `rng_state_after` field)
**Status:** NEW (design intent observation)

```cpp
// RNG advance is captured by the handler before/after rolling; we don't
// recompute here. Store the post-pull RNG state if the event provided one
// (left to a follow-on commit; current event carries only rng_state_before).
```

`PullPerformed` carries `rng_state_before` and `algorithm_version` but no `rng_state_after`. Reducer can't reconstruct the slot's `rng_state` after a replay fold — only the next pre-state of the next event provides it. On the final event of a partial replay, the post-state RNG is unknown. Pattern C (spec line 58) intentionally trusts the captured `outcomes` array on replay rather than re-rolling, so live-state replay is correct — but a snapshot rebuild that fires the reducer over `[N, N+5]` events leaves `SlotPullsState::rng_state` stuck at `rng_state_before` of event N+5 (the last advance was during event N+5's roll, which we skipped because we trusted outcomes). The projection then writes back an RNG state that's one-pull-stale. The next live pull would roll with stale RNG, breaking the rng_state_before audit invariant on event N+6.

**Impact today:** zero — replay path is unreachable (C-V5-1). The cost surface only materializes when SnapshotWriter writes a snapshot containing `SlotPullsState::rng_state` and a subsequent cold-start rebuild reproduces an off-by-one stale state. Live writes from the handler use the actual in-memory RNG (which is correct) — the divergence is between in-memory state and replay-rebuilt state.

**Fix sketch:** add `rng_state_after` to `PullPerformed` (mirroring the existing pity_*_after fields). Reducer stores it on Apply. Bump `schema_version` for pull events to 2 (first real consumer of the upgrade-path machinery — see L-V6-3). Pre-launch this is observation only; gate the actual change on C-V5-1 wire-up because the cost of an event schema bump exceeds the replay-correctness win until snapshots are alive.

### L-V6-3. `events.schema_version` has no version-bump runbook — first migration will write itself into a corner
**File:** `Server/Account/schema.sql:311-324` (DEFER comment), `Server/Account/src/Events/Event.hpp:32` (default = 1)
**Status:** REPEAT (M-V5-2 cross-cutting); re-flagged from event-sourcing lens

The schema comment cites spec lines 670-671 (weak-additive vs upcaster) and names the action ("add an upcaster registry keyed by (event_type, from_version, to_version), wire AppendEvent to write the current per-type version, wire reducer-fold to call the upcaster chain before dispatch") — but no scaffolding exists. The current emit path writes `1` unconditionally because the struct default is 1; the read path reads the int but no consumer branches on it.

The architectural debt is the **per-event-type** version semantics. Today the column is one global int, but the doc'd evolution path requires per-(event_type, version) tracking. The `Event` struct has no per-type version override mechanism (e.g., `kCurrentVersion` constant on each payload type). A future v2 emit for `pull_performed` while `credits_spent` stays at v1 is a meaningful migration but has no shape in the current types.

**Impact:** the first time a payload migrates, **whoever owns it gets to invent the entire upgrade contract from scratch**, which means the SchemaMigrationTest fixtures (which assert `== 1` on every event_type) need re-shaping, the reducer dispatch needs a switch on the column, and the constant has to live somewhere coordinated with the upcaster registry. None of this exists today; the migration spec describes intent but not a landing zone. Architectural debt blocking future migrations.

**Fix sketch:** even a minimal landing zone closes the gap — (a) per-payload-type `static constexpr int kSchemaVersion = 1` on each event struct (Wallet::CurrencyDelta, etc.), (b) `Event::FromPayload<T>` helper that copies `T::kSchemaVersion` into the event, (c) `ReducerCommon::ApplyMigrations(event_type, schema_version, json) → json` stub that's a no-op today but pins the seam. The upcaster registry follows when first needed. ~40 LOC, no behavior change, lays the foundation.

---

## Observations / Lows

- **Idempotency-cache short-circuit correctly bypasses the event log.** All 8 cache-hit sites (`Server/Account/src/Handlers/{GachaHandlers,QuestHandlers,ProgressionHandlers,AccountHandlers}.hpp`) return the cached `response_payload` without calling `Begin()` / `AppendEvent` / `Commit`. The same event log row written at T1 is the only row; T2 retry sees the cache hit and never appends. Verified clean — no event log advancement on retry.

- **`AccountTransaction::Commit` correctly bumps `cached_*_version` only AFTER `tx_->commit()` and only for events actually appended in this txn (`AccountTransaction.hpp:374-382`).** The denormalized accounts row (advanced via `MutableDirty().accounts_row = true` in `AppendEvent` line 79) is flushed in the same pqxx::work as the event INSERTs. Lockstep verified.

- **`AccountRepository::LoadEventVersions` (line 646-676) hydrates per-aggregate cursors from `MAX(version)` GROUP BY aggregate_kind.** A reload after eviction reads the true DB state; the next emit uses `cached_*_version + 1` and the EventStore::AppendInTx pre-check (line 70-79) is the optimistic concurrency backstop. Cursor consistency verified.

- **Today's "replay contract" is effectively `LoadEventVersions` + relational table reads.** The events table is append-only audit; the projection is the relational rows. Reducers are exercised only by `Server/Account/tests/GoldenFile/SchemaMigrationTest.cpp` and the property tests. No production code path reads `LoadStream` results into a reducer fold. C-V5-1 SnapshotWriter wire-up is the precondition for the replay contract becoming load-bearing — at which point M-V5-2 (ProgressionReducer baseline check), M-V6-1 (FromStr silent coercion), M-V6-2 (FromJson zero-fill), L-V6-2 (PullsReducer rng_state_after) all become real divergence risks.

- **AggregateKind enum + `AggregateKindToStr` switch (Event.hpp:9-24) is the one centralized vocabulary.** Adding a new aggregate requires touching the enum + 3 switches (AggregateKindToStr, AccountTransaction.hpp:376-381, AccountRepository.hpp:657-660). The pattern is consistent and the LoadEventVersions ERROR-log (line 669-673) for unknown aggregate_kind makes the gap impossible to miss after a schema bump. Worth keeping — don't refactor.

- **WalletReducer's `next != evt.balance_after` check (line 35) implicitly validates `balance_before` via the on-emit invariant `balance_after = balance_before + amount`.** If `current_state != evt.balance_before`, then `next = current_state + amount != balance_before + amount = balance_after`, throw. So an explicit `balance_before` check would be redundant in the well-formed case. The implicit check breaks only if a malformed event sets `balance_after = current_state + amount` directly — which is forgery-shape, not drift-shape. Treat as tight enough.

- **No new defects introduced by the v5 medium-tail batches (a–r) in this dimension.** Verified against `git log` of the 15+ commits since `a9bb510`; none touched the reducer / event payload / EventStore surfaces.

---

## Suggested triage order

**Pre-launch (Medium):**
1. **M-V6-1** — `CurrencyFromStr` / `RewardKindFromStr` throw on unknown. ~10 LOC each + reducer-level catch. Closes a replay-corruption surface that becomes real with C-V5-1.
2. **M-V6-2** — strict-parse `*FromJson` shims OR per-field presence checks at load-bearing fields. ~30 LOC.

**Bundle with C-V5-1 when revisited:**
3. **L-V6-2** — add `rng_state_after` to `PullPerformed`, store in reducer. Couples with first schema_version bump.
4. **L-V6-3** — minimal upcaster scaffolding (per-payload `kSchemaVersion` constant + ApplyMigrations no-op shim). Closes the architectural-debt surface before the first migration writes itself into a corner.

**Carry-forward (no urgency):**
5. **L-V6-1** — `event_type` constants. ~6 string declarations + 8 site swaps; closes typo-silently-lands surface.

---

## Closing observation

The event-sourcing dimension is **still clean for production traffic today** — every defect in this report (M-V6-1, M-V6-2, L-V6-2, L-V6-3 + the v5 carry-forwards M-V5-2 and C-V5-1) requires the replay path to be reachable to bite. Live writes are correct: handlers compute event payloads from in-memory state, AppendInTx's optimistic concurrency check + advisory lock guard version monotonicity, AccountTransaction's atomic events→flush→outbox→audit→idem chain stays consistent, idempotency cache correctly skips the event log on retry.

The architectural debt sits in the **replay contract**, which is half-built. SnapshotWriter (C-V5-1) is the keystone — wire it up and every deferred reducer/parser/event-version gap surfaces simultaneously. The right v6 move is **bundle the C-V5-1 wire-up with M-V5-2 (Progression baseline) + M-V6-1 (strict FromStr) + M-V6-2 (strict FromJson) + L-V6-2 (rng_state_after) + L-V6-3 (schema_version scaffolding)** as one coherent "make replay correct end-to-end" landing. Independently each is a single-digit-LOC fix; together they close the entire selective-event-sourcing design's intended invariant surface in one pass.

No new Criticals. No new Highs. Two new Mediums (M-V6-1, M-V6-2). Three new Lows (L-V6-1, L-V6-2, L-V6-3). The v5 in-dimension surface (C-V5-1, M-V5-2, L-V5-1 through L-V5-4) carries forward unchanged except for M-V5-1 which is verified closed.
