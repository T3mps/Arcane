# v4 Follow-up Audit — Event Sourcing + Atomicity
**Date:** 2026-06-03
**Auditor:** Auditor v4 (follow-up to v1 2026-06-02, v2 2026-06-02, v3 2026-06-03)
**Scope:** Verify the v3 remediation arc (commits f46d496..HEAD) for the event-sourcing dimension and surface any new findings.

Sources reviewed:
- v3 synthesis (`2026-06-03-server-persistence-audit-v3.md`)
- v3 per-dimension report (`2026-06-03-v3-followup-event-sourcing.md`)
- v2 synthesis (`2026-06-02-server-persistence-audit-v2.md`) — context only
- Current code under `Server/Account/src/` and `Server/Common/src/State/AccountDirty.hpp`
- v3 remediation commits: 96f0893 (C-V3-2), 15f4330 (H-V3-1), 8a73914 (H-V3-2 + M-V3-1), and 24 supporting commits.

---

## Verdict

The v3 event-sourcing remediation arc landed cleanly. **C-V3-1** (ProgressionReducer `difficulty_tier = 0`) is closed in `ProgressionReducer.hpp:10`; the four-file alignment is annotated in `Account.hpp:526-534`. **C-V3-2** (Begin above GetPity/GetGuarantee) is closed in `GachaHandlers.hpp:124` (HandlePull) and `:369` (HandleMultiPull). **H-V3-1** (Begin scope expansion) is closed for HandleSetParty (`AccountHandlers.hpp:305`), HandleReportQuestProgress (`QuestHandlers.hpp:244`), and HandleCompleteQuest (`QuestHandlers.hpp:725`). **H-V3-2 + M-V3-1** (advisory lock once-per-commit) is closed by hoisting the lock into `AccountTransaction::EnsureOpen` at `AccountTransaction.hpp:396` via `EventStore::AcquireAdvisoryLockInTx`. Per-aggregate cursor invariant is preserved — the write surface remains confined to AccountTransaction::Commit + LoadEventVersions + AccountHydrator + DirtyState::Clear's preservation contract (now documented at `AccountDirty.hpp:38-52`). M-V3-4 sibling-key invariant is documented in code (`QuestHandlers.hpp:481-491`). M-V3-3 cursor-invariant is documented (`AccountDirty.hpp:38-52`).

**One new High emerged** — the H-V3-1 scope expansion missed HandleClaimQuestReward, which calls `TickQuests::Apply` (a mutator) BEFORE `Begin()`. This is the exact defect class H-V3-1 targeted, and the same mechanical fix (relocate `Begin()` above `TickQuests::Apply`) closes it. No new Criticals. M-V3-2 (EffectDispatcher H10 latent collision) remains carry-forward — dispatcher is still dead-coded and the cursor-internal refactor that would close it is not yet started.

---

## CRITICAL

**None.** Every v3 Critical is verified closed.

---

## HIGH

### H-V4-1. `HandleClaimQuestReward` runs `TickQuests::Apply` before `Begin()` — H-V3-1 scope miss
**File:** `Server/Account/src/Handlers/QuestHandlers.hpp:579` (TickQuests::Apply), `:605` (Begin)
**Status:** NEW — H-V3-1 scope-creep gap (same defect class as v3's H-V3-1, just one handler down)

```cpp
TickQuests::Apply(account, m_ctx.questLoader, std::time(nullptr), playerId);  // L579 — MUTATES
// ... read-only validation (L581-586) and pre-snapshot reads (L590-599) ...
auto txn = m_ctx.repository->Begin(account);   // L605 — snapshot captures POST-Tick state
```

`TickQuests::Apply` (TickQuests.hpp:40-132) is a mutator: it `Upsert`s newly-eligible quests via `account.GetQuestStates().Upsert(def.MakeInitialState())` (line 58), flips per-quest state to `Completed` / `Available` via `q->state = ...` (lines 87, 107), and inserts into `account.MutableDirty().quest_ids` (lines 59, 128-129). The Memento snapshot at L605 therefore captures POST-tick state. If `Commit()` throws downstream (RelationalFlush conflict on quest_states UPSERT, event-version race against a future second writer, advisory-lock SQL failure), `RestoreFrom` puts the live Account back to the post-tick point — exactly the C7-A defect the v3 H-V3-1 pass closed for HandleReportQuestProgress + HandleCompleteQuest. Both of those handlers correctly relocated `Begin()` above their `TickQuests::Apply` call (QuestHandlers.hpp:244 / 725); HandleClaimQuestReward was missed.

**Why functional safety holds today:**
- C1 stale-flag eviction on rollback still recovers the next handler invocation (per the v3 followup's same observation about the original C7 design).
- `TickQuests::Apply` is idempotent (its own header comment claims so), so a replay after reload would re-derive the same dirty bits.

**Why it's still a real bug:**
- The pre-claim snapshot reads at L590-599 (`pre.preCredits` etc.) and the `BuildClaimEvents` payload derivation at `:418-540` consume Account state that's been touched by `TickQuests::Apply`. The v3 H-V3-1 design intent for ReportQuestProgress and CompleteQuest was "Rollback restores pre-tick state so the response payload built from post-mutation state never gets returned with a failed Commit." HandleClaimQuestReward builds a response payload at L670-682 from the same post-tick state — same defect.
- The Memento snapshot covers `m_questStates`, `m_dirty`, and the four cached_*_version cursors. If tick auto-completes a different quest (e.g. an Auto-mode login-streak quest tied to the same TickQuests pass) and that auto-complete trips RelationalFlush::FlushQuests on Commit, the live Account has speculatively reported `q->ToJson()` for the claimed quest using post-Commit-failure state.

**Fix:** Mechanical relocation — move `auto txn = m_ctx.repository->Begin(account);` from L605 to immediately after the idempotency cache lookup short-circuit at L578 (above the `TickQuests::Apply` call at L579). Same shape as the v3 H-V3-1 patches to HandleReportQuestProgress / HandleCompleteQuest. The early returns at L581-586 (Quest not started / state error / already claimed / not yet completed) become rollback-on-destruct paths instead of pre-Begin returns; the AccountTransaction destructor's Rollback handles them. The pre-state read block at L590-599 is pure-read and stays where it is.

---

## MEDIUM

### M-V4-1. `HandleClaimQuestReward`'s pre-state read happens AFTER `TickQuests::Apply` but BEFORE `Begin()`
**File:** `Server/Account/src/Handlers/QuestHandlers.hpp:579, 590-599`
**Status:** NEW — observation tightly coupled to H-V4-1

`pre.preStoryLevel`, `pre.preStoryXp`, `pre.preDifficultyTier` (L596-598) are reads of fields TickQuests does NOT touch — story progression doesn't move inside the tick pass. So the reads are correct as written. But `pre.preCredits/preUniversal/preTickets/preLimitedTickets/preScrap` (L591-595) and `q->ToJson()` (L672) are derived from state that COULD have been touched if a future `TickQuests` extension granted login-streak rewards or auto-completed a quest that mutates the wallet via a future side-effect. Today the tick pass is wallet-pure; the invariant ("TickQuests is wallet-pure") is enforceable only by code review.

**Fix:** when H-V4-1 is applied (Begin() above TickQuests::Apply), the pre-state read block moves down to be wall-clock-after `Begin()` and remains a pure read — invariant becomes explicit instead of "we trust TickQuests doesn't move the wallet."

### M-V4-2. `EffectDispatcher`'s H10 latent collision remains unresolved
**Files:** `Server/Account/src/Effects/EffectDispatcher.hpp:34, 130, 149-151`
**Status:** UNRESOLVED carry-forward from v2 H1 + v3 M-V3-2

`wallet_version_cursor_` seeds from `account.Dirty().cached_wallet_version` at construction and pre-increments on each `GrantCurrencyEffect`. Same dual-emitter race documented in v3 M-V3-2: if a handler queues a wallet event manually (`txn.AppendEvent(walletEv)` with `walletEv.version = dirty.cached_wallet_version + 1`) AND then runs the dispatcher in the same commit, both write `cached + 1` → AppendInTx's SELECT MAX catches it on the second INSERT and throws `ConcurrencyConflict`. Latent today because no live handler wires the dispatcher. v3 proposed cursor-internal-to-AppendEvent as the structural fix — not started.

### M-V4-3. Per-aggregate cursor reads still happen AFTER `Begin()` but read the live (non-snapshot) struct
**Files:** `Server/Account/src/Handlers/GachaHandlers.hpp:202-203, 543-544`, `Server/Account/src/Handlers/AccountHandlers.hpp:417`, `Server/Account/src/Handlers/ProgressionHandlers.hpp:527`, `Server/Account/src/Handlers/QuestHandlers.hpp:455, 461, 522`
**Status:** UNRESOLVED carry-forward from v3 M-V3-3

The invariant ("no handler self-increments `cached_*_version`") is now documented in `AccountDirty.hpp:38-52` — a real win, since the constraint becomes grep-discoverable. Compiler enforcement (`friend class AccountTransaction;` privatization, or moving the cursor read into `AppendEvent` itself) is still not done. The doc comment closes the "how would I notice this if it's violated" leg; it doesn't close the "what stops it from being violated" leg.

### M-V4-4. `EventStore::Append` / `AppendIdempotent` still take the advisory lock at `Append` scope, not at the caller's tx scope
**File:** `Server/Account/src/Db/EventStore.hpp:115-121, 133-145`
**Status:** NEW — narrow scope, minor consistency wart

H-V3-2 + M-V3-1 hoisted the advisory lock from `AppendInTx` to `AccountTransaction::EnsureOpen` for the live-RPC path. The standalone `Append` (used by tests + `AppendIdempotent`) acquires its own connection + tx and calls `AcquireAdvisoryLockInTx` inline before `AppendInTx`. This is correct, but `AppendIdempotent`'s conflict-recheck branch (L137-144) opens a *second* `pqxx::work` to query for the duplicate event and DOES NOT take the advisory lock on the second tx — a second writer could insert the same idempotency_key between the throw and the recheck SELECT. The recheck only reads for dedup confirmation, so the only side-effect is incorrectly re-throwing (instead of swallowing) a real idempotency conflict that landed during the recheck window. Live-path callers don't reach `AppendIdempotent` (live commits go through AccountTransaction), so this is hard to trigger.

**Fix:** acquire the advisory lock on the recheck tx too, OR (simpler) collapse the recheck SELECT onto the same tx as `Append` — already-aborted-but-not-released — and reuse its lock. Lowest priority of the four mediums.

---

## LOW / OBSERVATION

### L-V4-1. v3 L-V3-2 carries forward — `HandleGetQuestState`'s Begin/Commit is still empty-relative-to-handler
**File:** `Server/Account/src/Handlers/QuestHandlers.hpp:102-103`
**Status:** OBSERVATION, no change

The empty `txn.Commit()` after a pure-read handler still flushes whatever `TickQuests::Apply` (from the load-time pass inside `AccountHydrator::FromData`) left as dirty bits. The handler itself doesn't mutate, so the C7-A invariant holds in a different way ("Begin captures the post-load state, which is the pre-handler state" — same point). Not changed; not a defect.

### L-V4-2. v3 L-V3-3 carries forward — `HandleAddCurrency`'s `balanceBefore` reads BEFORE `Begin()`
**File:** `Server/Account/src/Handlers/AccountHandlers.hpp:391` (read) → `:398` (Begin)
**Status:** OBSERVATION, doc added at L382-390

Acknowledged in-source via the L-V3-3 doc-comment. Pure read; safe under stripe lock + sequential code. Carry-forward observation.

### L-V4-3. v3 L-V3-5 carries forward — `EventStore::Append` standalone variant
**File:** `Server/Account/src/Db/EventStore.hpp:106-121`
**Status:** OBSERVATION, doc added at L106-114

Comment now names the two valid callers (integration tests + AppendIdempotent). Carry-forward win.

### L-V4-4. v3 L-V3-6 carries forward — X-macro doc-link to HandleClaimQuestReward
**File:** `Server/Account/src/State/Account.hpp:103-110`
**Status:** OBSERVATION, doc added

Forward-link from the snapshot X-macro to ClaimQuestReward as the densest user is in place. Carry-forward win.

### L-V4-5. `events.event_type` is still log/analytics-only; reducer dispatch is by C++ type
**File:** `Server/Account/src/Handlers/QuestHandlers.hpp:523-533`
**Status:** OBSERVATION, doc added at L523-532

The "event_type is diagnostic, not dispatch" invariant is now annotated where the `story_level_advanced` / `story_xp_gained` vocab split lives. Carry-forward note (M-V3-1 cross-cutting from v3). If a future event introduces a payload shape change, the comment instructs adding a reducer overload + explicit dispatch.

### L-V4-6. `GetPity` / `GetGuarantee` still flip `m_dirty.pity_slots` on first lookup
**File:** `Server/Account/src/State/Account.hpp:205, 222`
**Status:** OBSERVATION, no change

C-V3-2's fix was to move `Begin()` above the lazy-mutating accessor (per Account.hpp:117-123 in HandlePull / 364-367 in HandleMultiPull). The structural alternative the v3 followup considered ("split GetPity into const lookup + mutating MarkPityDirty") is not done. The current placement is defensible — both pity and guarantee are about to be advanced by `banner.Pull` anyway, so flipping the dirty bit is correct semantically. The latent risk (a future caller that reads pity without then advancing it would falsely mark the slot dirty) remains.

---

## Verified Closed from v3

| Item | Source | Where verified |
|---|---|---|
| C-V3-1 — `ProgressionReducer` default tier 1 → 0 | `2026-06-03-server-persistence-audit-v3.md:48-55` | `Server/Account/src/Reducers/ProgressionReducer.hpp:10` (`difficulty_tier = 0`); `Server/Account/src/State/Account.hpp:526-534` (cross-file traceability comment); commit 8c53a34 |
| C-V3-2 — Begin() above GetPity/GetGuarantee in HandlePull | `2026-06-03-server-persistence-audit-v3.md:57-71` | `Server/Account/src/Handlers/GachaHandlers.hpp:124` (Begin) precedes `:126-127` (GetPity / GetGuarantee); comment at `:117-123` cites C-V3-2; commit 96f0893 |
| C-V3-2 — same for HandleMultiPull | `2026-06-03-server-persistence-audit-v3.md:57-71` | `Server/Account/src/Handlers/GachaHandlers.hpp:369` (Begin) precedes `:371-372` (GetPity / GetGuarantee); comment at `:364-368`; commit 96f0893 |
| H-V3-1 — HandleSetParty Begin scope | `2026-06-03-v3-followup-event-sourcing.md:24-46` | `Server/Account/src/Handlers/AccountHandlers.hpp:305` (Begin) precedes `:307-309` (SetParty / SetWeaponEquipment / SetGearEquipment); comment at `:295-304`; commit 15f4330 |
| H-V3-1 — HandleReportQuestProgress Begin scope | `2026-06-03-v3-followup-event-sourcing.md:24-46` | `Server/Account/src/Handlers/QuestHandlers.hpp:244` (Begin) precedes `:246` (TickQuests::Apply) and all subsequent quest mutations; comment at `:238-243`; commit 15f4330 |
| H-V3-1 — HandleCompleteQuest Begin scope | `2026-06-03-v3-followup-event-sourcing.md:24-46` | `Server/Account/src/Handlers/QuestHandlers.hpp:725` (Begin) precedes `:727` (TickQuests::Apply) and `:730-744` (Upsert / state mutations); comment at `:723-724`; commit 15f4330 |
| H-V3-2 — advisory lock empty-Commit bypass | `2026-06-03-v3-followup-event-sourcing.md:48-57` | `Server/Account/src/Cache/AccountTransaction.hpp:396` (`db::EventStore::AcquireAdvisoryLockInTx` called once in `EnsureOpen`); EventStore comment at `:29-46` documents the migration; commit 8a73914 |
| M-V3-1 — per-event advisory lock perf wart | `2026-06-03-v3-followup-event-sourcing.md:62-68` | Same as H-V3-2 — lock is now acquired exactly once per commit at `AccountTransaction.hpp:396`; AppendInTx (`EventStore.hpp:51`) no longer calls the lock SQL; commit 8a73914 |
| M-V3-3 — cursor-read invariant documented | `2026-06-03-v3-followup-event-sourcing.md:78-93` | `Server/Common/src/State/AccountDirty.hpp:38-52` carries the M-V3-3 doc-comment naming AccountTransaction::Commit as sole writer + invariant text; commit 21256f5 |
| M-V3-4 — sibling key invariant documented | `2026-06-03-v3-followup-event-sourcing.md:95-105` | `Server/Account/src/Handlers/QuestHandlers.hpp:481-491` ("one delta per currency per claim" + explicit instructions to extend the key shape before adding a swap-pattern claim); commit eaac521 |
| L-V3-1 — RestoreFrom catch silent | `2026-06-03-v3-followup-event-sourcing.md:111-122` (carry-from-v2) | `Server/Account/src/Cache/AccountTransaction.hpp:347-357` now logs RestoreFrom throws (LOG_DATA_WARN) before falling back to stale-mark; commit 21256f5 |
| L-V3-3 — `balanceBefore` reads pre-Begin | `2026-06-03-v3-followup-event-sourcing.md:131-134` | Doc-comment present at `Server/Account/src/Handlers/AccountHandlers.hpp:382-390`; commit 21256f5 |
| L-V3-5 — `EventStore::Append` callers | `2026-06-03-v3-followup-event-sourcing.md:142-146` | Doc-comment at `Server/Account/src/Db/EventStore.hpp:106-114` names the two valid callers; commit 21256f5 |
| L-V3-6 — X-macro forward-link to ClaimQuestReward | `2026-06-03-v3-followup-event-sourcing.md:148-163` | Doc-comment present at `Server/Account/src/State/Account.hpp:103-110`; commit 21256f5 |
| Per-aggregate cursor invariant — write surface confined | `2026-06-03-v3-followup-event-sourcing.md:199-209` | `grep cached_(wallet\|pulls\|quest_claims\|progression)_version =` surfaces exactly 4 write sites: `AccountTransaction.hpp:293-296` (Commit advance), `AccountRepository.hpp:622-625` (LoadEventVersions), `AccountHydrator.hpp:80-83` (load transcribe), `AccountDirty.hpp:76-79` (Clear preservation). No handler self-increments. |
| DirtyState preservation across snapshot/restore | `2026-06-03-v3-followup-event-sourcing.md:208-209` | X-macro at `Server/Account/src/State/Account.hpp:112-132` captures `dirty` directly — Snapshot field on L132 |
| Snapshot covers ClaimQuestReward sub-systems | `2026-06-03-v3-followup-event-sourcing.md:148-163` | X-macro covers wallet, questStates, loginStreak, lastStreakDay, storyLevel, storyXp, difficultyTier, dirty (Account.hpp:113-132); confirms L-V3-6 audit |
