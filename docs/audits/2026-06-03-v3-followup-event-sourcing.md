# v3 Follow-up Audit — Event Sourcing + Atomicity
**Date:** 2026-06-03
**Auditor:** Auditor v3 (follow-up to v1 2026-06-02 + v2 2026-06-02)
**Scope:** AccountTransaction (Commit / Rollback / Memento snapshot), EventStore (AppendInTx + M-V2-5 advisory lock), RelationalFlush, all four reducers, all event types, the four event-emitting handlers (GachaHandlers, AccountHandlers, ProgressionHandlers, QuestHandlers), AccountDirty + cursor management, schema (events partition, UNIQUE constraints, advisory-lock interaction), Account.hpp Snapshot via X-macro.

---

## Summary

The v2 remediation pass landed cleanly. C7-A (move `Begin()` above all mutations) is correctly applied in the five handlers explicitly scoped: HandlePull, HandleMultiPull, HandleAddCurrency, the four progression handlers via `CommitProgressionScrapSpend`, and HandleClaimQuestReward. M-V2-5 (advisory lock) is in `AppendInTx`. M-V2-6 (reverse cursor-then-clear order in the post-commit catch) is in place. H-V2-9 (deterministic sibling event keys) is wired in both Pull/MultiPull/AddCurrency and the QuestHandlers BuildClaimEvents helper. M-V2-4 (`BuildClaimEvents` extraction) preserves the v2 inline behavior byte-for-byte. The per-aggregate cursor (`cached_*_version`) is the property of `AccountTransaction::Commit` alone — no handler self-increments — and the post-commit catch + stale-flag reload + LoadEventVersions hydration close the failure-recovery loop.

The v3 sweep turns up **zero new Critical**. Two new Highs and four Mediums emerged, all but one being scope-creep findings (v2 framed C7-A as a 5-handler list; three other handlers that ALSO mutate `Account` and rely on transactional rollback semantics were left out of the rewrite). The remaining items are minor: an advisory-lock-per-event perf wart inside multi-event commits, an event-less Commit path that lacks the M-V2-5 protection it was supposed to receive, and the persistent H10 latent bug in `EffectDispatcher` that v2 already noted.

---

## Critical

**None.** Every v1 + v2 Critical in this dimension is verified closed.

---

## High

### H-V3-1. C7-A leaves three mutating handlers in the v1 pre-fix state
**Files:**
- `Server/Account/src/AccountHandlers.hpp:281-289` (HandleSetParty — `account.SetParty/SetWeaponEquipment/SetGearEquipment` at 281-283, then `Begin()` at 289)
- `Server/Account/src/QuestHandlers.hpp:226-287` (HandleReportQuestProgress — `TickQuests::Apply` at 226, quest state mutations 229-285, then `Begin()` at 287)
- `Server/Account/src/QuestHandlers.hpp:666-686` (HandleCompleteQuest — `TickQuests::Apply` at 666, quest state mutations 668-684, then `Begin()` at 686)
**Status:** NEW (scope-creep of C7-A — the v2 fix listed 5 handlers and missed these 3)

C7-A's design intent: `AccountTransaction`'s Memento snapshot captures pre-mutation `Account` state at construction (`preTxSnapshot_(account.CaptureSnapshot())` at AccountTransaction.hpp:49), so a Commit throw → Rollback → RestoreFrom puts the live account back to its pre-RPC state. v2 verified this for the 5 event-sourced handlers; these three were missed.

All three mutate the live `Account` BEFORE constructing the `AccountTransaction`:

- **HandleSetParty** writes `m_party`, `m_weaponEquipment`, `m_gearEquipment` plus their dirty bits (`party_slots`, `loadout_keys`) via `account.SetParty(newParty)` / `SetWeaponEquipment` / `SetGearEquipment` at lines 281-283. The snapshot at 289 captures POST-mutation values; on Commit-throw (RelationalFlush failure on `party_slots` UPSERT or `loadouts` UPSERT), Rollback's `RestoreFrom` restores POST-mutation state. The live `Account` keeps the speculative party / equipment write while the DB rolled back — the original C7 wart, just relocated to non-event-sourced flows.
- **HandleReportQuestProgress** mutates `q->state`, `q->startedAt`, per-objective `progress`, `q->metadata["score"]`, `q->completedAt`, `q->resetAt`, plus `dirty.quest_ids` at 229-285 — all before `Begin()` at 287.
- **HandleCompleteQuest** mutates `q->state = QuestState::Completed`, `q->startedAt`, `q->completedAt`, `q->resetAt`, plus `dirty.quest_ids` at 679-684 — all before `Begin()` at 686.

The C1 stale-flag eviction is still the saving recovery path on the NEXT handler call (per the v2 followup's own observation about the original C7 design), so the system is functionally safe. But the C7-A guarantee that "a handler whose Commit throws can no longer build a 'success' response from speculatively mutated state" is **false for these three handlers** — and the Quest handlers in particular return `{"quest", q->ToJson()}` in their response payload from POST-mutation state. If the relational flush throws, the response is built and returned to the client, claiming success on a quest state that didn't persist.

**Fix:** Mechanical relocation of `auto txn = m_ctx.repository->Begin(account);` to the top of each handler, immediately after `getLockedAccount` succeeds. Same pattern as the C7-A v2 fix:
- HandleSetParty: move `Begin()` above line 281.
- HandleReportQuestProgress: move `Begin()` above line 226 (above the `TickQuests::Apply` call too — Tick mutates `dirty.quest_ids` for any quest it advances).
- HandleCompleteQuest: move `Begin()` above line 666 for the same reason.

**Caveat:** `HandleGetQuestState` (QuestHandlers.hpp:74) also doesn't follow the pattern — it reads quest state, builds the response, then calls `Begin/Commit` to flush whatever dirty bits the load-time `TickQuests::Apply` left behind. This one's not as urgent: TickQuests already ran during the prior `LoadAccountFromData` (inside `AccountHydrator::FromData`, which is in turn inside `LoadById`'s pqxx::work), so the mutations are effectively pre-handler. Worth flagging but lower priority.

### H-V3-2. Event-less commits bypass the M-V2-5 advisory lock entirely
**Files:** `Server/Account/src/AccountTransaction.hpp:158-181` (Commit's main body), `Server/Account/src/db/EventStore.hpp:43-44` (the lock site)
**Status:** NEW (gap exposed by M-V2-5's localization to AppendInTx)

The M-V2-5 fix takes `pg_advisory_xact_lock(account_id)` at the top of `EventStore::AppendInTx`. This works for event-sourced commits — but `AccountTransaction::Commit` skips the entire AppendInTx loop when `events_.empty()`. For those commits (HandleSetParty / HandleReportQuestProgress / HandleCompleteQuest / HandleGetQuestState — every non-event-sourced relational flush, plus the load-time TickQuests flush), the per-player advisory lock is never taken. The RelationalFlush UPSERTs run with stripe-lock as their sole concurrency guard.

Single-instance impact is zero — the in-process stripe lock already serializes per player. But the M-V2-5 commentary at EventStore.hpp:35-42 explicitly frames the lock as "defense in depth on top of stripe-lock for multi-instance or stripe-lock-bypass paths." For non-event-sourced flows that depend on the same denorm row (e.g., a future quest action that bumps `accounts.story_xp` without emitting a progression event — the loadout UPSERT under HandleSetParty, the wipe-and-rewrite of `quest_states` / `quest_objectives` under HandleReportQuestProgress), the defense-in-depth claim doesn't hold.

**Fix:** Take the advisory lock at the top of `Commit()` itself, immediately after `EnsureOpen()` and before the events loop — `tx_->exec("SELECT pg_advisory_xact_lock($1)", pqxx::params{account_.GetAccountId()})`. This both (a) makes the lock acquisition unconditional regardless of whether events_ is empty, and (b) removes the per-event re-acquisition cost noted in M-V3-1 below. AppendInTx's existing lock call becomes redundant when invoked through AccountTransaction but stays correct (idempotent within a tx) and is still needed for `EventStore::Append`'s standalone path.

---

## Medium

### M-V3-1. M-V2-5 advisory lock re-acquired per event in multi-event commits
**File:** `Server/Account/src/db/EventStore.hpp:43-44`
**Status:** NEW — perf wart, not a correctness bug

`AccountTransaction::Commit`'s events loop calls `store_.AppendInTx(*tx_, ev)` for each event. Each call issues `SELECT pg_advisory_xact_lock(account_id)` as its first SQL. Postgres treats redundant `pg_advisory_xact_lock` calls within the same transaction as cheap counter increments (the lock is held for the rest of the tx regardless), but each call is a separate round-trip to the DB. A `HandleClaimQuestReward` commit emits 1 claim event + up to 5 wallet events + optionally 1 progression event = up to 7 events = up to 7 extra round-trips per commit on a happy-path RPC. Same per-event waste in HandleMultiPull (2 events = 2 lock SQLs) and HandlePull (2 events = 2 lock SQLs).

**Fix:** Identical to H-V3-2 — hoist the advisory lock to AccountTransaction::Commit so it runs exactly once per commit. AppendInTx can keep its lock call for the `EventStore::Append` standalone path (used by tests and outbox replay), but the per-event path goes through AppendInTx-without-lock when the parent's already locked.

### M-V3-2. EffectDispatcher H10 latent collision unchanged from v2
**Files:** `Server/Account/src/EffectDispatcher.hpp:34, 130`, `Server/Account/src/AccountTransaction.hpp:147-149` (cursor reads in handlers)
**Status:** UNRESOLVED from v2 (H1 in 2026-06-02-followup-event-sourcing.md)

`EffectDispatcher::wallet_version_cursor_` seeds from `account.Dirty().cached_wallet_version` at construction and pre-increments on each `GrantCurrencyEffect`. This produces sequential versions when the dispatcher is the SOLE wallet-event emitter in a commit. But if a future handler queues a wallet event manually (`txn.AppendEvent(walletEv)` with `walletEv.version = dirty.cached_wallet_version + 1`) AND then runs the dispatcher in the same commit, both write `cached + 1` → collision. AppendInTx's in-tx `SELECT MAX(version)` catches it at the second INSERT and throws `ConcurrencyConflict`, so the safety guard fires — but the commit aborts and the player sees a transient error.

Today nothing wires EffectDispatcher into a live handler, so this is latent. The v2 followup noted this same finding and proposed two fixes (a) take a `starting_cursor` parameter on the dispatcher, or (b) centralize cursor advancement inside `AccountTransaction::AppendEvent`. Both are still good options. Option (b) is the cleaner long-term move — it would also let HandlePull / HandleMultiPull / HandleAddCurrency / HandleClaimQuestReward stop computing `cached_*_version + 1` by hand.

### M-V3-3. Cursor read happens AFTER Begin but reads the live (non-snapshot) dirty struct
**Files:** `Server/Account/src/GachaHandlers.hpp:201-203, 545-546`, `Server/Account/src/AccountHandlers.hpp:387`, `Server/Account/src/ProgressionHandlers.hpp:527`, `Server/Account/src/QuestHandlers.hpp:421, 427, 476`
**Status:** NEW — observation; correctness depends on a non-obvious invariant

Every event-sourced handler computes `ev.version` as `dirty.cached_*_version + 1`. The read happens AFTER `Begin()` (so the snapshot was captured first — good for C7-A) but BEFORE the cursor advancement loop in the post-commit catch. The read returns the cursor's CURRENT (live) value, which is unchanged since Begin captured the snapshot — no handler self-increments the cursor.

This is currently correct only because:
1. No handler self-increments `cached_*_version` outside the post-commit catch block.
2. No other thread can mutate `dirty.cached_*_version` (stripe lock).
3. `Begin()` doesn't reset the cursor (Snapshot's `DirtyState dirty` field captures it, then Account's m_dirty stays untouched until ClearDirty in the post-commit catch).

If invariant #1 is ever broken (someone adds a `dirty.cached_wallet_version = X` write in a handler), the next handler's read sees a value that doesn't match what's in the DB, and the next event commit will collide.

**Fix:** Two options:
- Lock the invariant in code: make `cached_*_version` private to `DirtyState` with `friend class AccountTransaction;` so only the catch-block can mutate it. (Plus `friend AccountHydrator/AccountRepository::LoadEventVersions` for the hydration path.)
- Move the cursor read INTO `AccountTransaction::AppendEvent`: handlers stop setting `ev.version` themselves; the transaction picks the next version off its own internal counter. This composes with M-V3-2's option (b).

### M-V3-4. Sibling event keys derive from `claim:Q:A` but include only currency name, not the wallet delta amount or direction
**Files:** `Server/Account/src/QuestHandlers.hpp:446`
**Status:** NEW — H-V2-9 close-but-not-tight

H-V2-9 derives sibling wallet keys as `bundle.claim.idempotency_key + ":wallet:" + cidStr` where `cidStr` is `"credits"` / `"universal_credits"` / etc. Each currency is unique → each sibling key is unique within one claim → no in-commit collision.

But the key shape doesn't encode the delta amount or whether it's an add or spend. A future quest claim that grants both `+100 credits` AND somehow `-50 credits` (e.g., a fee-applied claim or a "swap" pattern) would emit two wallet events with the same currency, both keyed `claim:Q:A:wallet:credits` → the second INSERT trips the events.UNIQUE constraint → ConcurrencyConflict → commit aborts.

This is a latent invariant rather than a live bug — current quest configs grant one direction per currency per claim, and the BuildClaimEvents helper's `emitDelta(before, after)` early-exits on `before == after` so no zero-delta event ever fires. Worth either documenting the invariant ("one wallet delta per currency per claim event") or hardening the key shape (`:wallet:credits:add` / `:wallet:credits:spend`, or just append the wallet event index).

**Fix (cheapest):** suffix the emitDelta index: `bundle.claim.idempotency_key + ":wallet:" + std::to_string(walletV)` (use the local cursor as the disambiguator). Removes the implicit one-direction-per-currency constraint and stays deterministic across retries because `walletV` increments deterministically from the same starting `cached_wallet_version`.

---

## Low / Observation

### L-V3-1. GetPity / GetGuarantee mutate `m_dirty.pity_slots` BEFORE Begin in HandlePull / HandleMultiPull
**Files:** `Server/Account/src/GachaHandlers.hpp:117-118, 364-365`, `Server/Account/src/Account.hpp:188-189, 211` (the GetPity / GetGuarantee implementations)
**Status:** OBSERVATION

`Account::GetPity(slotId, config)` and `Account::GetGuarantee(slotId)` insert into `m_dirty.pity_slots` as a side effect of the non-const lookup. HandlePull calls both at L117-118, four lines before `Begin()` at L140. Strictly speaking, C7-A's "no mutation before Begin" invariant is violated — the snapshot at L140 captures the dirty bit + the freshly-default-initialized PityTracker / GuaranteeTracker entries.

In practice this is harmless:
- The lookup itself only inserts if the slot is missing, so it's a no-op for repeat pulls on an already-active banner.
- The dirty bit is the SAME bit C7-A would set on a successful pull anyway (banner.Pull mutates pity through this reference).
- Rollback restores the snapshot back to a state that has these initialized-but-default trackers, and the next-Save flushes the default tracker state which is a write of "0 / 0 / not-guaranteed" — same as never having the row.

Worth either documenting the "lookup-initialization is the only allowed pre-Begin mutation" exception, or making `GetPity` const + returning the lookup-initialized tracker without setting the dirty bit (let `banner.Pull` set it on actual pity advancement).

### L-V3-2. HandleGetQuestState's Begin/Commit flushes load-time TickQuests dirt; mutations are pre-handler
**Files:** `Server/Account/src/QuestHandlers.hpp:74-107`
**Status:** OBSERVATION

The handler is a pure read, but it calls `Begin()` / `Commit()` to flush whatever dirty bits the load-time `TickQuests::Apply` (inside `LoadAccountFromData` → `AccountHydrator::FromData`) left behind. The C7-A invariant ("Begin before mutation") is not strictly violated because the mutations happened during load, before this handler was even dispatched. But a casual reader sees mutations-via-`TickQuests` running on a returned-by-`GetAll()` quest list AT line 89 and may worry. Worth a one-line comment that the only dirty bits this flush picks up are from the load-time tick, not from this handler.

### L-V3-3. HandleAddCurrency's `balanceBefore` is captured BEFORE Begin
**Files:** `Server/Account/src/AccountHandlers.hpp:361, 368`
**Status:** OBSERVATION

`const std::int64_t balanceBefore = wallet.GetBy(currencyEnum);` at L361 reads the wallet's pre-grant balance four lines before `Begin()` at L368. This is a read, not a mutation, so C7-A is unaffected. The captured value is then used as both the event payload's `balance_before` and (via `+ amount`) the `balance_after`. If a different code path managed to mutate the wallet between L361 and L368 (it can't today — stripe lock + sequential code), the event payload would be wrong. Not a bug; just worth knowing the read happens before the snapshot.

### L-V3-4. `pg_advisory_xact_lock` cast to `(int)` not needed for `std::int64_t` account_id
**File:** `Server/Account/src/db/EventStore.hpp:43-44`
**Status:** OBSERVATION

`pg_advisory_xact_lock(bigint)` is the version Postgres routes when given a single int8 arg. pqxx will type-tag the parameter from `ev.account_id`'s C++ type (`std::int64_t`) correctly. No fix needed; just noting that the overload selection is implicit.

### L-V3-5. `EventStore::Append` (standalone, non-tx variant) is alive only for tests + future outbox replay
**File:** `Server/Account/src/db/EventStore.hpp:86-93`
**Status:** OBSERVATION (carried over from v2 L1)

Same as v2 L1 — `Append` is only invoked by the test surface and by `AppendIdempotent`. Live RPC commits go through `AppendInTx` via `AccountTransaction::Commit`. Worth a comment naming the two valid uses.

### L-V3-6. Commit-failure recovery comment in QuestHandlers references "Rollback restores pre-claim snapshot" — verify the snapshot includes ALL claim-touched fields
**Files:** `Server/Account/src/QuestHandlers.hpp:545-548, 629-633`
**Status:** OBSERVATION

The C7-A comment in HandleClaimQuestReward says "Rollback which restores the pre-claim snapshot and marks the account stale." HandleClaimQuestReward mutates 7+ sub-systems:
- `q->state` / `q->resetAt` (QuestStateStore)
- Wallet (via ApplyClaimRewards → DispatchCurrencyReward)
- StoryLevel / StoryXp (via AddStoryXp inside ApplyClaimRewards)
- DifficultyTier (via AdvanceDifficultyTier)
- LoginStreak (via AdvanceStreakIfNewDay)
- Quest state of OTHER quests (via PropagateClaim modifying their objectives + state)
- Newly-unlocked quests (via UnlockEligibleQuests inserting into QuestStateStore)

The `APHELYON_ACCOUNT_SNAPSHOT_DIRECT_FIELDS` X-macro includes `wallet`, `questStates`, `loginStreak`, `lastStreakDay`, `storyLevel`, `storyXp`, `difficultyTier`, and `dirty`. So Rollback DOES restore everything HandleClaimQuestReward mutates. ✓

Worth a forward-link comment from the X-macro at Account.hpp:96 noting that ClaimQuestReward is the densest user of the snapshot — so any new sub-system added in a future claim refactor must extend the snapshot.

---

## Verified Closed

1. **C7-A — Memento snapshot captures pre-mutation state (for the 5 listed handlers).**
   - HandlePull: `Begin()` at GachaHandlers.hpp:140, BEFORE wallet.TrySpend at 142, RNG advance via banner.Pull at 153, pity/guarantee tracker mutations at 153, RecordPull at 154, collection Dispatch at 179/191. ✓
   - HandleMultiPull: `Begin()` at GachaHandlers.hpp:403, BEFORE the per-pull loop's `account.GetWallet().TrySpendForPullByType` at 411, banner.Pull at 421, RecordPull at 482, collection Dispatch at 497/509. ✓
   - HandleAddCurrency: `Begin()` at AccountHandlers.hpp:368, BEFORE `wallet.AddBy` at 369. ✓
   - HandleLevelCharacter / HandleAscendCharacter / HandleLevelWeapon / HandleAscendWeapon: `Begin()` at ProgressionHandlers.hpp:184 / 273 / 365 / 455, BEFORE `account.GetWallet().SpendScrap` at the very next line + `account.GetCollection().Dispatch` two lines later. ✓
   - HandleClaimQuestReward: `Begin()` at QuestHandlers.hpp:549, BEFORE `q->state = QuestState::Claimed` at 551, ApplyClaimRewards at 554, AdvanceStreakIfNewDay at 564, AdvanceDifficultyTier at 573, PropagateClaim + UnlockEligibleQuests at 581-582. ✓

2. **M-V2-5 advisory lock — wired in AppendInTx.**
   `tx.exec("SELECT pg_advisory_xact_lock($1)", pqxx::params{ev.account_id})` at EventStore.hpp:43-44, inside the same `pqxx::work` as the SELECT MAX + INSERT. Lock auto-released at COMMIT/ROLLBACK. Multiple AppendInTx calls in the same tx re-acquire the same advisory key — Postgres treats this as idempotent (counter increment, no block).
   Deadlock analysis: the lock is keyed by ONLY `account_id`, so no cross-resource lock ordering exists. Two concurrent commits on the same account serialize cleanly at the LOCK call; two on different accounts don't contend. No deadlock risk against the in-process stripe lock either — stripe is held throughout, advisory is taken inside the held stripe, released at COMMIT before stripe.
   Aggressiveness: yes, this serializes wallet + pulls + quest_claims for the same player at the DB layer. But they're ALREADY serialized by the in-process stripe lock, so the lock adds no contention beyond what already exists.

3. **M-V2-6 catch ordering reversed.**
   AccountTransaction.hpp:289-298 advances cursors first (pure-int assignment, can't throw), then calls `account_.ClearDirty()`. `DirtyState::Clear()` (AccountDirty.hpp:52-64) still has its cursor-preservation contract in place defensively, so the new order is robust even if `Clear` is ever refactored.
   No scenario where cursor advance can corrupt state if ClearDirty later throws: cursor bumps to ev.version on the just-committed events are MONOTONIC (each subsequent commit reloads from DB MAX if stale-marked, getting the same or higher values). Even a partial advance followed by ClearDirty throw → MarkStaleForReload → next access reloads from DB → cursors hydrated from MAX → no drift.

4. **H-V2-9 sibling event keys derive from claim's deterministic key.**
   QuestHandlers.hpp:446: `e.idempotency_key = bundle.claim.idempotency_key + ":wallet:" + cidStr;`
   QuestHandlers.hpp:478: `pe.idempotency_key = bundle.claim.idempotency_key + ":progression";`
   Where `bundle.claim.idempotency_key = "claim:" + questId + ":" + std::to_string(account.GetAccountId())` at line 423.
   Collision analysis: across two retries of the same claim, all keys are byte-identical → events.UNIQUE dedups. Within one claim, currency suffixes differ (Currency enum surjects to 5 distinct strings) and progression has its own suffix → no in-commit collision. See M-V3-4 for the latent "one delta per currency per claim" invariant this depends on.
   GachaHandlers Pull at L237/265, MultiPull at L580/606, AccountHandlers AddCurrency at L393, ProgressionHandlers CommitProgressionScrapSpend at L536 — all derive wallet sibling event keys from `scopedKey + ":wallet"` (when scopedKey is non-empty) or fall back to a unique-per-call form otherwise. Consistent with H-V2-9's intent.

5. **M-V2-4 BuildClaimEvents extraction is behavior-preserving.**
   Cross-referenced the new helper at QuestHandlers.hpp:384-484 against the v2-baseline event shape documented in the v2 followup:
   - quest_reward_claimed event: aggregate=QuestClaims, version=`cached_quest_claims_version + 1`, idempotency_key=`"claim:" + questId + ":" + std::to_string(accountId)`. ✓ (matches L421/L423)
   - Per-currency wallet currency_delta events: aggregate=Wallet, version increments local from `cached_wallet_version`, reason="quest_reward", reason_ref=claim event UUID, before/after = pre/post wallet balances. ✓ (matches L427-447)
   - Conditional progression event: aggregate=Progression, version=`cached_progression_version + 1`, event_type=`"story_level_advanced"` if level moved else `"story_xp_gained"`. ✓ (matches L476-480)
   The only behavioral change is the H-V2-9 sibling key derivation, which is intentional.

6. **Per-aggregate cursor consistency — write surface confined to AccountTransaction.**
   `cached_*_version` mutations across the codebase:
   - `AccountTransaction.hpp:293-296`: post-commit catch block (the only +N advance).
   - `AccountRepository.hpp:622-625`: LoadEventVersions hydration from DB MAX.
   - `AccountHydrator.hpp:80-83`: AccountData → Account transcribe at load.
   - `AccountDirty.hpp:60-63`: Clear() preservation contract.
   - Tests only read; no test writes outside these paths.
   No handler self-increments. No read-modify-write race. The post-commit catch's MarkStaleForReload on failure ensures any future divergence is recovered by reload + LoadEventVersions rehydration.

7. **DirtyState preservation across snapshot/restore.**
   The X-macro at Account.hpp:96-116 includes `dirty` as a captured field, so `RestoreFrom` puts the entire DirtyState (including cached_*_version cursors) back. Combined with the C1 stale-mark-on-Rollback belt-and-suspenders, a Rollback recovers cursor state correctly even if `Commit`'s post-commit catch fired (which would have marked stale anyway).

---

## New Assurance from v2→v3 Pass

1. **C7-A as designed for the 5 named handlers.** Pull / MultiPull / AddCurrency / the four progression handlers / ClaimQuestReward all genuinely capture pre-mutation state. A Commit throw on any of these restores wallet, RNG, pity, guarantee, collection, story progression, login streak, difficulty tier, quest states, and the cursor cache to their pre-handler values — the C7 design contract is finally live for these flows.
2. **Multi-event commits chain naturally with the advisory lock.** A HandleClaimQuestReward commit holds the per-player advisory lock from the first AppendInTx call through `tx_->commit()`, so a concurrent commit on the same player blocks at the lock and sees the committed state when it acquires. Per-aggregate cursor monotonicity is enforced at SQL layer (SELECT MAX + check) AND at the stripe layer (single in-process owner) AND at the advisory layer (single DB-wide owner per player).
3. **BuildClaimEvents is now a single grep target for "what does a quest claim emit."** The previous inline 100+ lines were split across the handler body; the helper at 384-484 is a single read for understanding the event shape and the sibling-key derivation. Maintenance win.

---

## Suggested triage order

**This week:**
1. H-V3-1 — relocate `Begin()` to top of HandleSetParty / HandleReportQuestProgress / HandleCompleteQuest. ~5-10 lines each.
2. H-V3-2 + M-V3-1 — hoist `pg_advisory_xact_lock` to AccountTransaction::Commit so it fires once-per-commit regardless of event count. ~5 lines.

**Before launch:**
3. M-V3-3 — make cursor reads transaction-internal (the AppendEvent-assigns-version refactor). Combines with M-V3-2 to fold EffectDispatcher's H10 latent bug into the same correctness story.
4. M-V3-4 — disambiguate sibling wallet event keys by index or direction to remove the implicit one-direction-per-currency invariant.

**Eventually:**
5. L-V3-1 — split `GetPity` into a const lookup + a mutating `MarkPityDirty` path.
6. L-V3-2 — comment HandleGetQuestState's Begin/Commit.
7. L-V3-6 — forward-link X-macro to ClaimQuestReward as the densest snapshot consumer.

---

## Test Coverage Status

Per-aggregate cursor advancement and Rollback-restore of cursors are tested only in `AccountTransactionTest.cpp:64` (post-commit wallet version assertion). No test:
- Exercises HandleSetParty / HandleReportQuestProgress / HandleCompleteQuest under a Commit-throw scenario to catch H-V3-1.
- Validates the advisory-lock behavior (would need two concurrent connections in the test harness).
- Validates BuildClaimEvents byte-for-byte against a known-good golden payload — M-V2-4's "byte-identical to pre-extraction" claim is currently rest-on-review-only.
- Sequentially emits two GrantCurrencyEffect for the same currency in one commit to exercise M-V3-2's latent collision.

These join the v2 backlog. Priority pick for v3: a regression test for H-V3-1 — construct an Account, mutate via SetParty, throw from a mocked RelationalFlush, assert Account state restored.
