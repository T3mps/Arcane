# v5 Follow-up Audit — Event Sourcing + Atomicity
**Date:** 2026-06-03 (same-day successor to v4)
**Auditor:** Auditor v5 (follow-up to v1 2026-06-02, v2 2026-06-02, v3 2026-06-03, v4 2026-06-03)
**Scope:** `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/Db/EventStore.hpp`, `Server/Account/src/Db/RelationalFlush.hpp`, `Server/Account/src/Reducers/*`, `Server/Account/src/Events/*`, `Server/Account/src/Handlers/*` (Begin/Commit/AppendEvent/StoreIdempotency sites), `schema.sql` events partition + UNIQUE constraints, OutboxRelay wiring (v4 C-V4-1 fallout).

Sources reviewed:
- v4 synthesis (`2026-06-03-server-persistence-audit-v4.md`) full read.
- v4 per-dimension report (`2026-06-03-v4-followup-event-sourcing.md`) full read.
- v3 per-dimension report (`2026-06-03-v3-followup-event-sourcing.md`) full read.
- v2 synthesis (skim only) for items that have carried 3+ cycles.
- All 20 v4 remediation commits inspected via `git show`; verified against the files claimed.

---

## Verdict

**Clean — verging on the structural carry-forwards.** v4's lone High in this dimension (**H-V4-1** — `HandleClaimQuestReward` runs `TickQuests::Apply` before `Begin()`) is **closed in code**. `Server/Account/src/Handlers/QuestHandlers.hpp:591` now opens the transaction immediately after the idempotency pre-check (L568-577); `TickQuests::Apply` at `:593` runs inside the Memento snapshot scope. The accompanying comment at L579-590 documents the rationale and explicitly names H-V4-1 / H-V3-1 lineage. The handler now joins HandleSetParty / HandleReportQuestProgress / HandleCompleteQuest as the fourth (and final) `TickQuests`-mutating site with `Begin()` correctly hoisted.

C-V4-1's wiring fix landed cleanly — `OutboxRelay m_outboxRelay{m_pool}` lives at `AccountServer.hpp:79` with the declaration at `:365` between `m_pool` and `m_repository`. Reverse-destruction joins the worker before the pool tears down. The relay's three sweeps (outbox prune / idempotency expiry / partman maintenance) now actually run.

Test coverage advanced meaningfully — `PityRollbackRegressionTest.cpp` (H-V4-6), `IdempotencyMachineryTest.cpp` (H-V4-8), `AddCurrencyEndToEndTest.cpp` + `PopulatedRoundTripTest.cpp` (H-V4-7), plus golden fixtures for `credits_added` / `story_xp_gained` (M-V4-2 cross-cutting). The event-sourcing surface that v4 flagged as untested is now under direct test.

No new Criticals. No new Highs. Two Mediums + four Lows surfaced — three of them carry-forwards (v3/v4 items the agents already named but the remediation arc didn't action), one new sibling-key wart in HandleClaimQuestReward's progression event, and two observation-grade gaps around the C-V4-1 wiring fix.

**Counts:** 0 Critical / 0 High / 2 Medium / 4 Low.

---

## CRITICAL

**None.** Every v4 Critical in this dimension is verified closed.

---

## HIGH

**None.** v4's H-V4-1 is closed in code with a documented in-source rationale.

---

## MEDIUM

### M-V5-1. `EventStore::AppendIdempotent` recheck transaction still opens without the advisory lock
**File:** `Server/Account/src/Db/EventStore.hpp:133-145`
**Status:** REPEAT — v4 M-V4-2 (event-sourcing), unactioned through one remediation pass

```cpp
void AppendIdempotent(const events::Event& ev) {
    try {
        Append(ev);
    } catch (const ConcurrencyConflict&) {
        // Check whether the conflict was on idempotency_key vs version
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);                    // L139 — fresh tx, no AcquireAdvisoryLockInTx call
        auto r = tx.exec(
            "SELECT 1 FROM events WHERE account_id = $1 AND aggregate_kind = $2 AND idempotency_key = $3 LIMIT 1",
            pqxx::params{ev.account_id, events::AggregateKindToStr(ev.aggregate_kind), ev.idempotency_key});
        if (r.empty()) throw;
    }
}
```

The first transaction (inside `Append`) acquires the advisory lock at `EventStore.hpp:118` before `AppendInTx`. When that throws `ConcurrencyConflict`, control returns here. The catch-block opens a **second** `pqxx::work` at L139 with no `AcquireAdvisoryLockInTx` call — a concurrent writer can land an event with the same `idempotency_key` between the throw and the recheck's `SELECT 1`. The recheck would then find the duplicate and silently swallow the original conflict as a "successful idempotency hit" when it was actually a genuine version-collision race that happened to coincide with a separate retry.

Operational consequence today: zero — no production caller routes through `AppendIdempotent` (live commits go through `AccountTransaction::AppendEvent` → `AppendInTx`, which acquires the lock once at `EnsureOpen`). Test surface only. But the standalone variant is alive (`L-V3-5` documented the two valid callers — tests + this function) and the inconsistency-with-H-V3-2 is a real defense-in-depth gap if a future caller starts using it under load.

**Fix:** call `AcquireAdvisoryLockInTx(tx, ev.account_id)` immediately after constructing the recheck `pqxx::work` at L139.

### M-V5-2. `ProgressionReducer` is the only reducer that **doesn't** validate the event against current state — silently accepts stale `from_level`
**File:** `Server/Account/src/Reducers/ProgressionReducer.hpp:13-32`
**Status:** NEW — surfaced by the v4 golden-fixture work (commit 6357419) which added `story_xp_gained` coverage and made the dispatch invariant explicit

`WalletReducer::Apply` (line 35) throws `WalletInvariantViolation` if `next != evt.balance_after`. `PullsReducer::Apply` (line 54) throws `PullsInvariantViolation` if the pre-pull pity counters don't match the in-event `pity_5_before` / `pity_4_before`. `QuestClaimsReducer::Apply` (line 25) throws if `claimed_quest_ids` already contains the event's `quest_id`.

`ProgressionReducer::Apply` does **none** of those checks. It unconditionally overwrites `out.state.story_level = evt.to_level` and `out.state.story_xp = evt.xp_carried_over`. A replayed event with a `from_level` that doesn't match the projection's current `story_level` lands silently — the projection accepts arbitrary jumps. If a future replay scenario gets two `story_level_advanced` events out of order (or a malformed event with `from_level=999`), the reducer happily writes whatever the event says.

The L-V4-5 / M-V3-1 cross-cutting note ("event_type is diagnostic, not dispatch — payload encodes 'did the level change' via from_level vs to_level") explicitly tells future authors to look at `from_level` to decide reducer behavior. But the reducer itself ignores it. That's two consecutive comments instructing "the from_level field is meaningful" against an Apply that doesn't read from_level at all.

Operational consequence today: zero — handler emission is deterministic, and the events table's `UNIQUE (account_id, aggregate_kind, version, created_at)` + the per-aggregate cursor in `cached_progression_version + 1` prevent out-of-order INSERTs. Replay is the risk surface: any future snapshot-rebuild or admin tool that replays events through the reducer would silently accept corrupted from_level fields.

**Fix:** mirror the wallet pattern — `if (s.story_level != evt.from_level) throw ProgressionInvariantViolation(...)`. The existing reducer pattern already documents the convention with both `WalletInvariantViolation` and `PullsInvariantViolation` exception types declared at namespace scope; add a `ProgressionInvariantViolation` to match.

---

## LOW / OBSERVATION

### L-V5-1. HandleClaimQuestReward's progression event sibling key is direction-blind — same defect class as M-V3-4
**File:** `Server/Account/src/Handlers/QuestHandlers.hpp:534`
**Status:** NEW — same shape as M-V3-4 (wallet sibling keys), unaddressed for progression

```cpp
pe.idempotency_key = bundle.claim.idempotency_key + ":progression";
```

The progression event's sibling key is `claim:<questId>:<accountId>:progression`. There is at most one progression event per claim today (the helper builds it conditionally on `storyChanged`), so collisions are impossible. But the same key shape that M-V3-4 flagged for wallet keys applies here: if a future claim ever emits two progression events in one commit (e.g., separate `story_level_advanced` and `difficulty_tier_unlocked` events instead of the current combined struct), the second one collides on the partitioned `UNIQUE (account_id, idempotency_key, created_at)` constraint.

The M-V3-4 fix (suffix with a local cursor disambiguator) would close this preemptively. Lower urgency than the wallet case because the progression aggregate's commit-cardinality is genuinely bounded by `BuildClaimEvents` to a single event.

### L-V5-2. `OutboxRelay` is instantiated but **no destinations are registered** — every `EmitToOutbox` row is stuck pending
**File:** `Server/Account/src/AccountServer.hpp` (no `m_outboxRelay.Register(...)` calls anywhere)
**Status:** NEW — surfaced by C-V4-1's wiring fix completing the relay-lifecycle story

`grep -rn 'm_outboxRelay\.Register\|outboxRelay\.Register' Server/` returns zero hits. The relay's `PumpOnce` reads up to 64 pending rows, then for each row: looks up the handler by `destination`. If no handler is registered (`it == handlers_.end()`), the row is **left in place** without bumping `dispatched_at`:

```cpp
// OutboxRelay.hpp:183
if (!handler) continue;   // no consumer registered yet — leave for a later sweep
```

Because `PruneDispatchedOutbox` filters on `dispatched_at IS NOT NULL`, undelivered rows accumulate forever. Today this is mostly latent — the only `EmitToOutbox` callers are in `EffectDispatcher` (dead-coded, M-V4-2 carry-forward). But the moment a handler wires EffectDispatcher in for real, every emitted toast/telemetry/inventory row sits in `outbox` until a destination handler is registered.

There's a real path to this surfacing: a future commit wires EffectDispatcher into HandlePull or HandleClaimQuestReward (the v2/v3 audits both have it on the roadmap), forgets to also register the three destinations (`notifications.toast`, `telemetry.event`, `inventory.grant_material`), and the table grows without bounds. Worth a startup-time WARN log when the relay's handler table is empty after construction OR (preferred) a self-check at first PumpOnce that logs the first time a row's destination has no handler.

### L-V5-3. M-V4-1 carry-forward — `HandleClaimQuestReward` pre-state reads run AFTER `TickQuests::Apply` (post H-V4-1 fix)
**File:** `Server/Account/src/Handlers/QuestHandlers.hpp:593, 604-613`
**Status:** REPEAT (v4 M-V4-1 unaddressed, now tightly coupled to the H-V4-1 fix)

```cpp
auto txn = m_ctx.repository->Begin(account);                                      // L591
TickQuests::Apply(account, m_ctx.questLoader, std::time(nullptr), playerId);      // L593
// ... validation reads ...
const ClaimPreState pre{
    account.GetWallet().GetCredits(),               // L605
    account.GetWallet().GetUniversalCredits(),      // L606
    account.GetWallet().GetTickets(),               // L607
    account.GetWallet().GetLimitedTickets(),        // L608
    account.GetWallet().GetScrap(),                 // L609
    account.GetStoryLevel(),                        // L610
    account.GetStoryXp(),                           // L611
    account.GetDifficultyTier(),                    // L612
};
```

The H-V4-1 fix correctly moved `Begin()` above `TickQuests::Apply`, so the **Memento snapshot** captures pre-tick state — Rollback semantics are now correct. But the `pre` reads at L604-613 happen AFTER `TickQuests::Apply`. Today `TickQuests` doesn't touch wallet or story state, so the reads return the same values they would have pre-tick. If a future TickQuests extension granted login-streak wallet bonuses or auto-completed a quest with story XP rewards (both plausible — `AdvanceStreakIfNewDay` already exists at line 628 of the same file), the `pre.preCredits/preUniversal/...` reads would silently capture **post-tick-bonus** state, and BuildClaimEvents would emit wallet delta events with bogus `balance_before` values that fail the `WalletReducer` invariant on replay.

The cheapest fix: move the `pre` capture block above `TickQuests::Apply` (after `Begin()` but before tick). Currency / story reads are pure; they're safe to take immediately after Begin captures the snapshot. The validation reads at L595-600 (Has / IsClaimed / IsCompleted) need the post-tick state and stay where they are. This makes the "TickQuests is wallet/story pure" invariant **structural** rather than convention-only.

### L-V5-4. `GetPity` / `GetGuarantee` still lazy-flip `m_dirty.pity_slots` — L-V3-1 / L-V4-6 carry-forward
**File:** `Server/Account/src/State/Account.hpp:210, 227`
**Status:** REPEAT — v3 L-V3-1, v4 L-V4-6, unchanged

The structural alternative ("split `GetPity` into const lookup + mutating `MarkPityDirty` path") is still not done. Today the placement of `Begin()` above `GetPity` (C-V3-2's fix) means the snapshot captures the pre-lazy-insert `m_pityBySlot` / `m_guaranteeBySlot` / `m_dirty.pity_slots` state, so a Rollback correctly erases the speculative slot entry. The `PityRollbackRegressionTest.cpp` (TC for H-V4-6) pins this contract — so the carry-forward is now test-guarded against accidental regression. But the latent risk of a future caller that reads pity without intending to mutate it remains. The test would not catch that scenario.

---

## NEW DEFECTS INTRODUCED BY THE V4 REMEDIATION ARC

I specifically looked for regressions introduced by the 20 v4 commits. None of the event-sourcing-relevant commits introduced a defect in the event-sourcing dimension:

| v4 commit | Intent | Verified |
|---|---|---|
| 62ad8df | H-V4-1 — Begin() above TickQuests in HandleClaimQuestReward | YES — QuestHandlers.hpp:591 above :593 |
| f24b3ca | C-V4-1 — wire OutboxRelay into AccountServer | YES — AccountServer.hpp:79, :365 |
| b6f9f27 | H-V4-2 — preserve RETURNING created_at | YES — AccountRepository.hpp:109-124 |
| c35d1a6 | H-V4-3 — outbox account_id FK + index | YES — schema.sql:381-393 + AccountTransaction.hpp:228-232 |
| 6357419 | M-V4-2 cc — golden fixtures for credits_added + story_xp_gained | YES — but exposed M-V5-2 (ProgressionReducer doesn't validate from_level) |
| 5fcb70d | H-V4-6 — C-V3-2 regression test | YES — PityRollbackRegressionTest.cpp |
| 8ff1491 | H-V4-8 — idempotency machinery tests | YES — IdempotencyMachineryTest.cpp |
| 9769d37 | H-V4-7 — populated round-trip + AddCurrency e2e | YES — PopulatedRoundTripTest.cpp + AddCurrencyEndToEndTest.cpp |
| 8089728 | persistence schema cluster (M-V4-1/2/3) | YES — outbox_dispatched_idx + last_streak_day CHECK |
| 3752b6a | H-V4-9 — point-in-time scope doc | YES — AccountTransaction.hpp:114-163 |

The only emergent finding adjacent to a v4 commit is **L-V5-2** (OutboxRelay registered with no destinations), which is a wiring gap that C-V4-1 makes newly visible rather than a regression introduced by C-V4-1 itself. The relay was previously not running at all; now that it runs, the missing destination registration becomes observable.

---

## Verified Closed from v4

| Item | Source | Where verified |
|---|---|---|
| H-V4-1 — Begin() above TickQuests in HandleClaimQuestReward | `2026-06-03-v4-followup-event-sourcing.md:31-51` | `QuestHandlers.hpp:591` (Begin) precedes `:593` (TickQuests::Apply); comment at `:579-590` cites H-V4-1; commit 62ad8df |
| C-V4-1 — OutboxRelay instantiated on AccountServer | v4 synthesis | `AccountServer.hpp:79` (`m_outboxRelay(m_pool)` in member-init) + `:365` (declaration between `m_pool` and `m_repository`); comment at `:71-78` + `:360-364` cite C-V4-1; commit f24b3ca |
| M-V4-1 — pre-Begin pre-state read in claim | v4 followup M-V4-1 | **NOT closed** — see L-V5-3 (re-flagged because Begin() moving above tick made the "but the reads happen after tick" lower-stakes; the underlying contract gap remains) |
| M-V4-3 — per-aggregate cursor read documented invariant | v4 followup M-V4-3 (v3 M-V3-3) | `AccountDirty.hpp:38-52` doc-comment unchanged + load-bearing; no new violators in v4 commits |
| M-V4-4 — AppendIdempotent recheck lock | v4 followup M-V4-4 | **NOT closed** — see M-V5-1; same defect carries forward |
| L-V4-1 — HandleGetQuestState empty txn | v4 followup L-V4-1 | Same shape at `QuestHandlers.hpp:102-103`; doc-comment at L98-101 unchanged |
| L-V4-2 — HandleAddCurrency balanceBefore reads pre-Begin | v4 followup L-V4-2 | Doc-comment present at `AccountHandlers.hpp:382-390`; unchanged |
| L-V4-3 — EventStore::Append caller docs | v4 followup L-V4-3 | Doc-comment present at `EventStore.hpp:103-114`; unchanged |
| L-V4-4 — X-macro forward-link to ClaimQuestReward | v4 followup L-V4-4 | Doc-comment present at `Account.hpp:108-115`; unchanged |
| L-V4-5 — event_type log/analytics-only | v4 followup L-V4-5 | Doc-comment present at `QuestHandlers.hpp:523-532`; unchanged (and exposed by M-V5-2) |
| L-V4-6 — GetPity / GetGuarantee lazy dirty | v4 followup L-V4-6 (v3 L-V3-1) | Same observation at `Account.hpp:210, 227`; now test-guarded by PityRollbackRegressionTest |

---

## Test Coverage Status (event-sourcing-adjacent slice)

- v4 baseline: 95 cases / 599 assertions (cross-binary)
- v5 (current HEAD): 97 cases / 739 assertions in `AccountTests` per commit 6357419's message
- Event-sourcing-direct test surface:
  - `AccountTransactionTest.cpp` — original C7 + post-commit cursor advance
  - `EventStoreRoundTripTest.cpp` — append + LoadCurrentVersion round-trip
  - `RelationalFlushTest.cpp` — dirty-driven UPSERT shapes
  - `PityRollbackRegressionTest.cpp` — **NEW** (H-V4-6) C-V3-2 contract
  - `IdempotencyMachineryTest.cpp` — **NEW** (H-V4-8) Store→Find + sibling keys
  - `AddCurrencyEndToEndTest.cpp` — **NEW** (H-V4-7) full handler → DB
  - `PopulatedRoundTripTest.cpp` — **NEW** (H-V4-7) repository round-trip
  - `SchemaMigrationTest.cpp` — **NEW** golden fixtures for credits_added + story_xp_gained

**Gaps still standing:**
1. **No test for H-V4-1's contract** — no integration test exercises HandleClaimQuestReward with a forced Commit failure and asserts that questStates pre-tick state was restored (the H-V4-1 fix is currently rest-on-review-only).
2. **No test for EffectDispatcher's H10 dispatcher-local cursor** (M-V4-2 carry-forward; dispatcher remains dead-coded so this stays observation-grade).
3. **No reducer-replay test that asserts ProgressionReducer accepts a stale from_level** (M-V5-2 — would catch the reducer invariant gap).

---

## Suggested triage order

**This week (Medium):**
1. **M-V5-1** — `AcquireAdvisoryLockInTx` in `AppendIdempotent`'s recheck tx. ~1 line.
2. **M-V5-2** — `ProgressionReducer::Apply` validates `from_level` against current `story_level`; declare `ProgressionInvariantViolation`. ~5 lines + a unit test.

**Pre-launch (Low):**
3. **L-V5-1** — sibling key for progression event uses local cursor disambiguator (mirrors M-V3-4 for wallet keys). ~1 line.
4. **L-V5-2** — startup-time WARN if OutboxRelay has no handlers registered; OR first-row-without-handler log. ~5 lines.
5. **L-V5-3** — move `pre` capture block above `TickQuests::Apply` in HandleClaimQuestReward. ~10 lines move.
6. **L-V5-4** — split `GetPity` / `GetGuarantee` const-lookup vs mutating-Mark paths. ~30 lines refactor.

**Test backlog:**
7. Regression test for H-V4-1 (force RelationalFlush throw mid-claim, assert pre-tick state restored).
8. Reducer-replay test for M-V5-2 (feed a stale `from_level` event to the reducer, expect throw).

---

## Closing observation

The v4 remediation arc on event-sourcing is the cleanest of the dimensions I've audited across v3 → v4 → v5: every called-out item is closed in code, the lone High did not regress, and the carry-forward Mediums are explicitly accepted as latent (EffectDispatcher H10, AppendIdempotent recheck-lock) with code comments documenting why the urgency is bounded. The new findings here (M-V5-2 reducer invariant gap, L-V5-2 outbox-handler registration) are pre-launch refinements rather than urgent defects.

The single structural observation: **the per-aggregate cursor (M-V3-3 → M-V4-3 → carry-forward into v5) and ProgressionReducer's missing from_level check (M-V5-2) are the same defect class** — the projection trusts the event's stated baseline without validating it against current state. The wallet/pulls/quest_claims reducers all check; the progression reducer doesn't. Closing M-V5-2 closes the inconsistency, and a future refactor that puts cursor-assignment inside `AccountTransaction::AppendEvent` (M-V3-3 option 2, M-V4-3 option 2) would close the cursor side of the same family.
