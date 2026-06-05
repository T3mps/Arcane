# v5 Follow-up Audit: Persistence + Schema

**Date:** 2026-06-03 (same-day successor to v4, after a 20-commit v4 remediation arc)
**Scope:** `Server/Account/schema.sql`, `Server/Account/seed.sql`, `Server/Dockerfile.postgres`, `Server/docker-compose.yml`, `Server/Account/src/Cache/AccountRepository.hpp`, `Server/Account/src/Cache/AccountTransaction.hpp`, `Server/Account/src/Db/RelationalFlush.hpp`, `Server/Account/src/Db/ConnectionPool.hpp`, `Server/Account/src/Db/OutboxRelay.hpp`, `Server/Account/src/Db/SnapshotWriter.hpp`, `Server/Account/src/Db/EventStore.hpp`, `Server/Account/src/Db/DbConfig.hpp`, plus live-DB introspection of `aphelyon_postgres`.
**Lens:** verify v4 persistence remediation against current code AND live DB, scan for regressions introduced by the v4 fixes, surface NEW drift.

---

## Verdict

The v4 persistence triage landed cleanly *in code*: H-V4-1 (RETURNING `created_at`), H-V4-2 (outbox `account_id` FK + index), H-V4-3 carry-forward — all match the schema.sql shape and are wired into the writer side. The schema cleanup cluster (M-V4-1 drop reducer_version, M-V4-2 `outbox_dispatched_idx`, M-V4-3 last_streak_day CHECK) also lands in schema.sql. L-V4-7's universal_credits explicit-INSERT clause is present.

**But the audit surfaces one new Critical of exactly the same wiring-vs-implementation shape as v4's C-V4-1**: `SnapshotWriter` is a fully implemented background worker that is **never instantiated** by `AccountServer`. Zero rows in the live `snapshots` table after ~1100 outbox writes; zero production callers of `Enqueue`; zero production callers of `EventStore::LoadStream` / `LoadCurrentVersion`. The entire snapshot-rebuild path is dead code. Today the system survives because every event-sourced aggregate's "current state" is read from the denormalized accounts row (wallet, pulls, story), never re-folded from the event log. The moment any future feature (audit replay, fork-point restore, point-in-time recovery, reducer-version migration) needs to fold a stream from genesis, it pays the unbounded scan cost the snapshot machinery was designed to cap.

**Two schema-vs-live drift findings:**
1. **H-V5-2:** `schema.sql:342` says `p_premake := 12` but live `partman.part_config.premake = 3`. The `partman.create_parent(...)` call only runs at first `db-setup`; live DB was initialized with the older `p_premake := 3` value and the v3 schema-side bump never reached it. With 8 partitions and OutboxRelay's hourly `run_maintenance_proc()` now actually running (post-v4-C-V4-1), the maintenance call keeps the 3-slot window rolling forward, so practical risk is lower than the v3 audit feared — but `schema.sql` is no longer the source of truth for premake.
2. **M-V5-2:** live DB has 60 outbox rows all with `account_id = NULL` (pre-v4-H-V4-3 test debris). The v4 H-V4-3 finding text explicitly noted the FK was applied via `ALTER TABLE` without `db-reset`, and the AccountTransaction writer correctly populates account_id going forward — but the existing NULL rows mean a GDPR delete on any pre-v4 account would still leave dispatched-but-archived rows orphaned.

**Two repeat-from-v4 findings still open:**
- **H-V4-2 (H-V5-1)** RETURNING `created_at` shipped, but the new code uses `EXTRACT(EPOCH FROM created_at)::bigint`, which TRUNCATES microseconds (Postgres' `now()` has microsecond precision; `bigint` epoch seconds doesn't). Load and Create now agree on the truncated value, so there's no in-process drift — but the AccountData carries a value that disagrees with the row's actual TIMESTAMPTZ value by up to 999ms. The audit text dismissed this as "second granularity" but the live schema column DOES have higher precision. Minor; demote to Low if you prefer.
- **M-V4-4** carry-forward: `events.idempotency_key UNIQUE` still partition-scoped. Documented in v4-followup; no new action.

**Tally:** 1 Critical, 2 High, 6 Medium, 9 Low.

---

## CRITICAL

### C-V5-1. `SnapshotWriter` is fully implemented but never instantiated — same wiring gap as v4-C-V4-1
**Files:**
- `Server/Account/src/Db/SnapshotWriter.hpp:32` — class definition (worker thread, queue, UPSERT in Run())
- `Server/Account/src/AccountServer.hpp:55-365` — no `m_snapshotWriter` member, no construction site
- `Server/Account/schema.sql:359-368` — `snapshots` table preserved
**Status:** NEW (mirror of v4-C-V4-1; same defect class went uncaught)

`grep -rn SnapshotWriter Server/Account/src/` returns only the class file itself. No instantiation in `AccountServer`, no instantiation in `main.cpp`. The `snapshots` table has zero rows in the live DB (`SELECT count(*) FROM snapshots` → 0), after ~1100 outbox writes (`outbox_pkey idx_scan = 1107`). Production code does not call `EventStore::LoadStream` or `EventStore::LoadCurrentVersion` (grep confirms only the EventStore class file references them).

Live verification:
```
$ docker exec aphelyon_postgres psql -U aphelyon -d aphelyon -c \
    "SELECT count(*) FROM snapshots;"
 count
-------
     0
(1 row)
```

Operational consequence:
- The snapshot optimization that caps replay length is **non-functional**: any future code path that folds a stream from genesis pays the full event-history cost. Today this is latent because the read path goes through the denormalized accounts row + the v2 LoadEventVersions `MAX(version)` cursor — no fold-from-events ever runs in a live RPC.
- The `snapshots.reducer_version` column that M-V4-1 explicitly kept ("invalidation lives entirely on snapshots.reducer_version") has zero rows, so the invalidation logic has nothing to read.
- A future reducer-version migration tool (the documented justification for keeping the column per the M-V4-1 commit) **cannot work** until SnapshotWriter is wired and a backfill job populates the table.

**Fix:** Same shape as C-V4-1's resolution. Instantiate as an `AccountServer` member after `m_pool`:
```cpp
db::SnapshotWriter m_snapshotWriter{m_pool};
```
Then wire `AccountTransaction::Commit` (after the existing `tx_->commit()` at AccountTransaction.hpp:312) to enqueue snapshot jobs for the aggregates whose version was just bumped. Cadence policy is a design call (per-commit vs every-N-events vs threshold-based); pick one and document.

Add an integration test that asserts a snapshot row exists after N events on a single aggregate.

**Why Critical and not High:** the v4 audit downgraded C-V4-1 to "non-functional code" rather than "data corruption" — same here. But every audit round that doesn't catch this leaves the snapshot story half-built. The earlier we wire it the cheaper the cadence-design conversation gets; deferring to "post-launch" means the production migration that motivates wiring it will be expensive.

---

## HIGH

### H-V5-1. `partman.part_config.premake` drift: schema.sql says 12, live DB has 3
**Files:**
- `Server/Account/schema.sql:342` — `p_premake := 12`
- Live DB `partman.part_config` — `premake = 3`
**Status:** NEW (schema-vs-code drift)

```
$ docker exec aphelyon_postgres psql -U aphelyon -d aphelyon -c \
    "SELECT parent_table, premake, retention, retention_keep_table
       FROM partman.part_config WHERE parent_table = 'public.events';"
 parent_table  | premake | retention | retention_keep_table
---------------+---------+-----------+----------------------
 public.events |       3 | 24 months | f
(1 row)
```

The retention update (`UPDATE partman.part_config SET retention = '24 months' ... WHERE parent_table = 'public.events'`) at schema.sql:350-353 DID land — retention is `24 months` in the live config. But `partman.create_parent(...)` only inserts a new row in `part_config` the first time it runs; the v3 H-V3-5 bump from `p_premake := 3` to `p_premake := 12` only affects newly-created parents. The live DB was bootstrapped with the older 3 value and the schema's value never reached it.

Currently 8 partitions exist (March 2026 through Sept 2026 plus default). With `premake=3` and `run_maintenance_proc()` firing hourly (now real, post-C-V4-1), partman maintains 3-month look-ahead — which is fine in steady state, but if OutboxRelay's `RunPartmanMaintenance` ever fails for a sustained period (DB hiccup, container restart loop), the 3-slot buffer is the entire margin. The schema's "12-month bootstrap window" docstring is misleading.

**Fix (non-destructive — DO NOT db-reset):**
```sql
UPDATE partman.part_config
   SET premake = 12
 WHERE parent_table = 'public.events';
SELECT partman.run_maintenance_proc();   -- materialize the new look-ahead
```

After this, partman will materialize partitions through May 2027, and the schema.sql `p_premake := 12` matches live. The same `ALTER`-don't-reset pattern that v4 used for the `outbox.account_id` FK applies.

### H-V5-2. `accounts.public_uid` UNIQUE constraint is dead surface — no SELECT path uses it
**Files:**
- `Server/Account/schema.sql:71` — `public_uid BIGINT NOT NULL DEFAULT nextval('public_uid_seq') UNIQUE`
- Live DB: `accounts_public_uid_key` idx_scan = 0
**Status:** OBSERVATION → promoted because `accounts_public_uid_key` is a btree on a BIGINT column with 0 lookups across 2495 `accounts_pkey` scans

```
$ docker exec aphelyon_postgres psql -U aphelyon -d aphelyon -c \
    "SELECT indexrelname, idx_scan FROM pg_stat_user_indexes
       WHERE indexrelname IN ('accounts_pkey', 'accounts_public_uid_key');"
       indexrelname        | idx_scan
---------------------------+----------
 accounts_public_uid_key   |        0
 accounts_pkey             |     2495
```

`public_uid` is the player-facing 9-digit display ID with the regional taxonomy documented at `schema.sql:37-58`. Today the only callers are `LoadByAccountId` (reads but doesn't filter) and `Create` (reads back the sequence value via RETURNING). No "look up by display UID" handler exists.

The defect: **the unique constraint enforces a contract no reader currently relies on.** If a future client-facing "/profile/:uid" endpoint lands, the index becomes valuable instantly. But the constraint is paid on every INSERT for a benefit no SELECT consumes. Less severe than dead columns (`reducer_version` defect class) because the docstring documents the design rationale — but worth noting that the v2/v3/v4 "indexes the code actually uses" claim still doesn't literally hold.

**Fix:** Either (a) add the planned "look up by public_uid" RPC and let the index pay for itself, or (b) add a comment block to `public_uid_seq` documenting that the UNIQUE is a forward-compatibility belt-and-suspenders for the planned profile-lookup feature.

Promoted to High because this is the third audit round noting an unused index/constraint pattern (v2 M2 enumerated 9 of them, v3 didn't sweep, v4 didn't sweep, the pattern persists).

---

## MEDIUM

### M-V5-1. `outbox.account_id NULL`able shape exists primarily to support test debris — production should not need it
**Files:**
- `Server/Account/schema.sql:381-388` — column is `BIGINT REFERENCES accounts(account_id) ON DELETE CASCADE` (no NOT NULL)
- `Server/Account/src/Cache/AccountTransaction.hpp:228-232` — writer always supplies `account_.GetAccountId()` (nonzero by AccountServer construction invariant)
- `Server/Account/tests/Integration/OutboxRelayTest.cpp` — tests insert raw rows without account_id
- Live DB: 60 outbox rows, all `account_id = NULL`
**Status:** NEW (introduced by v4 H-V4-3)

The H-V4-3 fix shipped account_id as nullable specifically because OutboxRelayTest fixtures insert raw rows without an account context. From the commit message:

> account_id stays nullable because the OutboxRelay infrastructure tests legitimately insert non-account-scoped rows (relay mechanics, not account-linked dispatches); production writers in AccountTransaction always populate it.

Two problems:
1. **Test-shape drives production schema.** Production never wants NULL. The "right" shape is `account_id BIGINT NOT NULL`, with the OutboxRelay tests using fixture accounts (TC-V3-1's IntegrationDbFixture already provides this) so the test rows ARE account-scoped.
2. **The live DB has 60 NULL rows** — pre-v4 test debris that exists because `db-reset` is policy-forbidden. A future GDPR-delete on any pre-v4 account leaves these orphans (they don't reference the deleted account, so CASCADE doesn't fire; but they ALSO don't have payload data identifying any account, so they're un-deletable by a payload-key sweep too).

**Fix:**
1. Tighten `OutboxRelayTest.cpp` to insert under TC-V3-1's seeded account (the fixture already exposes `repo.Create()`); supply the seeded account's id to the test rows.
2. Once tests pass: `ALTER TABLE outbox ALTER COLUMN account_id SET NOT NULL` after a one-time `DELETE FROM outbox WHERE account_id IS NULL` (acceptable: dispatched rows already past their semantic value; pending rows with no account_id are test-only artifacts).
3. Update schema.sql to NOT NULL.

### M-V5-2. v4-H-V4-2 RETURNING `created_at` truncates microseconds to whole seconds
**File:** `Server/Account/src/Cache/AccountRepository.hpp:112-115, 124-125`
**Status:** PARTIAL (v4 closed in code; new audit catches the precision loss)

```cpp
RETURNING account_id, public_uid,
          EXTRACT(EPOCH FROM created_at)::bigint,
          EXTRACT(EPOCH FROM last_login)::bigint
...
data.createdAt = static_cast<std::time_t>(r[0][2].as<std::int64_t>());
```

Postgres' `TIMESTAMPTZ` has microsecond precision; `EXTRACT(EPOCH FROM ...)::bigint` truncates to whole seconds. The H-V4-2 audit text dismissed this as "second granularity" but the live DB column does carry higher precision. After the fix:

- Drift between `data.createdAt` and `accounts.created_at`: up to 999ms (the truncated fractional part).
- Load path (`LoadByAccountId`) also truncates via `EXTRACT(EPOCH FROM created_at)::bigint`, so Load and Create AGREE on the truncated value — no IN-PROCESS drift.
- But the C++ value is provably not equal to the DB row's actual stored value.

This is what the H-V4-2 audit was trying to prevent ("eliminating the silent drift surface" per the commit message), but it's still present at sub-second granularity. The fix is logically half-done.

**Fix:** if microsecond precision matters, store both in `time_t` (whole seconds) plus a separate microseconds field, OR use `to_char(created_at, 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"')` and parse client-side. Realistically: document the truncation in a comment block at the RETURNING site so the next auditor doesn't re-flag this as a regression.

### M-V5-3. `idempotency_cache.created_at` is written by `ON CONFLICT DO UPDATE SET ... created_at = now()` — overwrites the original creation timestamp
**File:** `Server/Account/src/Cache/AccountTransaction.hpp:296-301`
**Status:** NEW

```cpp
INSERT INTO idempotency_cache
  (account_id, scoped_key, response_payload, expires_at)
VALUES ($1, $2, $3, now() + ($4 || ' seconds')::interval)
ON CONFLICT (account_id, scoped_key) DO UPDATE
  SET response_payload = EXCLUDED.response_payload,
      expires_at       = EXCLUDED.expires_at,
      created_at       = now()
```

The H2 refresh-on-conflict pattern (audit M-V4 idempotency context) deliberately bumps `expires_at` and `response_payload` so a refresh extends the TTL. But it ALSO rewrites `created_at` to `now()` — making `created_at` semantically equal to `last_updated_at`. The column name says "when was this row first inserted"; the value says "when was it last touched."

Operational consequence:
- Forensic queries like "how long ago did this idempotency key first appear?" cannot be answered from this table.
- The `idempotency_cache_expires_idx` (indexed on `expires_at`) is fine; no current query references `created_at`. But the name lies.

**Fix:** drop the `created_at = now()` line. Insert-time value is what the column name claims. If you genuinely need a "last touched" timestamp, add a separate `updated_at` column.

### M-V5-4. `events.idempotency_key UNIQUE (account_id, idempotency_key, created_at)` is still partition-scoped (carry-forward from v4-M-V4-4)
**File:** `Server/Account/schema.sql:312`
**Status:** CARRY-FORWARD (v4-M-V4-4 explicit defer)

Still documented at the constraint, still backed by `idempotency_cache.PRIMARY KEY (account_id, scoped_key)` for cross-partition dedup. No change recommended; tracking only.

### M-V5-5. `accounts.public_uid_seq` overflow @ 100M accounts per region — no monitoring
**File:** `Server/Account/schema.sql:52-58`
**Status:** OBSERVATION (pre-launch concern)

```sql
CREATE SEQUENCE public_uid_seq
    ...
    MAXVALUE 199999999      -- 100M accounts per region prefix
    NO CYCLE;
```

100M is generous, but with `NO CYCLE` the next allocation will throw on overflow rather than silently wrap. There is no alerting on the sequence approaching exhaustion. Pre-launch this is fine; post-launch a "if `nextval` is within 1M of MAXVALUE, page on-call" metric is worth wiring.

**Fix (eventually):** add a startup or periodic check: `SELECT last_value FROM public_uid_seq; ...` against `MAXVALUE`.

### M-V5-6. `RelationalFlush::FlushQuests` uses `to_timestamp(int)` for nullable timestamps — `to_timestamp(0)` returns 1970-01-01 UTC instead of NULL when handlers set startedAt=0 to mean "not set"
**File:** `Server/Account/src/Db/RelationalFlush.hpp:407-410`
**Status:** NEW (subtle)

The pattern at line 407:
```sql
CASE WHEN $5 = 0 THEN NULL ELSE to_timestamp($5) END
```
…relies on handlers using `0` as the "not started" sentinel. `PlayerQuestState::startedAt` is `std::time_t` (default-constructed to whatever `time_t{}` is — typically 0 on POSIX), so this works today. But:
1. If any future code path explicitly sets `startedAt = 1` (genuine 1970-01-01T00:00:01), the CASE doesn't fire and the row stores epoch-second-1.
2. The same pattern works for `completedAt`/`resetAt`.

The same idiom IS used safely on `accounts.last_streak_day` (RelationalFlush.hpp:72-73) but that one writes `DATE` not `TIMESTAMPTZ` and the M-V4-3 CHECK now constrains the value. The quest one has no equivalent guard.

**Fix:** model the optional timestamps as `std::optional<std::time_t>` on PlayerQuestState; pass `std::nullopt` through the flush layer. The `to_timestamp` CASE collapses to `$5` (pqxx already handles nullopt → SQL NULL). Lower-priority because the sentinel collision is theoretical.

---

## LOW / OBSERVATION

- **L-V5-1.** `events_wallet_recent_idx` (schema.sql:323-324) and `events_pulls_banner_idx` (325-326) have 0 scans in live DB — speculative indexes carried since v2. Same observation as v2-M2 / v4-L-V4-3.
- **L-V5-2.** `accounts.updated_at` (schema.sql:132) has DEFAULT now() but no trigger / no application code writes it explicitly. It's incremented in `FlushAccountsRow` (RelationalFlush.hpp:89) — fine, but the v2 H4 fix removed `last_login` from the brute-force flush; the comment at line 132 should be consistent ("written on every accounts_row dirty flush, not the same as last_login").
- **L-V5-3.** Live DB `outbox_dispatched_idx` (added M-V4-2) has 0 scans — `PruneDispatchedOutbox` clearly hasn't run yet against the 60 NULL-account_id debris rows (they're all dispatched=null per the test fixture pattern). Once OutboxRelay drains them, the index will start paying for itself.
- **L-V5-4.** `idempotency_cache_expires_idx` (schema.sql:445) is a non-partial btree on `expires_at`. With M-V4-1's bumped 24h TTL the sweep walks recent-time-window rows; a partial index `WHERE expires_at < now()` would be more selective. Trivial; defer until row count justifies.
- **L-V5-5.** `gear_substats.slot_idx CHECK BETWEEN 0 AND 3` plus `RelationalFlush.hpp:238` `std::min<std::size_t>(g.subStats.size(), 4)` cap — L4 v3 closure. Verified. The L4 comment at RelationalFlush.hpp:234-237 includes the WARN. Closed.
- **L-V5-6.** Schema's `partman` extension installation is correct: `Dockerfile.postgres` installs `postgresql-16-partman`. Verified extension is present: `\dx` on the live DB would show pg_partman. Match.
- **L-V5-7.** `seed.sql` still empty (45 lines all comments). L-V4-6 carry-forward; documented design.
- **L-V5-8.** Outbox rows in live DB (`SELECT count(*) FROM outbox` = 60) all `dispatched_at IS NULL` — pre-OutboxRelay-wiring debris. Once C-V4-1 wired the relay AND the AccountServer-startup loop has time to dispatch them, the 60-row debris should drain. After it does, `PruneDispatchedOutbox`'s 24h window will catch them.
- **L-V5-9.** `OutboxRelay::RunPartmanMaintenance` (OutboxRelay.hpp:146-152) uses `CALL partman.run_maintenance_proc()`. Postgres 16 supports CALL on procedures, fine. The H-V5-1 partman drift (premake=3) will silently keep the relay maintenance call "working" — it'll keep extending the 3-month look-ahead. No alert exists if the call fails persistently.

---

## Verified Closed (from v4)

| Item | Status | Verification |
|---|---|---|
| C-V4-1 wire OutboxRelay | **Closed** | `AccountServer.hpp:79 m_outboxRelay(m_pool)` + `:365 db::OutboxRelay m_outboxRelay` member |
| C-V4-2 Probe inspects body | **Out of scope** for this dimension (Networking) |
| H-V4-1 Begin() above TickQuests | **Out of scope** for this dimension (Event sourcing) |
| H-V4-2 RETURNING created_at | **Partial** — see M-V5-2 (microsecond truncation) |
| H-V4-3 outbox account_id FK + index | **Closed in code; live drift** — see M-V5-1 (60 NULL debris rows) |
| H-V4-4 ServiceClient timeout | **Out of scope** for this dimension (Networking) |
| H-V4-5 AccountCache/Hydrator tests | **Out of scope** for this dimension (Test coverage) |
| H-V4-6 C-V3-2 regression test | **Out of scope** for this dimension |
| H-V4-7 populated round-trip tests | **Out of scope** for this dimension |
| H-V4-8 idempotency tests | **Out of scope** for this dimension |
| H-V4-9 point-in-time doc | **Closed** — verified at AccountTransaction.hpp:114-163 (per-handler enumeration) |
| H-V4-10 connection caps | **Out of scope** for this dimension (Networking) |
| M-V4-1 drop reducer_version | **Closed** — schema.sql:124-128 comment block; live DB `\d accounts` confirms column absent |
| M-V4-2 outbox_dispatched_idx | **Closed** — schema.sql:398 + live DB `outbox_dispatched_idx` btree (dispatched_at) WHERE dispatched_at IS NOT NULL |
| M-V4-3 last_streak_day CHECK | **Closed** — schema.sql:89 + live `accounts_last_streak_day_bound CHECK (last_streak_day IS NULL OR last_streak_day >= '2020-01-01'::date)` |
| M-V4-4 events idempotency UNIQUE | **Carry-forward** — M-V5-4 above |
| M-V4-5 Save brute-force re-mark | **Open** — `AccountRepository::Save` (line 152-197) still brute-force-marks every collection dirty; not addressed in the 20-commit v4 arc |
| L-V4-7 universal_credits in INSERT | **Closed** — AccountRepository.hpp:96, 110 (explicit INSERT column + RETURNING-extracted into data.universalCredits) |

---

## Triage

**Immediate (today):**
1. **C-V5-1** — instantiate `SnapshotWriter` as an `AccountServer` member; wire `AccountTransaction::Commit` to `Enqueue` on the bumped aggregates. ~10 LOC.
2. **H-V5-1** — `UPDATE partman.part_config SET premake = 12 WHERE parent_table = 'public.events'` + `SELECT partman.run_maintenance_proc()`. One-shot psql command; no schema.sql change needed (it already says 12).

**This week:**
3. **H-V5-2** — decide policy on `accounts.public_uid` UNIQUE. Either build the profile-lookup RPC or annotate the constraint with planned-use.
4. **M-V5-1** — make `outbox.account_id` NOT NULL after fixing OutboxRelayTest to use seeded accounts; ALTER TABLE on the live DB (after DELETE-ing NULL debris).
5. **M-V5-3** — drop the `created_at = now()` bump from idempotency_cache ON CONFLICT.

**Before launch:**
6. **M-V5-2** — document the EXTRACT(EPOCH)::bigint truncation at the H-V4-2 fix site (or move to TO_CHAR(...) for full precision).
7. **M-V4-5 (carry-forward)** — actually replace `Save`'s bulk re-mark with `RelationalFlush::Flush` direct.
8. **M-V5-5** — public_uid_seq overflow monitoring.
9. **M-V5-6** — optional<time_t> for quest timestamps.

**Eventually:**
10. L items + M-V5-4 tracker.

---

## Audit-vs-fix scorecard (v4 → v5)

| v4 finding | Verdict |
|---|---|
| C-V4-1 OutboxRelay wire | **Closed in code AND live** (outbox_pkey idx_scan=1107 vs zero in v4 pre-fix; PumpOnce running) |
| H-V4-2 RETURNING created_at | **Partial** — code path is correct, but sub-second precision dropped. Demote to Low if pragmatically irrelevant |
| H-V4-3 outbox FK + index | **Closed in code; live DB carries 60 pre-fix NULL rows that need cleanup** |
| M-V4-1 drop reducer_version | **Closed** |
| M-V4-2 outbox_dispatched_idx | **Closed** (idx_scan=0 currently — no prune cycles have fired yet against drainable rows) |
| M-V4-3 last_streak_day CHECK | **Closed** |
| L-V4-7 universal_credits | **Closed** |

**Systemic risk this pass:** the v3→v4 "wiring-vs-implementation" pattern (OutboxRelay was written, never instantiated) is exactly the same pattern that hides C-V5-1 (SnapshotWriter is written, never instantiated). Both classes were authored in the same migration arc and both got integration tests. Neither got wired into AccountServer. The migration spec at `docs/superpowers/specs/2026-06-01-account-db-migration-design.md` should be cross-referenced against `AccountServer.hpp` member-init list as a literal checklist; deferred-instantiation is the load-bearing audit trap.
