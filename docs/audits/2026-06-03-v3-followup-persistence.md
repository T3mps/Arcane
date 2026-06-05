# v3 Follow-up Audit: Persistence + Schema

**Date:** 2026-06-03
**Scope:** `Server/Account/schema.sql`, `Server/Account/seed.sql`, `Server/Account/src/AccountRepository.hpp`, `Server/Account/src/db/RelationalFlush.hpp`, `Server/Account/src/db/ConnectionPool.hpp`, `Server/Account/src/AccountHydrator.hpp`, `Server/Account/src/AccountTransaction.hpp`, `Server/Account/src/db/EventStore.hpp`, `Server/Account/src/db/OutboxRelay.hpp`, `Server/Common/src/AccountData.hpp`, `Server/Account/src/Account.hpp`, `Server/Account/src/reducers/ProgressionReducer.hpp`
**Lens:** Schema ↔ code drift after the v2 remediation pass (commits `18b6627` through `d163b07`). 22 fixes shipped since `8e19666`; this audit verifies them and re-scans for residual drift.

---

## Summary

H-V2-2's headline fix — schema `difficulty_tier DEFAULT 0` to match C++ — landed correctly in `schema.sql:93`, `AccountData.hpp:56`, `Account.hpp:511`, and the load path. **But the reducer side of the fix is still wrong**: `ProgressionReducer::ProgressionState::difficulty_tier = 1` (line 10) was flagged in both v1's audit and v2 H-V2-2's "Recommended fix" bullet, and was not updated when the schema/struct/class side was. This is a CRITICAL — a fresh-stream fold through the reducer reports tier=1 while the live Account reports tier=0, so any code path that consults the reducer state (replay, audit, snapshot rebuild) will disagree with the authoritative Account. The audit also surfaces three new High findings: events queries never include a `created_at` predicate (no partition pruning ever — every LoadEventVersions/AppendInTx scans 60 monthly partitions in a 5-year retention), the outbox table has no cleanup path (dispatched rows accumulate forever in production), and the idempotency_cache has no sweep daemon despite v1 H2 + schema comment promising one. Existing dev-DB rows with `difficulty_tier=1` are preserved by the schema change (DEFAULT only applies on INSERT) — the next FlushAccountsRow will normalize them to whatever C++ says. AccountHydrator (M-V2-3) is verified to populate every Account field that LoadByAccountId selects, with one observation about `m_storeIdempotency_storeAt_metadata` having no equivalent column.

---

## Critical

### C1. `ProgressionReducer::ProgressionState::difficulty_tier` still defaults to 1
**Files:** `Server/Account/src/reducers/ProgressionReducer.hpp:10`
**Status:** NEW (regression of v2 H-V2-2 — the Recommended fix in v2-followup explicitly called out fixing this reducer line, and the fix on commit `18b6627` only updated the schema + AccountData + Account but left the reducer)

```cpp
struct ProgressionState {
    int story_level     = 1;
    int story_xp        = 0;
    int difficulty_tier = 1;   // <-- disagrees with everyone else
};
```

Every other site agrees on `0`:
- `schema.sql:93` — `difficulty_tier INT NOT NULL DEFAULT 0`
- `AccountData.hpp:56` — `int difficultyTier = 0`
- `Account.hpp:511` — `int m_difficultyTier = 0`
- `Account.hpp:507-508` comment — `"difficultyTier = 0 is the starting tier (HSR's EL0 analogue)"`

A fresh ProgressionState fold (from an account with no `progression`-aggregate events yet) yields `difficulty_tier=1` while the live `Account` carries `0`. The two are reconciled only when an event with `difficulty_tier_unlocked` actually fires — until then any consumer of `ProgressionState` (replay-from-snapshot, audit, future projection rebuild path) reports the wrong starting tier.

`ProgressionReducerTest.cpp:25` seeds `s.difficulty_tier = 1` and asserts post-event tier == 2, so the test passes with the wrong default and provides no regression coverage. **The test is part of the reason this regressed** — anyone updating the reducer would see the test pass and assume the default was correct.

**Why this is Critical not High:** the four event-sourced aggregates are the system's source of truth for replay and audit. The denormalized accounts row is a read projection that drifts only briefly under failure. A reducer that disagrees with its own projection is a quiet correctness bug that nobody hits today because no code path actually folds progression events from genesis at runtime — but the moment a snapshot rebuild, an audit ticket, or any future event-replay tooling runs it, the reported starting tier is wrong.

**Fix:**
1. `ProgressionReducer.hpp:10` — change `difficulty_tier = 1` to `difficulty_tier = 0`.
2. `ProgressionReducerTest.cpp:25-30` — update the test to either start from `s.difficulty_tier = 0` and assert advance to 1, OR add a separate test asserting the default-constructed state matches `Account::m_difficultyTier` default.

---

## High

### H1. `events` queries never include `created_at` predicate — no partition pruning
**Files:** `Server/Account/src/db/EventStore.hpp:52-55, 113-114, 124-132, 156-158`, `Server/Account/src/AccountRepository.hpp:613-618`
**Status:** NEW

`events` is `PARTITION BY RANGE (created_at)` with monthly partitions managed by pg_partman. Every query against the table filters by `(account_id, aggregate_kind[, version, idempotency_key])` — none include any `created_at` predicate. Postgres' partition pruner can only prune when the query references the partition key, so every SELECT scans every existing monthly partition (with the appropriate per-partition index probe).

At launch this is fine (one partition). After 12 months it's a 12× index-probe fan-out per query. After 5 years (retention horizon for audit) it's 60×. Each `LoadEventVersions` does `GROUP BY aggregate_kind`, so 60 index probes × 4 aggregates ≈ 240 random reads per account hydrate. Each `AppendInTx` does a `SELECT MAX(version)` pre-check, paid on every event-sourced commit.

This is latent — the per-partition index on `(account_id, aggregate_kind, version)` makes each probe O(log n) — but the constant factor grows linearly with partition count and there's no plan to cap it. With `pg_partman` retention dropping old partitions (not currently configured — see H3), the count would stabilize; without retention, every commit gets slower forever.

**Fix:** Two complementary options:
1. Per-aggregate `cached_*_version` cursors already exist on accounts (denormalized). The `SELECT MAX(version)` pre-check in `AppendInTx` could trust the cursor (sourced from `LoadEventVersions` at load time + bumped by Commit) instead of re-querying. The advisory_xact_lock from M-V2-5 already serializes commits at the DB layer; trusting the cursor under that lock is safe.
2. For the actual MAX-query at load time, add a `created_at >= now() - interval '7 days'` predicate (or wherever the highest version is guaranteed to live based on activity), with a fallback to full scan if empty. This is a refinement; (1) is the more reliable fix.

### H2. Outbox has no cleanup — dispatched rows accumulate forever
**Files:** `Server/Account/src/db/OutboxRelay.hpp:74-118`, `Server/Account/schema.sql:336-344`
**Status:** NEW (v1/v2 missed)

`OutboxRelay::PumpOnce` `UPDATE`s `dispatched_at = now()` on success (line 113) but never `DELETE`s. The schema's `outbox_pending_idx` (line 344) is partial on `WHERE dispatched_at IS NULL` so the index stays small, but the table itself grows linearly with every wallet/pull/claim commit forever. Once `dispatched_at` is set the row is dead weight — there is no retry, no audit query, no consumer.

For a 1000-DAU steady state with 10 outbox-emitting commits per user per day, that's 10k rows/day → 3.65M/year → 18M after 5 years. Each row carries the JSONB payload (lz4-compressed but uncapped), so steady-state table size grows unbounded.

`docs/operations/backup-drill.md` mentions backups but no operational doc references outbox cleanup. The L-V2 set didn't flag it.

**Fix:** Either (a) add a `DELETE FROM outbox WHERE dispatched_at < now() - interval '24 hours'` sweep to `OutboxRelay::PumpOnce` (once per N iterations), or (b) document the operational requirement that an external cron job must do it, or (c) `DELETE` instead of `UPDATE` on successful dispatch (no audit benefit kept, but the path is correct-by-construction).

Recommend (c) — the relay is the only consumer; once dispatched there's nothing to retain. Audit history lives in `audit_log`, not outbox.

### H3. `idempotency_cache` and `events` partitions have no cleanup daemon
**Files:** `Server/Account/schema.sql:374-388, 309-316`
**Status:** NEW (audit text promises a sweep daemon)

The schema comment at `schema.sql:376-378` explicitly says: *"TTL: stored on every row via expires_at (default 1 hour from handler). Lookup queries filter on expires_at > now(); a cleanup daemon will sweep the table later — until launch the entries are small enough to ignore."* No such daemon exists. Application code never `DELETE`s from `idempotency_cache`. The `idempotency_cache_expires_idx` (line 387) is created but unused.

Same shape on `events`: pg_partman is configured with `p_premake := 3` (creates 3 future partitions) but the schema does not set `retention` or `retention_keep_table` on the parent. `partman.run_maintenance()` must be called periodically — there's no application code that does it, no cron in `docker-compose.yml`, no scheduled job in `scripts/`. After the 3 pre-made partitions are consumed (~3 months post-launch), inserts will start failing on missing partitions.

**Fix:**
1. Schedule `partman.run_maintenance_proc()` in Postgres via `pg_cron` (extension installed in the postgres image — verify) OR via an external cron hitting psql. Set `retention = '5 years'`, `retention_keep_table = false` on the `partman.part_config` row for `public.events`.
2. Add a background sweep to Account.exe (or a small standalone tool) that does `DELETE FROM idempotency_cache WHERE expires_at < now()` every N minutes.
3. Alternatively, document the operational requirement explicitly in `docs/operations/` and gate at startup (check that retention is configured, fail loud if not).

### H4. Wallet columns have no `CHECK (>= 0)` constraint
**Files:** `Server/Account/schema.sql:96-100`
**Status:** REPEAT (v2 followup-persistence.md M1 flagged this; not closed in the v2 sweep)

```sql
credits                 BIGINT NOT NULL DEFAULT 0,
universal_credits       BIGINT NOT NULL DEFAULT 0,
tickets                 BIGINT NOT NULL DEFAULT 0,
limited_tickets         BIGINT NOT NULL DEFAULT 0,
scrap                   BIGINT NOT NULL DEFAULT 0,
```

`material_inventory.quantity` carries `CHECK (quantity >= 0)` (line 206); the five wallet columns do not. WalletReducer asserts non-negativity in-process, but the DB cannot catch a divergence introduced by manual psql, a future direct UPDATE (e.g. an admin grant tool), or a buggy refactor that bypasses the reducer.

Promoted from Medium because the v2-followup explicitly recommended it ("Cheap belt-and-suspenders") and the recommendation was not actioned in the C/H/M sweep. Adding 5 CHECK constraints is a one-line schema change with zero behavior risk for compliant writers.

**Fix:** Append to the accounts table definition:
```sql
CHECK (credits >= 0),
CHECK (universal_credits >= 0),
CHECK (tickets >= 0),
CHECK (limited_tickets >= 0),
CHECK (scrap >= 0)
```

---

## Medium

### M1. `AccountRepository::Create` still doesn't populate all columns; new schema defaults can drift silently
**Files:** `Server/Account/src/AccountRepository.hpp:92-114`
**Status:** REPEAT (v2 followup-persistence.md M5 — not actioned)

Create's INSERT still names only `(username, password_hash, credits, tickets, limited_tickets, scrap)`. Every other column relies on the schema default. The H-V2-2 fix aligned `difficulty_tier` to 0 on both sides, but the architectural defect — Create() doesn't `RETURNING *`, and the returned AccountData carries C++ struct defaults — remains.

**Specific columns at risk if a future schema bump adds a non-zero default:**
- `login_streak` (DEFAULT 0; struct 0; agree)
- `last_streak_day` (DEFAULT NULL; struct `std::time_t 0`; agree because flush writes `CASE WHEN $3 = 0 THEN NULL`)
- `last_login_claim_at` (DEFAULT NULL; struct never populated — DB-default-only column with no C++ field)
- `last_login_claim_day_idx` (DEFAULT NULL; struct never populated — DB-default-only)
- `story_level` (DEFAULT 1; struct 1; agree)
- `story_xp` (DEFAULT 0; struct 0; agree)
- `difficulty_tier` (DEFAULT 0; struct 0; agree — but only after `18b6627`)
- `universal_credits` (DEFAULT 0; not in INSERT; relies on default — agree at 0)
- `total_*_pulls`, `*_star_*` stats (DEFAULT 0; struct 0; agree)
- `reducer_version` (DEFAULT 1; struct never populated — DB-default-only)
- `created_at` / `last_login` / `updated_at` (DEFAULT now(); Create sets `data.createdAt = time(nullptr)` at line 105-106 — Postgres' `now()` and C++'s `time(nullptr)` can differ by RTT, so the in-memory createdAt may be slightly earlier than the DB value)

`last_login_claim_at`, `last_login_claim_day_idx`, and `reducer_version` are **DB-only columns** with no C++ field. They're harmless as long as no read path consults them; verified — no `SELECT` references these three names anywhere in `Account/src/`. But they're invisible drift surface: a future "claim last login" feature that adds a C++ field will hit the same H-V2-2 trap.

**Fix:** Change `RETURNING account_id, public_uid, created_at` to `RETURNING *` and populate `AccountData` from the result row using the same column→field mapping as `LoadByAccountId`. Eliminates the bug class; cost is one extra column-deserialization block in Create.

### M2. `events.metadata` and `audit_log.metadata` rely on schema DEFAULT '{}'; no INSERT site supplies the column
**Files:** `Server/Account/src/AccountTransaction.hpp:185, 222-231`, `Server/Account/src/db/EventStore.hpp:63-68`, `Server/Account/schema.sql:286, 358`
**Status:** NEW

`AccountTransaction::Commit` builds the events INSERT (via `EventStore::AppendInTx`) with `(event_id, account_id, aggregate_kind, version, event_type, schema_version, data, metadata, idempotency_key)` — `metadata` IS populated from `ev.metadata.dump()`. Verified.

`audit_log` INSERT however names only `(account_id, actor, action, target, before, after)` — metadata is omitted and relies on the schema's `DEFAULT '{}'`. That's consistent but it means there's currently no way to attach metadata to an audit log entry without a SQL change. If a future caller wants to record actor IP, trace ID, etc., the column exists but no helper threads it through. Not a live bug; minor surface.

`outbox.payload` (NOT NULL, no default) is always explicitly populated — verified.

**Fix (optional):** Extend `AccountTransaction::AuditEntry` to carry optional metadata; pass through to the INSERT. Or document that `audit_log.metadata` is operator-only (set via direct UPDATE during forensics).

### M3. `last_login_claim_at` + `last_login_claim_day_idx` are DB-only columns with no C++ field
**Files:** `Server/Account/schema.sql:80-81`
**Status:** NEW

```sql
last_login_claim_at     TIMESTAMPTZ,
last_login_claim_day_idx SMALLINT,
```

These were added at some point for daily-login-claim bookkeeping. No code path reads or writes them (verified via grep of both column names — zero hits outside the schema). The `loginStreak`/`lastStreakDay` pair handles the actual claim logic.

Either dead surface (drop them) or planned future feature with a half-built schema. Compare with `loadouts.preset_id > 0` which has the same "planned future feature" comment at `schema.sql:184-188` documenting intent. These two columns have no such comment.

**Fix:** Either drop the two columns from schema.sql, or add a comment block documenting the planned daily-login-claim feature that will use them.

### M4. `events.idempotency_key` UNIQUE includes `created_at` (partition key) — same gotcha v1-C6 closed for the version constraint
**Files:** `Server/Account/schema.sql:293`
**Status:** REPEAT (v2 followup-persistence.md L4 — observation, now promoted because OutboxRelay's `AppendIdempotent` relies on a post-conflict SELECT that doesn't restrict by `created_at`)

```sql
UNIQUE (account_id, idempotency_key, created_at),
```

Across partitions this UNIQUE doesn't enforce dedup. `EventStore::AppendIdempotent` compensates at lines 110-115 with `SELECT 1 FROM events WHERE account_id = $1 AND aggregate_kind = $2 AND idempotency_key = $3 LIMIT 1` — that SELECT also doesn't restrict by `created_at` and therefore scans all partitions (same H1 issue).

The C6-class fix would be `pg_advisory_xact_lock` keyed on `(account_id, hash(idempotency_key))`. Today the per-player stripe lock makes the in-process race impossible, so this is latent.

**Fix:** Add a comment to the UNIQUE constraint documenting that cross-partition dedup is enforced by stripe lock + AppendIdempotent's post-conflict SELECT, not by this constraint. OR shrink the UNIQUE to `(account_id, idempotency_key)` — would require dropping the partition-key constraint, requires research on Postgres 16 partitioned-table UNIQUE semantics.

### M5. ConnectionPool capacity is hardcoded — no operational tuning surface
**Files:** `Server/Account/src/db/ConnectionPool.hpp:27-31`
**Status:** NEW

```cpp
explicit ConnectionPool(std::string conn_string,
                        std::size_t capacity = 16,
                        std::chrono::milliseconds acquire_timeout = std::chrono::seconds(5))
```

Two operational tunables (pool size, acquire timeout) with hardcoded defaults and no env-var or config override. Postgres' default max_connections is 100; with 3 Account instances at capacity=16 the pool consumes 48 (plus 5 for Auth and other services). Fine today; will trip operators who scale Account instances and don't realize each one holds 16 connections.

**Fix:** Read from `APHELYON_DB_POOL_CAPACITY` and `APHELYON_DB_ACQUIRE_TIMEOUT_MS` at construction (in the caller — `Account.hpp` or wherever the pool is constructed). Default values are reasonable; surface them as overridable.

### M6. AccountHydrator never copies `m_storyXp` overflow into `m_difficultyTier` consistency check
**Files:** `Server/Account/src/AccountHydrator.hpp:65-67`
**Status:** OBSERVATION (verified safe today, defensive)

`FromData` hydrates `storyLevel`, `storyXp`, `difficultyTier` independently from the AccountData row. If the row's denorm is corrupt (e.g. story_level=10 but difficulty_tier=99, beyond what the curve allows) there's no consistency check at load time — the bad state lives until the next `AddStoryXp` rolls it through `cfg.CapForTier`. Not a v3 regression; defensive observation for a future load-time invariant scanner.

### M7. `seed.sql` has zero fixtures — re-running db-reset gives an empty DB
**Files:** `Server/Account/seed.sql:1-43`
**Status:** OBSERVATION

The file is entirely comments + one commented-out `SELECT setval`. CLAUDE.md describes `seed.sql` as the dev-fixture file that makes `db-reset.bat` cheap, but in reality every reset produces an empty DB with no test account. Per the file's own comment block this is intentional (passwords can't be pre-hashed deterministically), but the result is friction not eliminated.

**Fix (optional):** Add a small dev-only script `scripts/dev-register.bat` that POSTs to Auth's Register RPC with a known username/password so the post-reset bootstrap is one command, not a manual client flow. Or use Account's internal RPC channel to inject a known PBKDF2 hash.

---

## Low / Observation

### L1. Hydrator's `ClearDirty` → cursor restore ordering is subtle but correct
**Files:** `Server/Account/src/AccountHydrator.hpp:69-83`
**Status:** OBSERVATION (verified safe)

After hydration the dirty bits are cleared, THEN the per-aggregate version cursors are written back. This works because `DirtyState::Clear()` (per v1 fix) explicitly preserves the four `cached_*_version` cursors — but the cursor write at lines 79-83 still happens AFTER Clear, so the write would land regardless. Defensive double-belt. No action needed; worth a one-line comment noting the redundancy.

### L2. `AccountHydrator::FromData` runs `TickQuests::Apply` inside the static factory
**Files:** `Server/Account/src/AccountHydrator.hpp:88`
**Status:** OBSERVATION

Hydration ends with `TickQuests::Apply(*account, questLoader, std::time(nullptr))` to fold quest expirations/unlocks. The factory is pure-static / no side effects on the loader, but `Apply` itself may flip dirty bits (reset_at transitions). A freshly-hydrated-but-immediately-dirty account is by design — it forces the next mutating handler to flush the auto-applied transitions. Not a bug; consider documenting that the hydrator's "clear dirty" + "tick quests" sequence intentionally leaves the account in a flushable state if quests transitioned.

### L3. `OwnedWeapon.acquired_at` is read by nothing on the C++ side
**Files:** `Server/Account/src/AccountRepository.hpp:435-437`, `Server/Account/src/db/RelationalFlush.hpp:172-173`
**Status:** OBSERVATION

`owned_weapons.acquired_at` is written on first INSERT and used by zero SELECT path. `OwnedGear.acquired_at` same. Dead-write surface that survives because Postgres-side ordering is occasionally useful for forensic queries. Keep — but note that a future "sort by acquisition time" UI feature would need to wire the column back into AccountData/CollectionState.

### L4. `gear_substats.slot_idx` CHECK is BETWEEN 0 AND 3 but the in-memory vector has no equivalent cap
**Files:** `Server/Account/schema.sql:171`, `Server/Common/src/CollectionState.hpp` (OwnedGear.subStats)
**Status:** REPEAT (v2-followup M3/M4 — not closed; latent)

A future code path that adds a 5th substat would fail at flush time with a CHECK violation. The std::vector has no compile-time cap. RelationalFlush's M15 sort caps the iteration at `std::min<std::size_t>(g.subStats.size(), 4)` (line 222) so the 5th element gets silently dropped before the INSERT — silent data loss rather than a CHECK fail. Worth either bumping the CHECK or asserting on the vector size.

### L5. `quest_states.metadata` CHECK is `jsonb_typeof = 'object'`; `events.metadata` and `audit_log.metadata` have no such CHECK
**Files:** `Server/Account/schema.sql:229-230, 286, 358`
**Status:** REPEAT (v2 L-V2-6)

Defensive consistency. `events.metadata`/`audit_log.metadata` could in principle hold a JSONB scalar or array; quest_states.metadata can only hold an object. Not breaking; would be nice to align.

### L6. AccountData `gearEquipment` slot key is `uint8_t` but `loadouts` table writes columns by slot semantics
**Files:** `Server/Common/src/AccountData.hpp:88-89`, `Server/Account/src/db/RelationalFlush.hpp:264-269`, `Server/Account/src/AccountRepository.hpp:520-524`
**Status:** OBSERVATION

The schema explicitly names `slot_helmet/slot_gauntlets/slot_chest/slot_boots` (4 columns). The C++ map is `slot_uint8 → uuid`. Adding a 5th slot (e.g. accessory) requires changes in BOTH the schema (new column) AND every flush/load site. Not a bug; just a coupling note for the rework.

### L7. `difficulty_tier` schema comment is verbose audit-trail but the canonical "starting value = 0" rationale is split across 3 files
**Files:** `Server/Account/schema.sql:86-92`, `Server/Account/src/Account.hpp:507-508`, `Server/Common/src/AccountData.hpp:53-56`
**Status:** OBSERVATION (acceptable)

H-V2-2 ships with a 7-line comment in schema.sql explaining the migration. Account.hpp has a 1-line comment. AccountData.hpp has a 3-line comment. The schema comment references "Account.hpp" by name — good. The other two don't reference the schema comment. If someone reads only AccountData.hpp they won't see the audit ID. Minor docs cohesion.

### L8. Idempotency cache `expires_at` index unused today but future-correct
**Files:** `Server/Account/schema.sql:387`
**Status:** OBSERVATION

Only useful when the H3 sweep daemon lands. Worth keeping; commented intent matches.

---

## Verified Closed (from v2)

1. **H-V2-2** — `difficulty_tier` schema/struct/Account class defaults now all 0. Existing rows with =1 are preserved by the migration (Postgres DEFAULT only affects new INSERTs). Code paths that BRANCH on the value: searched for `difficulty_tier > 0`, `difficultyTier > 0`, `GetDifficultyTier()` — only readers are `AccountHandlers.hpp:476` (response payload) and `QuestHandlers.hpp:459-469, 542, 573, 625` (claim path, breakthrough quest reward). The branch at `QuestHandlers.hpp:573` is `account.AdvanceDifficultyTier(def->breakthroughUnlocksTier, cfg)` which compares the new tier to `m_difficultyTier` and rejects if not greater — works correctly with `0` as the starting baseline. No code path treats tier=0 as "locked, unlock at tier=1". **But the reducer mismatch (C1) is still open.**
2. **AccountHydrator (M-V2-3)** — Verified `FromData` populates every field that `LoadByAccountId` selects:
   - SELECT scalars (lines 348-357): username/passwordHash → SetPasswordHash, createdAt → SetCreatedAt, lastLogin → (not hydrated to Account; lives on AccountData only — verified safe, only consumed by Auth's response payload), loginStreak → SetLoginStreak, lastStreakDay → SeedStreakDay, storyLevel → SetStoryLevel, storyXp → SetStoryXp, difficultyTier → SetDifficultyTier, wallet 5-tuple → SetState, stats 7-tuple → GetStats() = data.GetPlayerStats()
   - Joined child loads: characters → GetCollection().SetState, weapons → SetState, gear → SetState, party → SetParty, weaponEquipment → SetWeaponEquipment, gearEquipment → SetGearEquipment, pity → SetRawPity, worldFlags → assignment, quests → assignment, materials → SetMaterials
   - LoadEventVersions cursors → MutableDirty().cached_*_version
   - **Fields selected but never assigned to Account:** `lastLogin` (intentional — used only for the Auth response, never mutated by Account)
   - **Fields hydrated but never selected:** none
   - **Account fields with no AccountData source:** `m_id`, `m_accountId`, `m_publicUid` (assigned directly from data.id/accountId/publicUid before the bulk hydrate)
3. **NOT NULL coverage** — Verified every NOT NULL column on `accounts`, `owned_characters`, `owned_weapons`, `owned_gear`, `gear_substats`, `loadouts`, `material_inventory`, `party_slots`, `quest_states`, `quest_objectives`, `world_flags`, `pity_state`, `events`, `snapshots`, `outbox`, `audit_log`, `idempotency_cache` is either populated by every INSERT site OR has a schema DEFAULT that flushes/INSERTs rely on. M1 above flags Create()'s reliance on defaults as a defect class even though it's currently correct.
4. **FK coverage holds** — Every account-scoped table has `ON DELETE CASCADE` on `account_id`. `gear_substats` references `owned_gear` (composite FK); `quest_objectives` references `quest_states`; both CASCADE. No path can INSERT a child before parent — every child INSERT happens in the same transaction as a parent INSERT (Save/Begin pattern), so the FK is satisfied at COMMIT.
5. **CHECK constraints** — `material_inventory.quantity >= 0`, `quest_states.metadata` jsonb_typeof = 'object', `party_slots.slot_idx BETWEEN 0 AND 3`, `gear_substats.slot_idx BETWEEN 0 AND 3` — all are handler-validated separately; the CHECK is a backstop. Wallet columns lack CHECK (H4).
6. **Partitioning** — `events` partitioned by `created_at` (monthly). pg_partman bootstrap at lines 309-316. **No queries use `created_at` predicates → no pruning** (H1). **No retention configured → unbounded partition count post-launch** (H3 part 2).
7. **L2 / M14 / M13 / M15 / M17 / M18 / H4 / H11 from v2-followup-persistence.md** — All re-verified closed in the current code per their previous-audit citations.

---

## Triage

**Today / immediate:**
1. **C1** — fix `ProgressionReducer.hpp:10` to `difficulty_tier = 0` + update `ProgressionReducerTest.cpp:25` to match. One-line schema fix, one-line test fix.

**This week:**
2. **H1** — trust `cached_*_version` cursors in `EventStore::AppendInTx` pre-check (eliminates partition fan-out for the hot-path commit).
3. **H2** — change OutboxRelay `UPDATE dispatched_at = now()` to `DELETE FROM outbox WHERE outbox_id = $1`. Or document the operational retention requirement.
4. **H3** — schedule `partman.run_maintenance_proc()` via `pg_cron` (verify extension availability in `aphelyon/postgres:16`) + `DELETE FROM idempotency_cache WHERE expires_at < now()` sweep.
5. **H4** — add 5 `CHECK (... >= 0)` constraints to the wallet columns.

**Before launch:**
6. **M1** — `Create()` → `RETURNING *`.
7. **M3** — decide on `last_login_claim_at`/`last_login_claim_day_idx`: implement or drop.
8. **M5** — pool capacity / acquire timeout via env vars.
9. **M2 / M4 / M6** — minor.

**Eventually:**
- L items + M7 dev-fixtures.
