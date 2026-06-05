# v6 followup — Persistence + Schema + DB State

**Date:** 2026-06-04 (followup to v5 2026-06-03)
**Scope:** `schema.sql`, `seed.sql`, `AccountRepository.hpp`, `RelationalFlush.hpp`, `EventStore.hpp`, `AccountTransaction.hpp`, `AccountHydrator.hpp`, `docker-compose.yml`, `Dockerfile.postgres`, plus live `aphelyon_postgres` introspection.
**Lens:** verify v5 persistence findings against current code AND live DB; flag substantive new gaps; honor anti-nitpick directive (no naming, no documentation, no pre-launch ops items).

---

## Status of v5 items in this dimension

| Item | v5 verdict | v6 verdict | Evidence |
|---|---|---|---|
| **H-V5-5 partman premake live drift** (schema says 12, live was 3) | OPEN | **CLOSED (live)** | `SELECT premake FROM partman.part_config WHERE parent_table='public.events'` → **12**. 17 partitions materialized 2026-03 through 2027-06 plus `events_default`. |
| **H-V5-7 accounts.public_uid UNIQUE dead surface** | OPEN | **STILL OPEN (documented)** | Live `accounts_public_uid_key.idx_scan = 0` against `accounts_pkey.idx_scan = 7930` and `accounts_username_key.idx_scan = 585`. The schema now carries an explicit DEFER block (schema.sql:71-81) tying the decision to the social-graph spec. Constraint pays INSERT cost; no reader. Acceptable as documented defer. |
| **M-V5-1 outbox NULL-account debris** (60 pre-fix rows + nullable column) | OPEN | **CLOSED (code + live)** | schema.sql:428 column is `BIGINT NOT NULL REFERENCES accounts(...) ON DELETE CASCADE`. Live `\d outbox` confirms `account_id ... not null`. Live outbox row count = 0 (debris cleaned). |
| **M-V5-2 epoch::bigint truncates microseconds** | PARTIAL | **PARTIAL (accepted)** | AccountRepository.hpp:110-123 carries an explicit accept-and-document block: "second-granularity contract is adequate ... upgrade when audit replay needs sub-second buckets." Demoted to Low by intent. |
| **M-V5-3 idempotency_cache.created_at refresh-overwrite** | OPEN | **CLOSED** | AccountTransaction.hpp:332-346 — `ON CONFLICT DO UPDATE SET` updates only `response_payload` and `expires_at`; `created_at` line removed. Documented at :317-322. |
| **M-V5-3 AccountRepository::Save brute-force re-mark** (M-V4-5 carry-forward) | OPEN | **STILL OPEN** | AccountRepository.hpp:181-194 still walks every collection and re-inserts into the dirty sets. No structural change. |

Cross-cutting v5 items still relevant here:
- **M-V5-1 event-sourcing AppendIdempotent recheck-lock** — **CLOSED** at EventStore.hpp:147-149 (advisory lock acquired in recheck tx, with `tx.commit()` to release).
- **C-V5-1 SnapshotWriter unwired** — **STILL OPEN (documented defer)**. AccountServer.hpp:356-366 carries a DEFER block tying the wire-up to four scaffolding pieces (REDUCER_VERSION constant, per-reducer ToJson/FromJson, last-snapshot-version tracking, projection-to-ReducerState). Live `snapshots` row count = 0. Acceptable pre-launch per the documented "current impact: zero" rationale; flagged in the v6 event-sourcing dimension report, not duplicated here.

**Live-DB observations:**
- partman premake = 12 (matches schema).
- 17 events partitions materialized; future runway covers through 2027-06.
- outbox: 0 rows, account_id NOT NULL enforced.
- idempotency_cache: 0 rows.
- 9 events total (4 wallet / 2 pulls / 2 quest_claims / 1 progression) across 13 accounts.
- 0 snapshots (C-V5-1 still defer).
- 0 audit_log rows — only ProgressionHandlers calls RecordAudit (scrap-spend path); no scrap-spend traffic in the dev DB. Not a finding.
- Dead-index situation: `accounts_public_uid_key` (0/7930), `outbox_dispatched_idx` (0 — prune hasn't had drainable rows), `outbox_account_idx` (0 — no GDPR scan exercised), `events_wallet_recent_idx` / `events_pulls_banner_idx` (0 — analytics not written).

---

## NEW findings

### [Medium-V6-1] World-flag mutations never set dirty bits — full silent data-loss path
**Files:**
- `Server/Common/src/State/WorldFlagStore.hpp:24-44` — `Set()` / `Clear()` / `Reset()` / `SetBatch()` / `ClearBatch()`
- `Server/Account/src/Cache/AccountRepository.hpp:181-194` — brute-force `Save()`
- `Server/Account/src/Db/RelationalFlush.hpp:35-36, 363-378` — `FlushWorldFlagAdds` / `FlushWorldFlagRemoves`
- `Server/Common/src/State/AccountDirty.hpp:24-25` — `world_flag_adds` / `world_flag_removes`

**Status:** NEW

`WorldFlagStore::Set` and `Clear` mutate the in-memory `m_flags` set with no callback to `account.MutableDirty().world_flag_adds`. `RelationalFlush::Flush` gates `FlushWorldFlagAdds` on `dirty.world_flag_adds.empty()`. Grep confirms zero production callers populate that dirty set — only `tests/Integration/PopulatedRoundTripTest.cpp:98` does, inline.

The brute-force `AccountRepository::Save` (the idle-eviction fallback) re-marks characters, weapons, gear, loadouts, party, pity, quests, and materials — but **omits world_flags entirely** (no loop over `account.GetWorldFlags()` enumerating into `dirty.world_flag_adds`).

**Today's actual impact:** zero. Grep `GetWorldFlags().Set` returns zero production callers — the only writer is `AccountRepository::LoadWorldFlags` during hydration. No handler ever flips a flag.

**Future-blocking impact:** the first handler that calls `account.GetWorldFlags().Set("tutorial_complete")` writes to memory and **never persists**. The mutation is invisible at:
1. Per-RPC commit (AccountTransaction → RelationalFlush gates on dirty bits).
2. Idle-evict (Save's brute-force loop doesn't enumerate flags).
3. Hydrate-after-evict (reload returns the pre-mutation DB state).

The plumbing looks complete, but the bridge is missing the central span. This is exactly the C-V3-1 / wiring-vs-implementation defect class the audit history keeps flagging — half-built persistence infra surfaces as silent data loss the moment a feature consumes it.

**Fix sketch:** option A (smallest): make `WorldFlagStore` hold a `DirtyState*` and have `Set/Clear` mark `world_flag_adds/removes`. Option B (less coupling): require callers to mark dirty themselves (handler convention) and add a Save-path enumeration into `world_flag_adds` for the brute-force case. Option C (defer): document the contract loudly at `WorldFlagStore::Set` ("DOES NOT MARK DIRTY — caller must `account.MutableDirty().world_flag_adds.insert(flag)`") and add the first production caller test that exercises the round-trip.

Recommend B for now: WorldFlagStore stays Common-pure; one Save-path loop + an explicit convention comment closes the trap.

### [Medium-V6-2] `AccountData.lastLogin` is a dead field — Hydrator drops it; never read
**Files:**
- `Server/Common/src/Types/AccountData.hpp:38` — `std::time_t lastLogin = 0;`
- `Server/Account/src/Cache/AccountRepository.hpp:140, 405` — populated on Create + LoadByAccountId
- `Server/Account/src/Cache/AccountHydrator.hpp:47-67` — `FromData` calls `SetCreatedAt` but never `SetLastLogin` (Account has no such setter)

**Status:** NEW

`Account` (Server/Account/src/State/Account.hpp:557) has `m_createdAt` but **no** `m_lastLogin`. The accounts.last_login column is owned exclusively by `AccountRepository::BumpLastLogin` (called from `VerifyCredentials` only), per the documented H4 fix. `AccountData.lastLogin` is loaded into the carrier struct and... goes nowhere. Grep `.lastLogin` returns only the two write sites in AccountRepository; no consumer reads it.

**Today's impact:** zero — the DB column is current and authoritative. The wire shape (InternalRpcHandlers serialization, VerifyCredentials response) doesn't expose it.

**Future-blocking impact:** any handler that wants to display "last session" or implement an inactivity-based decoration WILL reach for `AccountData.lastLogin`, find it populated, and get the value of `last_login` AT HYDRATION TIME — which is the BumpLastLogin write that happens at session start. So the value is actually correct for "session start time" but not "previous session end." A natural reader would assume "the previous lastLogin" and get the current session start, off by one.

**Fix sketch:** either (a) drop `AccountData.lastLogin` and the two RETURNING/SELECT bindings (10 LOC delete; closes a future trap), or (b) add a tracking comment at the struct field ("populated from accounts.last_login but never consumed; the column is owned by BumpLastLogin"). Recommend (a) — dead surface is the same defect class as M-V4-1's reducer_version drop. Pre-launch is the right time.

### [Low-V6-1] `outbox_account_idx` is a non-partial btree on a NOT-NULL column
**File:** `Server/Account/schema.sql:436`

**Status:** NEW (introduced by M-V5-1 closure)

Schema.sql:432-435 documents that the partial `WHERE account_id IS NOT NULL` predicate was dropped when the column went NOT NULL — correct. But the index now covers every outbox row, including the dispatched-and-pending mix. The intended use is the GDPR hard-delete CASCADE scan, which only runs on account deletion. Live `idx_scan = 0` across 0 outbox rows.

Pre-launch this is fine; not a substantive blocker. Tracked as a Low because the schema comment correctly explains why the partial was dropped, so the next pass won't re-litigate it.

### [Low-V6-2] FK on `outbox.account_id` to `accounts(account_id)` ON DELETE CASCADE — but no test covers the cascade path
**File:** `Server/Account/schema.sql:428`

**Status:** NEW

The H-V4-3 / M-V5-1 work added the FK + CASCADE for GDPR hard-delete. There is no integration test that exercises the cascade (insert outbox row → DELETE from accounts → assert outbox row gone). Pre-launch, with no GDPR delete RPC implemented, this is observation-grade. Worth a single Catch2 case under `OutboxRelayTest.cpp`'s sibling pattern when the GDPR path lands; defer until then.

### [Low-V6-3] `seed.sql` is still 45 lines of comments
**File:** `Server/Account/seed.sql`

**Status:** REPEAT (v4 L-V4-6, v5 L-V5-7)

Carry-forward observation. Documented design — accounts must be created through the Register RPC for the password hash to be valid. No action needed.

---

## Observations / Lows

- partman now has 12 months of pre-warmed partitions (through 2027-06). The OutboxRelay's hourly maintenance call is the only mechanism keeping this rolling; if Account.exe stays down for >12 months, INSERTs would start failing. Pre-launch this is a non-issue.
- `events.idempotency_key UNIQUE (account_id, idempotency_key, created_at)` is partition-scoped by design; cross-partition dedup lives on `idempotency_cache.PRIMARY KEY (account_id, scoped_key)`. Verified intentional — schema.sql:336, AccountTransaction.hpp:332-346. Carry-forward from v4 M-V4-4 / v5 M-V5-4, no action.
- Version-cursor invariants: `AccountTransaction::Commit` (AccountTransaction.hpp:374-382) bumps `cached_*_version` after `tx_->commit()` for each emitted event keyed by `aggregate_kind`. `DirtyState::Clear` (AccountDirty.hpp:68-80) preserves the four cursors across ClearDirty. `AccountHydrator` round-trips them from `data.dirty.cached_*_version` (AccountHydrator.hpp:79-83). The full chain is sound.
- `LoadEventVersions` (AccountRepository.hpp:646-676) loads MAX(version) per aggregate and logs an ERROR on unknown `aggregate_kind`. Good belt-and-suspenders.
- Hydration round-trip cross-check: `Save` writes everything `Hydrator` reads except (a) world_flags (Medium-V6-1 above), (b) last_login (Medium-V6-2 above — by design, but the carrier struct lies about it). Wallet/stats/collection/loadouts/party/pity/quests/materials all round-trip. Char-traces have an asymmetric path (`char_traces_added/removed` dirty sets are mutation-driven; Save doesn't enumerate them, but characters that gain traces would have been dirty-marked at the handler that granted them).
- OutboxRelay sweep cadences (OutboxRelay.hpp:154-163, schema.sql:476-481) now match: outbox prune every 60s, idempotency sweep every 65s, partman maintenance every 1h. v5 L-V5-1's "every minute" drift comment is closed.
- `outbox_dispatched_idx` (M-V4-2 ship) still shows 0 scans live — prune cycle hasn't had work to do since the debris was cleaned. Not a finding.
- The brute-force `AccountRepository::Save` (M-V4-5 carry-forward, still open) is the idle-evict fallback path. Today the per-RPC commit is the dominant write path; Save runs only on cache eviction, on a non-stale Account, with no pending mutations beyond what the per-RPC path already flushed. The brute-force re-mark is wasteful but not incorrect (idempotent UPSERTs cover the over-mark). Not promoting; v4 already accepted the deferral.
