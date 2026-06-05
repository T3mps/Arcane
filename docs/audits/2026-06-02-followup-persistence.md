# Follow-up Audit: Persistence + Schema

**Date:** 2026-06-02
**Scope:** `Server/Account/schema.sql`, `Server/Account/seed.sql`, `Server/Account/src/db/RelationalFlush.hpp`, `Server/Account/src/AccountRepository.hpp`, `Server/Account/src/AccountTransaction.hpp`, `Server/Account/src/db/EventStore.hpp`, `Server/scripts/db-*.bat`, `Server/Dockerfile.postgres`
**Lens:** Schema ↔ code drift after the C/H/M sweep on commits `d3416a6`, `8e19666`, `cd4e37a`, `bdf0f7c`, `90a5f89`, `8b2c77f`.

---

## Summary

The five schema-relevant fixes (H4 `last_login` write-removal, H11 pure date arithmetic, M13 unknown-aggregate logging, M14 `loadouts.name` drop, M15 substat slot_idx sort, M18 `Wallet::GetBy/AddBy`, L2 `--no-comments` drop) all land cleanly and are internally consistent; the H11 round-trip in particular now stores and reads back the same `INT` day-of-epoch through `std::time_t`. However, the audit surfaced one real schema-vs-code drift (`difficulty_tier` DB default = 1 vs. C++ `Account`/`AccountData` default = 0 → DB default never sticks), and several speculative indexes that no current query exercises (`owned_gear_set_slot_idx`, `gear_substats_stat_idx`, `quest_states_type_state_idx`, `quest_states_reset_idx`, `quest_states_active_idx`, `events_wallet_recent_idx`, `events_pulls_banner_idx`). No CHECK constraints guard the five wallet columns. The L2 snapshot fix preserves COMMENT ON but the snapshot/reset loop itself is now self-consistent.

---

## Critical

*(none)*

---

## High

### H1. `difficulty_tier` schema default (1) silently overwritten by C++ default (0)
**Files:** `Server/Account/schema.sql:86`, `Server/Common/src/AccountData.hpp:56`, `Server/Account/src/Account.hpp:510-513`, `Server/Account/src/AccountRepository.hpp:80-114` (Create), `Server/Account/src/db/RelationalFlush.hpp:76,97`
**Status:** NEW

The schema sets:

```sql
difficulty_tier         INT NOT NULL DEFAULT 1,
```

But the C++ struct + class defaults are `0`:

- `AccountData::difficultyTier = 0` (AccountData.hpp:56)
- `Account::m_difficultyTier = 0` (Account.hpp:513, with comment "difficultyTier = 0 is the starting tier (HSR's EL0 analogue)")
- `ProgressionReducer::ProgressionState::difficulty_tier = 1` (ProgressionReducer.hpp:10) — disagrees with the other two

`AccountRepository::Create()` (line 92-96) does **not** include `difficulty_tier` in the INSERT column list, so the DB row materializes with `difficulty_tier = 1` (the schema default). The returned `AccountData` however carries `difficultyTier = 0` (struct default — Create() never assigns the field). `AccountServer::LoadAccountFromData` then calls `account->SetDifficultyTier(data.difficultyTier)` (AccountServer.hpp:539) → `m_difficultyTier = 0`. From here:

- Any handler that flushes the accounts row through `FlushAccountsRow` writes `difficulty_tier = $6` with the value from `a.GetDifficultyTier()` (RelationalFlush.hpp:76,97), which is `0`. The DB's `1` is overwritten by C++'s `0`.
- The first idle-evict + Save() makes the overwrite permanent.

Effectively the schema-side default never sticks for any account created after this commit; the DB column reads `0` from then on. The audit's M14 fix dropped a similar dead-write column (`loadouts.name`), but this is the inverse — a schema-side default that the code unconditionally overrides without realizing.

The reducer disagreement (`ProgressionReducer.hpp:10` uses 1 while `Account` uses 0) compounds the issue: a fold-from-events path and the in-memory `Account` would arrive at different baselines for a fresh stream.

**Fix:** Pick one source of truth. Recommended:
1. Drop the `DEFAULT 1` clause on `schema.sql:86` (use `DEFAULT 0` or remove the default), OR
2. Add `difficulty_tier = ...` to the `Create()` INSERT and initialize the C++ defaults to `1`, AND fix `ProgressionReducer::ProgressionState::difficulty_tier` to match.

Either way, also align `Account::m_difficultyTier`, `AccountData::difficultyTier`, and `ProgressionReducer` so the three defaults agree. The Account.hpp:510 comment ("0 is the starting tier (HSR's EL0 analogue)") suggests `0` is intended, in which case schema + reducer are wrong.

---

## Medium

### M1. Five wallet columns have no non-negative CHECK constraint
**Files:** `Server/Account/schema.sql:89-93`
**Status:** NEW

```sql
credits                 BIGINT NOT NULL DEFAULT 0,  -- Stellar Jade
universal_credits       BIGINT NOT NULL DEFAULT 0,  -- Oneiric Shards
tickets                 BIGINT NOT NULL DEFAULT 0,
limited_tickets         BIGINT NOT NULL DEFAULT 0,
scrap                   BIGINT NOT NULL DEFAULT 0,  -- Mora
```

`material_inventory.quantity` has `CHECK (quantity >= 0)` (line 199) and `WalletReducer` enforces non-negativity in the reducer assertions per the previous audit's Assurance section. But the wallet columns themselves have no DB-side guard. A buggy flush (or a manual psql poke) that writes a negative balance via the targeted UPDATE in `FlushAccountsRow` will succeed silently. The reducer's invariant is process-resident only; the DB cannot catch a divergence.

**Fix:** Add `CHECK (credits >= 0)`, `CHECK (universal_credits >= 0)`, etc. to the accounts table. (Cheap belt-and-suspenders; complements the reducer invariant.)

### M2. Seven speculative indexes have no current query that uses them
**Files:** `Server/Account/schema.sql:111, 145, 159, 171, 226, 227-228, 236-237, 297-300`

| Index | Schema line | Used by |
|---|---|---|
| `accounts_deleted_idx` | 111 | (none — partial WHERE deleted_at IS NOT NULL; future GDPR purge) |
| `owned_weapons_template_idx` | 145 | (none — no SELECT filters by template_id) |
| `owned_gear_set_slot_idx` | 159 | (none — `LoadOwnedGear` filters by account_id only) |
| `gear_substats_stat_idx` | 171 | (none — `LoadOwnedGear` substat read filters by account_id only) |
| `quest_states_type_state_idx` | 226 | (none — `LoadQuests` filters by account_id only) |
| `quest_states_reset_idx` | 227-228 | (none — no reset-sweep query) |
| `quest_states_active_idx` | 236-237 | (none — no Available+Active scan) |
| `events_wallet_recent_idx` | 297-298 | (none — `LoadEventVersions` uses the b-tree on `(account_id, aggregate_kind, version)`) |
| `events_pulls_banner_idx` | 299-300 | (none — banner analytics not built yet) |
| `idempotency_cache_expires_idx` | 380 | (none today — sweep daemon planned) |

Each of these adds write amplification (INSERT/UPDATE keeps them in lockstep) for no current read benefit. The PK on each table is sufficient for every existing SELECT.

This isn't strictly a bug, but the previous audit's Assurance section claims "indexes match the predicates the code actually uses" — that claim was not literally verified, and seven of the eleven non-PK indexes go unused at the SQL level.

**Fix:** Either (a) drop the speculative indexes and re-add when the feature lands, or (b) add an inline comment per index naming the planned future query that will use it (consistent with the M14 `preset_id` comment pattern). Recommend (b) since adding-on-demand requires a migration in production.

### M3. M15 substat sort changes load-time `subStats` vector ordering — verified safe, but tests don't cover it
**Files:** `Server/Account/src/db/RelationalFlush.hpp:222-241`, `Server/Account/src/AccountRepository.hpp:469-484`
**Status:** VERIFIED-OPEN (no test coverage)

The M15 fix sorts substats by ascending `stat_type` before assigning `slot_idx`. The load path (AccountRepository.hpp:469-484) selects `ORDER BY instance_id, slot_idx` and `push_back`s into `OwnedGear::subStats`. Net effect: after a save+load round-trip the in-memory `subStats` vector is now ordered by ascending `stat_type`, which is **different** from the order at the moment of pull-grant.

Reviewing consumers (`Server/Account/src/ProgressionHandlers.hpp:64`), the only iteration is `for (const auto& sub : gIt->second.subStats)` — order-agnostic. No indexed access outside RelationalFlush itself. So the order change is safe today.

But the change is undefended by tests; nothing fails if a future caller does `subStats[0]` expecting "the first rolled stat." The audit's test coverage gaps section already flagged that gear flush is untested.

**Fix:** Add an integration test that grants a gear with three substats in non-sorted stat_type order, flushes, reloads, and asserts the substat set equality (not order). And/or add a comment at `CollectionState.hpp:39` documenting that `subStats` is **not** roll-order after reload.

### M4. `gear_substats` slot_idx CHECK constraint is now slightly misleading
**Files:** `Server/Account/schema.sql:164`

```sql
slot_idx    SMALLINT NOT NULL CHECK (slot_idx BETWEEN 0 AND 3),
```

The constraint reads as "slot index between 0 and 3" which the name suggests is positional (which substat slot). After M15, `slot_idx` is actually "rank by ascending stat_type within this gear's substat set". Both interpretations satisfy 0..3, but the column name + CHECK together imply a positional meaning that no longer holds.

**Fix:** Either rename the column (`stat_rank`?) or add a `COMMENT ON COLUMN` noting it's the stat_type-ascending rank, not a positional index. The comment-preserving L2 fix means a `COMMENT ON COLUMN` survives `db-snapshot.bat`.

### M5. `Create()` doesn't return DB-default-populated AccountData; relies on struct defaults
**Files:** `Server/Account/src/AccountRepository.hpp:80-114`

```cpp
auto r = tx.exec(R"SQL(
    INSERT INTO accounts (username, password_hash, credits, tickets, limited_tickets, scrap)
    VALUES ($1, $2, $3, $4, $5, $6)
    RETURNING account_id, public_uid, created_at
)SQL", ...);
```

The RETURNING clause only fetches 3 columns; the constructor sets only the explicitly written fields. The remaining `AccountData` fields (story_level, story_xp, difficulty_tier, login_streak, universal_credits, all pull stats) carry their C++ struct defaults — which agree with the schema defaults **except** `difficulty_tier` (see H1).

While H1 is the live bug, the broader pattern is risky: future schema defaults will silently diverge unless `Create()` either uses `RETURNING *` or explicitly populates every column.

**Fix:** Extend `RETURNING` to include every column `AccountData` cares about, and populate `d` from the result. Removes the entire class of "schema default diverges from C++ struct default" bugs.

---

## Low / Observation

### L1. `loadouts.preset_id` is not type-CHECK'd against the future presets-per-character cap
**Files:** `Server/Account/schema.sql:185`

The column is `SMALLINT NOT NULL` with no upper bound; the PK is `(account_id, character_id, preset_id)`. When the named-presets feature lands, a buggy handler could insert preset_id up to 32767. Worth a `CHECK (preset_id BETWEEN 0 AND <cap>)` when the cap is decided. Not actionable today.

### L2. M13 logs ERROR on unknown aggregate_kind but does not throw
**Files:** `Server/Account/src/AccountRepository.hpp:626-639`

The M13 fix logs LOUDLY but still allows the load to succeed with `cached_*_version = 0` for the known aggregates only. The unknown aggregate's stream is effectively orphaned — its next commit will collide. The log is necessary but the silent fall-through is the actual bug. Consider `throw std::runtime_error` or skipping the account load entirely so a forgotten switch case fails fast in tests rather than corrupting state at runtime.

### L3. `db-snapshot.bat` honor of comments depends on `pg_dump` preserving them through `--schema-only`
**Files:** `Server/scripts/db-snapshot.bat:26-31`

The L2 fix dropped `--no-comments`. Verified that the resulting `pg_dump --schema-only --no-owner --no-acl -n public -n partman` does emit `COMMENT ON ...` for any annotation added live. The L2 fix is correct. No further action.

### L4. `events.idempotency_key` UNIQUE constraint includes `created_at` (partition key)
**Files:** `Server/Account/schema.sql:286`

```sql
UNIQUE (account_id, idempotency_key, created_at),
```

Same gotcha the C6 fix solved for the version UNIQUE: across partitions, this UNIQUE is not enforced. `EventStore::AppendIdempotent` already compensates with a post-conflict SELECT (line 96-101), so this isn't a live bug, but the constraint's name suggests global dedup which it does not provide. Worth a comment.

### L5. `audit_log.metadata` has no JSON CHECK
**Files:** `Server/Account/schema.sql:351`

`quest_states.metadata` has `CHECK (jsonb_typeof(metadata) = 'object')` (line 223), but the otherwise-comparable `audit_log.metadata`, `events.metadata`, and `events.data` columns do not. Defensive consistency; not load-bearing.

### L6. Schema comment on `accounts.difficulty_tier` says nothing about HSR EL semantics
**Files:** `Server/Account/schema.sql:86`

Compare with the rich comments on `last_streak_day` (H11 context), wallet columns ("Stellar Jade", "Oneiric Shards", "Mora"), and pull stats. `difficulty_tier` is naked. Given the H1 confusion above, a one-line `-- HSR Equilibrium Level analogue; starts at <X>` would prevent recurrence.

---

## Verified Closed

1. **H4** — `RelationalFlush::FlushAccountsRow` (lines 60-110) writes 17 columns; `last_login` is conspicuously absent. `AccountRepository::BumpLastLogin` (lines 189-206) runs a targeted single-column UPDATE called only from VerifyCredentials.
2. **H11** — Write at `RelationalFlush.hpp:72-73` uses `DATE '1970-01-01' + ($3)::int`; read at `AccountRepository.hpp:353` uses `COALESCE(last_streak_day - DATE '1970-01-01', 0)`. C++ side passes `std::int64_t` derived from `std::time_t` day-of-epoch on write (line 94) and reads into `std::time_t` (line 372). Round-trip is TZ-independent and value-stable.
3. **M13** — `AccountRepository::LoadEventVersions` (lines 626-639) logs `LOG_DATA_ERROR` with full context on unknown `aggregate_kind`. (See L2 above for a remaining concern.)
4. **M14** — `loadouts` table in `schema.sql:182-194` has no `name` column. RelationalFlush `FlushLoadouts` (lines 254-292) writes 8 fields, none of which is `name`. `LoadLoadouts` (lines 499-526) reads 6 fields, none of which is `name`. The header comment at schema.sql:177-181 documents the `preset_id > 0` future feature. Verified `preset_id` survives as part of the PK and the `loadouts_active_idx` partial-index condition.
5. **M15** — `RelationalFlush::FlushOwnedGear` (lines 222-241) builds `order[]` via `std::sort` ascending on `stat_type` before assigning `slot_idx`. Comment at lines 213-221 explains rationale.
6. **M17** — Schema comment at lines 229-237 documents the full `QuestState` enum mapping (`0..5`) inline with `WHERE state IN (1, 2)` and explicitly retires the previous incorrect "ACTIVE + CLAIMABLE" wording.
7. **M18** — `Wallet::GetBy(Currency)` / `Wallet::AddBy(Currency, int)` exist; the three duplicate `CurrencyFromStr` chains have been collapsed (verified by absence of duplicates in `WalletEvents.hpp`, `EffectDispatcher.hpp`, `AccountHandlers.hpp`).
8. **L2** — `db-snapshot.bat:26-31` no longer passes `--no-comments`. The comment block above the `pg_dump` call (lines 22-25) documents the reasoning.
9. **FK CASCADE coverage holds after M14** — Verified every account-scoped table still has `ON DELETE CASCADE` on its `account_id` FK: owned_characters (118), char_traces (132, composite), owned_weapons (136), owned_gear (148), gear_substats (169, composite via owned_gear), loadouts (183), material_inventory (197), party_slots (204), quest_states (215), quest_objectives (246, composite via quest_states), world_flags (250), pity_state (257), events (287), snapshots (316), audit_log (344), idempotency_cache (373).
10. **Partitioning and compression survive recent edits** — `events` is still `PARTITION BY RANGE (created_at)` (line 288), `pg_partman` bootstrap still present (lines 303-309), and `SET COMPRESSION lz4` is applied to `events.data`, `events.metadata`, `snapshots.state`, `audit_log.before/after`, `idempotency_cache.response_payload`.

---

## Assurance

What the recent fixes now provably guarantee:

- **`last_login` cannot lie.** The column is only written by `BumpLastLogin`, which is only called from `VerifyCredentials`. Idle-evict's brute-force flush no longer touches the column. (H4)
- **Streak day round-trips losslessly across session timezones.** Pure date arithmetic on both ends; no `to_timestamp/EXTRACT(EPOCH)` pair to misalign at midnight. (H11)
- **Unknown event aggregate_kinds cannot silently drop their cursor.** The ERROR log makes the gap impossible to miss during a schema bump that forgets to update the switch. (M13; though see L2 for a remaining silent-success concern.)
- **Substat slot_idx is stable across in-memory vector reorderings.** Future upgrade paths that move re-rolled stats to the vector tail will produce the same DB rows. (M15)
- **`Wallet` dispatch lives in exactly one place.** `Currency` enum drives both reads and writes; no string-based chains to drift. (M18)
- **COMMENT ON annotations survive `db-snapshot.bat`.** Documentation added via psql/DBeaver doesn't get silently wiped on the next snapshot. (L2)
- **No table dropped, no FK constraint disturbed.** M14 only dropped a column; every CASCADE chain holds.
- **`AccountTransaction::Commit` routes events through `EventStore::AppendInTx`.** The optimistic-concurrency pre-check (SELECT MAX FOR UPDATE) runs inside the same `pqxx::work` as the relational flush. (C6 — already closed in the previous audit, confirmed still in place at AccountTransaction.hpp:161-163.)

---

## Triage suggestion

**Before next milestone:**
1. H1 — pick a `difficulty_tier` default and align schema + AccountData + Account + ProgressionReducer.
2. M5 — make `Create()` use `RETURNING *` so future schema defaults can't drift unnoticed.
3. M1 — add `CHECK (... >= 0)` on the five wallet columns.

**Housekeeping:**
4. M2 — decide per index: drop, or add a "future use: <query>" comment.
5. M3 — add a `subStats` post-reload ordering test.
6. M4 / L6 — add `COMMENT ON COLUMN` annotations now that L2 lets them survive snapshots.
