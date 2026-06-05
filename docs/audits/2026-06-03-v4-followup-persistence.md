# v4 Follow-up Audit: Persistence + Schema

**Date:** 2026-06-03
**Scope:** `Server/Account/schema.sql`, `Server/Account/seed.sql`, `Server/Account/src/Cache/AccountRepository.hpp`, `Server/Account/src/Db/RelationalFlush.hpp`, `Server/Account/src/Db/EventStore.hpp`, `Server/Account/src/Db/OutboxRelay.hpp`, `Server/Account/src/Db/ConnectionPool.hpp`, `Server/Account/src/Db/SnapshotWriter.hpp`, `Server/Account/src/Cache/AccountHydrator.hpp`, `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/Reducers/*`, `Server/Common/src/Types/AccountData.hpp`
**Lens:** verify the v3 remediation arc landed cleanly and surface residual + new persistence-layer drift.

---

## Verdict

The v3 remediation arc landed cleanly across the persistence dimension. C-V3-1 (`ProgressionReducer::difficulty_tier`), H-V3-4 (outbox DELETE-on-dispatched daemon), H-V3-5 (idempotency sweeper + partman maintenance + 24-month retention with `p_premake` bumped 3 → 12), H-V3-6 (wallet `CHECK (>= 0)` × 5), M-V3-3 (dropped `last_login_claim_at` + `last_login_claim_day_idx`), and L5 (`jsonb_typeof='object'` CHECK on `events.metadata` + `audit_log.metadata`) all verified closed in code with consistent audit-trail comments. The OutboxRelay is now a triple-duty daemon (outbox prune / idempotency sweep / partman maintenance) keyed off independent pump-count counters to avoid stampede; this is a clean piece of work. The remaining persistence debt is a small set of dead-or-undertested surfaces and one schema-vs-code asymmetry around `Create()` that has now persisted across three audit rounds — not blocking, but should be cleaned up before launch. No CRITICAL findings.

---

## CRITICAL

None.

---

## HIGH

### H-V4-1. `AccountRepository::Create` still doesn't `RETURNING *` — `data.createdAt` drifts from `accounts.created_at`
**File:** `Server/Account/src/Cache/AccountRepository.hpp:88-113`
**Status:** REPEAT (v2 M5, v3 M-V3-1 / M1) — carried forward unactioned for the third audit round.

`INSERT INTO accounts (username, password_hash, credits, tickets, limited_tickets, scrap) ... RETURNING account_id, public_uid, created_at` returns three columns, but the C++ side then **overwrites `data.createdAt` with `std::time(nullptr)`** (line 105) instead of reading column index 2 of the RETURNING row. The DB's `now()` at INSERT time and the wall-clock read at line 105 can differ by tens of ms (or more — `time(nullptr)` has second granularity; the DB has microsecond). The mismatch is silent: the in-memory `AccountData.createdAt` lies about the row's true `created_at`, and the very first flush via `FlushAccountsRow` doesn't touch the column (good — it's INSERT-only), so the drift sits forever.

Every other column with a schema default that Create() doesn't INSERT also gets struct defaults (`loginStreak=0`, `storyLevel=1`, `difficultyTier=0`, `universal_credits=0`, `total_*_pulls=0`, …) — those agree by coincidence today, but the architectural defect remains: a future schema bump (e.g. starting `story_level` at 5 for a new-player buff event) would silently desync.

**Fix:** read the RETURNING row's `created_at` column:
```cpp
data.createdAt = r[0][2].as<std::time_t>();   // not std::time(nullptr)
data.lastLogin = data.createdAt;
```
Or — more robust — change to `RETURNING *` and populate every field from the row using the same column-mapping pattern as `LoadByAccountId`.

### H-V4-2. `events`/`audit_log`/`outbox` `account_id` FK is not indexed for the CASCADE delete path
**File:** `Server/Account/schema.sql:284-344`, `377-393`, `364-372`
**Status:** NEW

`accounts.account_id` has `ON DELETE CASCADE` references in `events`, `audit_log`, and `outbox` (via the implicit account scoping). `events_account_aggregate_version_idx` on `(account_id, aggregate_kind, version)` covers events; `audit_log_account_time_idx` covers audit_log. But **`outbox` has zero index on `account_id`** — the table doesn't even reference accounts via FK (the column doesn't exist on outbox; it's only in the JSONB payload). That's intentional for the relay's `FOR UPDATE SKIP LOCKED` scan pattern, but it means **soft-delete cleanup or GDPR hard-delete of an account leaves orphaned outbox rows that reference the account in their payload**. The relay can still dispatch them (handlers look up by payload contents); if the destination service has its own account-existence check, the dispatched event silently no-ops. Not a live bug because we don't hard-delete accounts today (only `deleted_at` soft-delete), but it's a GDPR-compliance landmine.

**Fix:** either (a) document explicitly that GDPR delete is a multi-step process that includes `DELETE FROM outbox WHERE payload->>'account_id' = $1`, or (b) add a real `account_id BIGINT NOT NULL` column on outbox with FK CASCADE so the cleanup is automatic.

---

## MEDIUM

### M-V4-1. `accounts.reducer_version` is a dead column
**File:** `Server/Account/schema.sql:119`
**Status:** NEW

```sql
reducer_version         INT NOT NULL DEFAULT 1,
```
Grep across the codebase: the only consumers of `reducer_version` are the `snapshots` table (where it's load-bearing per `SnapshotWriter.hpp:23-97`) and this `accounts.reducer_version` column — but **nothing reads, writes, or branches on the column on the accounts row**. The reducer-version invalidation logic lives entirely on the snapshots row. Same defect class as the `last_login_claim_at` / `last_login_claim_day_idx` columns dropped in M-V3-3 — vestigial schema surface that a future "reducer version migration" feature might want to use but isn't wired today.

**Fix:** drop the column (same M-V3-3-style sweep), OR add a comment block documenting the planned future use and reference SnapshotWriter as the live-today consumer.

### M-V4-2. `outbox` has no `created_at` index for the dispatched-row prune
**File:** `Server/Account/schema.sql:364-372` + `Server/Account/src/Db/OutboxRelay.hpp:112-123` (PruneDispatchedOutbox)
**Status:** NEW

`PruneDispatchedOutbox` runs `DELETE FROM outbox WHERE dispatched_at IS NOT NULL AND dispatched_at < now() - interval`. The only index on outbox is `outbox_pending_idx` which is partial `WHERE dispatched_at IS NULL` — exactly the inverse of what the prune needs. The prune runs a sequential scan of all dispatched rows on every prune tick (every ~60s). Today the table is small; once the relay catches up, the dispatched set is the majority. A non-partial index on `(dispatched_at)` would let the prune go logarithmic.

**Fix:** add `CREATE INDEX outbox_dispatched_idx ON outbox (dispatched_at) WHERE dispatched_at IS NOT NULL;`

### M-V4-3. `accounts.last_streak_day` lacks `CHECK (last_streak_day >= DATE '2020-01-01' OR last_streak_day IS NULL)`
**File:** `Server/Account/schema.sql:83`
**Status:** OBSERVATION

`FlushAccountsRow` writes `DATE '1970-01-01' + $3::int` when `$3 != 0`. If `Account::GetLastStreakDay()` ever returns a tiny but nonzero unix-day-count (e.g. corrupted to 7 for "epoch + 7 days"), the column stores `1970-01-08`, which is technically valid but obviously nonsense. A bounded CHECK would surface the corruption.

**Fix:** optional — `CHECK (last_streak_day IS NULL OR last_streak_day >= DATE '2020-01-01')`.

### M-V4-4. `events.idempotency_key UNIQUE` still partition-scoped — H-V3-12 traceability landed but the underlying gap remains
**File:** `Server/Account/schema.sql:303`
**Status:** REPEAT (v3 H-V3-12 / M-V3-4 — explicitly deferred with documented rationale)

v3 closed this as "documented" via the schema comment at L296 noting the constraint shape. The actual cross-partition dedup still relies on `idempotency_cache.PRIMARY KEY (account_id, scoped_key)` (un-partitioned), which is the correct backstop. Carried forward as a tracker — when a non-stripe-lock failure mode lands (horizontal scale), this gap reopens.

### M-V4-5. `AccountRepository::Save` brute-force-marks every collection dirty without skipping unchanged rows
**File:** `Server/Account/src/Cache/AccountRepository.hpp:146-159`
**Status:** NEW (minor)

The idle-evict path's `Save()` writes every owned_character, weapon, gear, loadout, party slot, pity slot, quest, and material — even for accounts that have made zero mutations since hydration. For an account with 50 characters, 200 weapons, 80 gear, that's ~350 INSERTs on idle-evict regardless of whether anything actually changed. Today the load is small; with 1k DAU and the default 30min idle window, you're looking at ~50 inserts/sec sustained from idle-evict alone.

**Fix:** the dirty-bit machinery is already in place — Save should only flush what's actually marked dirty (i.e. call `RelationalFlush::Flush` directly without the bulk re-mark step). The bulk re-mark only existed because the pre-Postgres JSON repo did blob-rewrites; it's vestigial in the relational world.

---

## LOW / OBSERVATION

- **L-V4-1.** `accounts_deleted_idx` is partial `WHERE deleted_at IS NOT NULL` (line 125) — fine for finding soft-deleted accounts in a GDPR audit; would not help a hot-path "show me all live accounts" query. Today the only `WHERE deleted_at IS NULL` query is single-row by `account_id` or `username` (PK / UNIQUE index handles it).
- **L-V4-2.** `events.xid` is `XID8 NOT NULL DEFAULT pg_current_xact_id()` and the `events_xid_seq_idx` exists (line 312) but no application code consults it. Same shape as the `acquired_at` columns L-V3-3 flagged — reserved for future point-in-time-recovery / change-data-capture work.
- **L-V4-3.** `events_pulls_banner_idx` uses an expression `(data->>'banner_id')` (line 317) — fine, but no current query path uses this index. Verified via grep on `banner_id` in SELECT contexts; the index pays its INSERT cost with no reader. Verify the H-V3-3 deferred created_at predicate work doesn't accidentally rely on this index.
- **L-V4-4.** `gear_substats.slot_idx CHECK BETWEEN 0 AND 3` (L4 from v3 — WARN landed in `FlushOwnedGear`, line 234-237). Verified. The schema CHECK itself remains as backstop; the application-layer WARN is the proximate fix.
- **L-V4-5.** `OutboxRelay::PumpOnce` (line 154-161) uses `LIMIT 64 FOR UPDATE SKIP LOCKED` — with the new prune daemon, the table now stays bounded so 64-row batches drain promptly. Pre-prune this needed `db-reset` mid-session per the H-V3-4 finding; verified closed.
- **L-V4-6.** `seed.sql` is still fixture-empty (45 lines, all comments — see line 22 referencing `scripts/db-seed-accounts.bat` for the dev-bootstrap path). This is the intentional outcome of v3's "passwords can't be deterministically pre-hashed" tradeoff. Document mentions the new helper script — verify it exists and works post-`db-reset`.
- **L-V4-7.** `accounts.universal_credits` schema default is 0, `AccountData.universalCredits` C++ default is 0, but `Create()` doesn't include the column in the INSERT (relies on default). If `GachaConfig::GetStartingUniversalCredits()` ever returns nonzero, the new-account starting wallet would silently disagree with config. Same defect class as H-V4-1; same fix.
- **L-V4-8.** `SnapshotWriter` writes the snapshot row with `INSERT ... ON CONFLICT (account_id, aggregate_kind) DO UPDATE`. The PK is correct and the writer is single-writer per account via stripe lock, so no race. Verified.
- **L-V4-9.** `events.schema_version` has DEFAULT 1 and is included in every INSERT site. If a reducer migration bumps the in-memory schema_version but a stale handler holds the old constant, the INSERT writes the old value with no enforcement. Defense-in-depth: a CHECK `(schema_version > 0)` is trivial and would catch a `0` or negative explicit write.
- **L-V4-10.** `pity_state.guarantee_5` column name is specific to the 5-star guarantee; if a future banner introduces a 4-star pity guarantee (some HSR banners do this for off-banner 4* characters), the column rename would touch every flush/load site. Not a current bug, just a coupling note for the gacha rework that the next major banner system change might cause.

---

## Verified Closed from v3

| Item | Verification |
|---|---|
| C-V3-1 (`ProgressionReducer::difficulty_tier = 1` → 0) | `Reducers/ProgressionReducer.hpp:10` reads `int difficulty_tier = 0;` |
| H-V3-3 (created_at predicate) | Deferred with documented rationale in `EventStore.hpp:62-69`; dependency on H-V3-5 retention noted in comments |
| H-V3-4 (outbox DELETE-on-dispatched daemon) | `OutboxRelay.hpp:112-123 PruneDispatchedOutbox`, scheduled every 120 pumps (~60s), 24h retention |
| H-V3-5 (idempotency sweeper + partman + 24-month retention) | `OutboxRelay.hpp:129-149 SweepExpiredIdempotency / RunPartmanMaintenance`; schema `p_premake := 12` (L333); `retention = '24 months'` UPDATE (L341-344) |
| H-V3-6 (wallet CHECK constraints) | `schema.sql:103-107` all five wallet columns have `CHECK (... >= 0)` |
| H-V3-12 (partition-local dedup documented) | `schema.sql:303` UNIQUE constraint preserved; cross-partition path via `idempotency_cache` |
| M-V3-3 (dropped `last_login_claim_at` + `last_login_claim_day_idx`) | Columns absent from `schema.sql:69-124`; audit comment block at L76-79 documents the removal |
| L4 (gear substat overflow WARN) | `RelationalFlush.hpp:234-237` `LOG_DATA_WARN` if `g.subStats.size() > 4` |
| L5 (`jsonb_typeof='object'` CHECK on events/audit_log metadata) | `schema.sql:296` events; L389 audit_log; both `CHECK (jsonb_typeof(metadata) = 'object')` |
| L7 (difficulty_tier=0 cross-file traceability) | `schema.sql:88-94` 7-line comment block referencing Account.hpp + audit ID H-V2-2 |
| H-V3-9 (`InsertIfAbsent` uses `try_emplace`) | `AccountCache.hpp:255-273` uses `try_emplace` with audit comment |
| M-V3-2 (`events.metadata` / `audit_log.metadata` NOT NULL) | Both columns have `NOT NULL DEFAULT '{}'` + the jsonb_typeof CHECK |
| H-V3-2 (advisory lock at EnsureOpen) | `EventStore.hpp:47-49 AcquireAdvisoryLockInTx` extracted; `AccountTransaction::EnsureOpen` calls it once per commit |
| Centralized DB config (M-V3-2 security) | `Db/DbConfig.hpp` reads `APHELYON_DB_CONNECTION` env var with Debug/Release fallback semantics |

---

## Triage

**This week:**
1. **H-V4-1** — fix `Create()` to read `created_at` from `RETURNING` instead of overwriting with `std::time(nullptr)`. 2-line change; eliminates a class of silent drift.
2. **H-V4-2** — decide policy on outbox-vs-GDPR-delete: either document the multi-step delete OR add an `account_id` FK column.

**Before launch:**
3. **M-V4-1** — drop `accounts.reducer_version` OR document its planned use.
4. **M-V4-2** — add `outbox_dispatched_idx` non-partial index for the prune path.
5. **M-V4-5** — replace `Save()`'s bulk re-mark with direct `RelationalFlush::Flush` on the existing dirty bits.

**Eventually:**
6. L items + M-V4-3 / M-V4-4 trackers.
