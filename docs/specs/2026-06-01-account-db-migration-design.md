# Account DB Migration — Design Spec

**Date:** 2026-06-01
**Scope:** GachaServer / GachaAccount + GachaCommon
**Status:** Approved design, pending schema review against industry patterns, then implementation plan

## Problem

GachaAccount persists per-player state as one JSON file per account in `bin/*/GachaAccount/data/accounts/acc_<ts>_<rand>.json`, with a flat `index.json` mapping `username → account_id`. Concretely:

- **Current size:** ~2 KB per account today. Projected ~120 KB at a heavy-player profile (1000 weapons + 200 characters + 500 gear pieces + 50 quest states).
- **Write amplification:** Every state-mutating RPC (12 handler sites across 4 handler files) calls `m_ctx.saveAccount(account)`, which serializes the entire account and writes a new file via temp+rename. One pull = one full-file rewrite. A heavy session is ~10–20 writes per minute.
- **No indexed queries:** Filtered views (`GetByType`, `GetByState`, `GetExpiredStates`, `HasAll`, `HasAny`) on `questStates` and `worldFlags` walk the in-memory maps linearly because there is no other choice.
- **No transactional integrity:** A pull updates wallet + pity + collection + stats. If the file write fails midway today, the temp-rename pattern protects us — but coupling these mutations to one atomic file write means any partial state change forces a full rewrite.
- **No audit trail:** When a player loses a 5★ to a bug, there is no authoritative record of what they earned. Customer support is "we'll see what we can do" rather than "let's replay your pulls."
- **No path to leaderboards / social:** Future features (rank by pulls, friend graphs, guilds) have no foundation in the current layout.

## Goals / Non-goals

**Goals**
- Replace the per-player JSON file with a Postgres-backed schema that supports indexed queries, granular per-mutation writes, and transactional integrity.
- Adopt **selective event sourcing** on the four player-state aggregates where audit + replay deliver real value: wallet, pulls, quest claims, progression milestones.
- Establish the infrastructure (event store schema, snapshot strategy, determinism discipline, outbox pattern) that lets selective ES be done correctly the first time.
- Preserve the current concurrency model — 64-stripe per-player lock + in-memory account cache — as the application-level serialization layer.
- Fix the `GetQuestState` read-RPC-writes-disk pattern as part of this work (it's an addressable gap, surfaced by the new transaction boundary).
- Lay the foundation for future features (leaderboards, friend graphs, analytics) without committing to them in this spec.

**Non-goals (now)**
- GachaAuth persistence (still in-memory sessions; sessions evaporate on restart, which is acceptable for current scale).
- GachaCombat persistence (still a stub; spec it when the service grows beyond stub).
- Valkey integration (deferred to the spec that introduces the first leaderboard or social feature).
- Leaderboard schema / friend graph schema (deferred to the spec that needs them).
- Migrating existing JSON saves (per project dev policy: schema changes during dev delete saves; users re-register).
- Cross-region / multi-instance scaling (single-server now; the chosen stack scales to 100k+ DAU on one vertical Postgres before that becomes a problem).
- Distributed SQL (Cockroach/Yugabyte) — over-engineered until single-Postgres becomes the bottleneck.
- Full event sourcing across all state mutations — the industry consensus (Greg Young 2025, Chris Kiehl, EventSourcingDB, real games at HoYoverse/Riot/EVE) is selective ES, not all-state ES.

## Driver

Two converging drivers, both real:

1. **Scale.** One-file-per-player JSON is genuinely unsustainable beyond a few thousand active players. Write amplification grows with state size; filtered queries scale linearly with file size; backup is a directory copy. The migration to Postgres is the work the codebase needs irrespective of feature ambition.
2. **Player recovery for disputable events.** "I pulled and didn't get the 5★ I see in my history" is a customer support nightmare without an authoritative audit trail. Event sourcing the pull/wallet/quest-claim/progression paths makes this a deterministic replay rather than a judgment call. This is the killer feature.

The chosen stack — Postgres + selective ES + thin audit table for non-ES paths — is what shipping gachas/MMOs (EVE, HoYoverse, Riot, Pokémon GO) actually use, with the boundary at "money and grievance."

## Decisions

| Decision | Choice | One-line rationale |
|---|---|---|
| Database engine | **PostgreSQL 16+** | Industry-default OLTP for game backends; JSONB for heterogeneous nested data; libpqxx C++ driver is BSD-3, actively maintained; scales single-vertical to 100k+ DAU before sharding becomes necessary. |
| Dev environment | **Docker Compose** | Single `postgres` service; avoids Windows-native installer pain; standard for solo C++ dev on Windows. |
| Schema granularity | **Hybrid normalized + JSONB** | Separate tables for every row-explosion candidate (characters, weapons, gear, gear substats, quests, quest objectives, world flags, pity). JSONB only for genuinely heterogeneous freeform data (quest metadata). |
| Event sourcing scope | **Selective — 4 aggregates** | Wallet, Pulls, Quest claims, Progression milestones. Everything else: relational tables + thin audit log. |
| Concurrency model | **Keep 64-stripe lock + in-memory cache** | Stripe lock enforces game semantics (Pull + SetParty can't race); cache absorbs reads; Postgres transactions add durability, not concurrency control. |
| Write semantics | **Dirty-flag flush per RPC transaction** | Account fields private; mutations via setters that auto-mark dirty (per-table bits for scalars, per-id sets for row-per-entity tables). One Postgres transaction per RPC. |
| Event capture (RNG) | **Pattern C — hybrid** | Record `rng_state_before` + `algo_version` + `outcomes` + `state_after`. Replay reads outcomes; offline auditor verifies. |
| PRNG | **xoshiro256++** (vendored, header-only) | `std::mt19937` diverges across libstdc++/libc++/MSVC — unsafe for cross-platform replay. xoshiro is 256-bit state, bit-exact across compilers. |
| Snapshot cadence | **Every 200 events OR 7 days, async, latest-only, per-aggregate, `reducer_version`-tagged** | Predictable login latency, survives idle whales, cheap invalidation. |
| Schema evolution | **Weak schema + upcasters, immutable events** | Industry consensus (Greg Young, Marten, Axon, EventStoreDB); reducer version bump invalidates snapshots lazily. |
| Sequence numbers | **`events(account_id, version) UNIQUE` + global `BIGSERIAL`** | Marten/EventStoreDB pattern: per-stream version for correctness, global sequence for projection cursors. Optimistic concurrency via retry on `23505`. |
| Side effects | **Two channels** | In-process descriptors (replay-skippable) + Postgres outbox table (cross-service, dispatched by polling relay with `FOR UPDATE SKIP LOCKED`). |
| Migration approach | **Big bang, no importer** | Per project dev policy. Delete existing JSON saves; re-register test accounts. |
| Backup/DR | **WAL-G + S3-compatible bucket** | Continuous WAL archive + nightly full + 30/12/3 retention + quarterly restore drill. ~$10/mo, ~1 min RPO. |
| Instance IDs | **UUID v7 (16-byte `UUID` column)** | Replaces `wpn_<ts>_<rand>` TEXT strings. Time-ordered B-tree append, RFC 9562. Content IDs stay TEXT. |
| Idempotency | **`idempotency_key` UNIQUE on every event** | Client-supplied per request; prevents double-charge / double-grant on network retries. |
| Soft delete | **`accounts.deleted_at TIMESTAMPTZ` + scheduled hard-delete job** | GDPR-compliant 30-day horizon; preserves audit trail vs. immediate `ON DELETE CASCADE` everywhere. |
| Loadouts | **`loadouts` table with `preset_id`** (preset 0 = active) | Replaces `equipment` + `gear_equipment`. Reserves shape for relic/build presets without future data migration. |
| Material economy | **`material_inventory` table** | HSR-style ascension/trace/level-up consumes dozens of stackable material types; schema must support this from day one. |

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│ GachaAccount Service                                                     │
│                                                                          │
│ ┌─────────────────┐    ┌─────────────────────────────────────────────┐  │
│ │ Handlers        │    │ AccountService                              │  │
│ │ (Pull, Claim,   │───▶│  - StripedMutex<64>  (kept)                 │  │
│ │  Level, etc.)   │    │  - InMemoryAccountCache (kept)              │  │
│ └─────────────────┘    │  - IdleEvictor                              │  │
│                        └──────┬──────────────────────────────────────┘  │
│                               │                                          │
│                               ▼                                          │
│   ┌───────────────────────────────────────────────────────────────────┐ │
│   │ AccountRepository (new)                                           │ │
│   │  - Postgres ConnectionPool (libpqxx, 16 conns)                    │ │
│   │  - Per-RPC transaction wrapper                                    │ │
│   │  - Dirty-flag flush for relational tables                         │ │
│   │  - EventStore append + optimistic concurrency for ES aggregates   │ │
│   │  - Outbox dispatch for cross-service side effects                 │ │
│   └─────────┬─────────────────────────────────────┬─────────────────┘   │
│             │                                     │                      │
│             ▼                                     ▼                      │
│   ┌──────────────────────┐         ┌──────────────────────────────────┐ │
│   │ Relational state     │         │ Event-sourced aggregates         │ │
│   │ (12 tables)          │         │ (events + snapshots)             │ │
│   │ + audit_log          │         │ + outbox                         │ │
│   └──────────────────────┘         └──────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────────┘
                          │                              │
                          ▼                              ▼
                     ┌─────────────────────────────────────────┐
                     │  PostgreSQL 16+                         │
                     │  (Docker Compose in dev, vertical box   │
                     │   in prod, monthly-partitioned events,  │
                     │   LZ4-compressed JSONB)                 │
                     └─────────────────────────────────────────┘
```

The account in memory remains the source of truth *during a request*. The Postgres DB becomes the source of truth *between* requests. The cache is a write-through hot set keyed by player ID. The 64-stripe lock continues to enforce per-player serialization at the application layer.

## Schema — Relational (13 tables)

These hold all *non-event-sourced* player state. Mutations are direct UPDATEs through the dirty-flag flush mechanism.

```sql
-- Account header (scalars, counters, progression cursors)
accounts (
  account_id              BIGSERIAL PRIMARY KEY,
  username                TEXT NOT NULL UNIQUE,            -- replaces index.json
  password_hash           TEXT NOT NULL,
  created_at              TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_login              TIMESTAMPTZ NOT NULL DEFAULT now(),
  login_streak            INT NOT NULL DEFAULT 0,
  last_streak_day         DATE,
  last_login_claim_at     TIMESTAMPTZ,                     -- when the most recent daily reward was claimed
  last_login_claim_day_idx SMALLINT,                       -- position in the daily-reward cycle (0..6 etc.)
  story_level             INT NOT NULL DEFAULT 1,
  story_xp                INT NOT NULL DEFAULT 0,
  difficulty_tier         INT NOT NULL DEFAULT 1,
  reducer_version         INT NOT NULL DEFAULT 1,          -- snapshot invalidation marker
  deleted_at              TIMESTAMPTZ,                     -- soft delete; hard-delete job purges after grace period
  updated_at              TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX accounts_deleted_idx ON accounts (deleted_at) WHERE deleted_at IS NOT NULL;

-- Owned characters (bounded ~50 per account, by template_id)
owned_characters (
  account_id      BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  character_id    TEXT NOT NULL,                  -- template ID (content slug)
  level           SMALLINT NOT NULL DEFAULT 1,
  current_xp      INT NOT NULL DEFAULT 0,         -- XP toward next level
  ascension       SMALLINT NOT NULL DEFAULT 0,
  resonance       SMALLINT NOT NULL DEFAULT 0,
  PRIMARY KEY (account_id, character_id)
);

-- Per-character unlocked traces (set; varies per character)
char_traces (
  account_id      BIGINT NOT NULL,
  character_id    TEXT NOT NULL,
  trace_id        TEXT NOT NULL,
  PRIMARY KEY (account_id, character_id, trace_id),
  FOREIGN KEY (account_id, character_id) REFERENCES owned_characters
);

-- Owned weapons (instanced — one row per pull; UUID v7 for time-ordered B-tree append)
owned_weapons (
  account_id      BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  instance_id     UUID NOT NULL,                  -- UUID v7
  template_id     TEXT NOT NULL,                  -- content slug
  level           SMALLINT NOT NULL DEFAULT 1,
  current_xp      INT NOT NULL DEFAULT 0,
  ascension       SMALLINT NOT NULL DEFAULT 0,
  refinement      SMALLINT NOT NULL DEFAULT 0,
  acquired_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (account_id, instance_id)
);
CREATE INDEX owned_weapons_template_idx ON owned_weapons (account_id, template_id);

-- Owned gear (instanced — one row per drop; UUID v7)
owned_gear (
  account_id      BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  instance_id     UUID NOT NULL,                  -- UUID v7
  set_id          TEXT NOT NULL,                  -- content slug
  slot            SMALLINT NOT NULL,              -- GearSlot enum
  rarity          SMALLINT NOT NULL,
  level           SMALLINT NOT NULL DEFAULT 0,
  main_stat       SMALLINT NOT NULL,              -- StatType enum
  main_value      REAL NOT NULL,
  acquired_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (account_id, instance_id)
);
CREATE INDEX owned_gear_set_slot_idx ON owned_gear (account_id, set_id, slot);

-- Gear substats (0..4 per gear piece; normalized for future filter queries)
gear_substats (
  account_id      BIGINT NOT NULL,
  instance_id     UUID NOT NULL,
  slot_idx        SMALLINT NOT NULL CHECK (slot_idx BETWEEN 0 AND 3),
  stat_type       SMALLINT NOT NULL,
  value           REAL NOT NULL,
  PRIMARY KEY (account_id, instance_id, slot_idx),
  UNIQUE (account_id, instance_id, stat_type),    -- no duplicate substat type per gear
  FOREIGN KEY (account_id, instance_id) REFERENCES owned_gear ON DELETE CASCADE
);
CREATE INDEX gear_substats_stat_idx ON gear_substats (account_id, stat_type);

-- Loadouts (per character, multiple presets; preset 0 = active)
-- Replaces the old `equipment` + `gear_equipment` tables.
loadouts (
  account_id           BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  character_id         TEXT NOT NULL,
  preset_id            SMALLINT NOT NULL,         -- 0 = active; 1..N = saved presets
  name                 TEXT,                      -- user-given name; null for preset 0
  weapon_instance_id   UUID,
  slot_helmet          UUID,
  slot_gauntlets       UUID,
  slot_chest           UUID,
  slot_boots           UUID,
  updated_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (account_id, character_id, preset_id)
);
CREATE INDEX loadouts_active_idx ON loadouts (account_id) WHERE preset_id = 0;

-- Material inventory (stackable items: XP books, ascension mats, etc.)
material_inventory (
  account_id     BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  material_id    TEXT NOT NULL,                   -- content slug
  quantity       INT NOT NULL CHECK (quantity >= 0),
  PRIMARY KEY (account_id, material_id)
);

-- Quest state (heavily queried — GetByType, GetByState, GetExpiredStates)
quest_states (
  account_id      BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  quest_id        TEXT NOT NULL,
  quest_type      SMALLINT NOT NULL,              -- QuestType enum
  state           SMALLINT NOT NULL,              -- QuestState enum
  started_at      TIMESTAMPTZ,
  completed_at    TIMESTAMPTZ,
  reset_at        TIMESTAMPTZ,
  metadata        JSONB NOT NULL DEFAULT '{}'
                  CHECK (jsonb_typeof(metadata) = 'object'),
  PRIMARY KEY (account_id, quest_id)
);
CREATE INDEX quest_states_type_state_idx ON quest_states (account_id, quest_type, state);
CREATE INDEX quest_states_reset_idx     ON quest_states (account_id, reset_at)
  WHERE reset_at IS NOT NULL;
-- Active quests are <1% of historical rows but 100% of reads — partial index pays off:
CREATE INDEX quest_states_active_idx    ON quest_states (account_id, quest_type)
  WHERE state IN (1, 2);                          -- 1=ACTIVE, 2=CLAIMABLE (placeholder enum values)

-- Quest objectives
quest_objectives (
  account_id      BIGINT NOT NULL,
  quest_id        TEXT NOT NULL,
  objective_id    TEXT NOT NULL,
  progress        INT NOT NULL DEFAULT 0,
  required        INT NOT NULL,
  PRIMARY KEY (account_id, quest_id, objective_id),
  FOREIGN KEY (account_id, quest_id) REFERENCES quest_states
);

-- World flags (HasAll / HasAny queries; also hosts first-time-clear via `ftc:<content_id>` namespace)
world_flags (
  account_id      BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  flag            TEXT NOT NULL,
  unlocked_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (account_id, flag)
);

-- Pity state per banner slot
pity_state (
  account_id      BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  slot_id         TEXT NOT NULL,
  pity_4          SMALLINT NOT NULL DEFAULT 0,
  pity_5          SMALLINT NOT NULL DEFAULT 0,
  guarantee_5     BOOLEAN NOT NULL DEFAULT false,
  PRIMARY KEY (account_id, slot_id)
);

-- Party slots (normalized; lets us grow to 5/6 slots later without ALTER TABLE)
party_slots (
  account_id     BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  slot_idx       SMALLINT NOT NULL CHECK (slot_idx BETWEEN 0 AND 3),
  character_id   TEXT,                            -- null = empty slot
  PRIMARY KEY (account_id, slot_idx)
);
```

## Schema — Event-Sourced (4 streams)

Single `events` table, monthly RANGE partitioned on `created_at`. Each aggregate (wallet / pulls / quest_claims / progression) writes to this table with its own `aggregate_kind` discriminator and an independent per-account version sequence.

```sql
CREATE TABLE events (
  event_id        UUID NOT NULL DEFAULT gen_random_uuid(),  -- UUID v7 generated client-side; v4 fallback
  sequence        BIGSERIAL,                                 -- global cursor; may have gaps
  account_id      BIGINT NOT NULL,
  aggregate_kind  TEXT   NOT NULL,                           -- 'wallet' | 'pulls' | 'quest_claims' | 'progression'
  version         INT    NOT NULL,                           -- per (account_id, aggregate_kind), 1..N, gapless
  event_type      TEXT   NOT NULL,                           -- e.g. 'pull_performed'
  schema_version  INT    NOT NULL DEFAULT 1,
  data            JSONB  NOT NULL,
  metadata        JSONB  NOT NULL DEFAULT '{}',              -- source, request_id, actor
  idempotency_key TEXT   NOT NULL,                           -- client-supplied; prevents double-process on retry
  xid             XID8   NOT NULL DEFAULT pg_current_xact_id(),
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),

  -- Partitioned-table constraint: every unique/PK index must include the partition column (created_at).
  PRIMARY KEY (event_id, created_at),
  UNIQUE (account_id, aggregate_kind, version, created_at),
  UNIQUE (account_id, idempotency_key, created_at)
) PARTITION BY RANGE (created_at);

CREATE INDEX events_account_aggregate_version_idx
  ON events (account_id, aggregate_kind, version);
CREATE INDEX events_xid_seq_idx
  ON events (xid, sequence);
CREATE INDEX events_wallet_recent_idx
  ON events (account_id, created_at DESC) WHERE aggregate_kind = 'wallet';
CREATE INDEX events_pulls_banner_idx
  ON events (account_id, (data->>'banner_id'), created_at) WHERE aggregate_kind = 'pulls';

-- Initial partitions managed by pg_partman, monthly cadence.
```

**Why `(event_id, created_at)` as PK:** PostgreSQL requires unique constraints on a partitioned table to include the partition key. `event_id` is globally unique by construction (UUID v7), so the composite constraint enforces global uniqueness via per-partition local indexes without coordination overhead.

**Idempotency scope:** `(account_id, idempotency_key)`. Clients send a stable key with each mutation (UUID v4 generated client-side, reused on retry). A duplicate insert returns `23505`; the handler treats it as success and reads back the prior event.

**Per-stream version** is supplied by the application from the loaded aggregate's current version + 1. On `INSERT`, the `UNIQUE (account_id, aggregate_kind, version)` constraint catches concurrent writes; the application catches `SQLSTATE 23505` and retries after re-loading the aggregate.

**Event types per aggregate (initial vocabulary):**

| Aggregate | Event types |
|---|---|
| `wallet` | `credits_added`, `credits_spent`, `tickets_added`, `tickets_spent`, `limited_tickets_added`, `limited_tickets_spent`, `universal_credits_added`, `universal_credits_spent`, `scrap_added`, `scrap_spent`, `admin_adjustment_applied` |
| `pulls` | `pull_performed`, `multi_pull_performed` |
| `quest_claims` | `quest_reward_claimed` |
| `progression` | `story_level_advanced`, `difficulty_tier_advanced`, `story_xp_gained` |

Past-tense, verb-in-middle, no generic `updated`/`changed`. Cron and admin actions are events too (`daily_reset_applied`, `admin_adjustment_applied`) tagged in `metadata.source`.

**Example: `pull_performed` payload (Pattern C RNG capture):**

```json
{
  "banner_id": "char_event_001",
  "banner_version": "2.7",                       // disambiguates banner reruns
  "cost": { "currency": "tickets", "amount": 1 },

  "rng_state_before": "0x9e3779b97f4a7c15...",   // xoshiro256++ state, hex
  "algorithm_version": 1,

  "pity_5_before": 49,
  "pity_4_before": 7,
  "guarantee_5_before": false,

  "results": [
    {
      "template_id": "char_4star_001",
      "rarity": 4,
      "instance_id": null,                       // weapons get UUID v7 instances; characters keyed by template
      "was_featured": true
    }
  ],

  "pity_5_after": 50,
  "pity_4_after": 0,
  "guarantee_5_after": false
}
```

Multi-pulls are **one event** (`multi_pull_performed`) containing the full result vector — one business decision, one event. `banner_version` is mandatory on every pull event; without it, post-hoc analytics on banner reruns silently merge two distinct events.

## Schema — Support tables

```sql
-- Snapshots: latest-only per (account_id, aggregate_kind), tagged with reducer_version
CREATE TABLE snapshots (
  account_id       BIGINT NOT NULL,
  aggregate_kind   TEXT   NOT NULL,
  version          INT    NOT NULL,               -- last event version applied
  reducer_version  INT    NOT NULL,
  state            JSONB  NOT NULL,
  snapped_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (account_id, aggregate_kind)
);

-- Outbox: cross-service side effects, dispatched by polling relay
CREATE TABLE outbox (
  outbox_id     BIGSERIAL PRIMARY KEY,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  dispatched_at TIMESTAMPTZ,
  destination   TEXT NOT NULL,                    -- 'push_notification' | 'analytics' | 'friend_feed_rpc'
  payload       JSONB NOT NULL,
  xid           XID8 NOT NULL DEFAULT pg_current_xact_id()
);
CREATE INDEX outbox_pending_idx ON outbox (xid, outbox_id) WHERE dispatched_at IS NULL;

-- Audit log: thin who/when/before/after for NON-event-sourced mutations
CREATE TABLE audit_log (
  audit_id      BIGSERIAL PRIMARY KEY,
  account_id    BIGINT NOT NULL,
  occurred_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  actor         TEXT NOT NULL,                    -- 'player' | 'system' | 'admin:<id>'
  action        TEXT NOT NULL,                    -- 'set_party' | 'level_character' | 'refine_weapon' | ...
  target        JSONB NOT NULL,                   -- { table, primary_key }
  before        JSONB,
  after         JSONB,
  metadata      JSONB NOT NULL DEFAULT '{}'
);
CREATE INDEX audit_log_account_time_idx ON audit_log (account_id, occurred_at DESC);
```

The audit log is the recovery story for non-ES paths: if a player loses their party composition or active loadout, we can see exactly when it changed and roll back manually — without paying the full ES tax on those tables.

## Schema — Future seams (deferred, but shape documented to avoid corner-painting)

These tables are NOT in scope for this migration. They are documented here so that (a) the table names are reserved, (b) the implementation plan knows what shape to expect, and (c) the relational design doesn't accidentally close off the path to them.

### Friends / social — `friendships` (Nakama pattern)

Directed-edge storage, two rows per friendship (one per direction). Symmetric for `mutual`; asymmetric for `pending`. Supports geo-sharding by `source_account_id` later.

```sql
-- DEFERRED, not in this migration
friendships (
  source_account_id  BIGINT NOT NULL,
  target_account_id  BIGINT NOT NULL,
  state              SMALLINT NOT NULL,   -- 0=pending_outgoing, 1=pending_incoming, 2=mutual, 3=blocked
  created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (source_account_id, state, target_account_id)
);
```

### Mail / inbox — compensation gifts, event mail

Universal in gachas. Attachments delivered via the `outbox` table on send for exactly-once-ish semantics.

```sql
-- DEFERRED, not in this migration
mail (
  mail_id         UUID PRIMARY KEY,
  account_id      BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  sender          TEXT NOT NULL,                  -- 'system' | 'event:<id>' | 'admin:<id>'
  subject         TEXT NOT NULL,
  body            TEXT NOT NULL,
  attachments     JSONB NOT NULL DEFAULT '[]',    -- [{ kind, content_id, qty }, ...]
  sent_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at      TIMESTAMPTZ,
  read_at         TIMESTAMPTZ,
  claimed_at      TIMESTAMPTZ
);
CREATE INDEX mail_pending_idx ON mail (account_id, expires_at) WHERE claimed_at IS NULL;
```

### Achievements / first-time-clear

Folded into `world_flags` via flag namespace (`ftc:stage_1_3`, `achv:first_5star`). No separate table needed at this stage. If achievement metadata grows beyond "yes/no + timestamp" (progress bars, tiered rewards), a dedicated `achievements` table mirrors the `quest_states` shape.

### Battle / season pass

```sql
-- DEFERRED, not in this migration
season_pass (
  account_id        BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
  season_id         TEXT NOT NULL,
  tier              INT NOT NULL DEFAULT 0,
  xp                INT NOT NULL DEFAULT 0,
  is_premium        BOOLEAN NOT NULL DEFAULT false,
  claimed_tiers     INT[] NOT NULL DEFAULT '{}',  -- bitset alternative: BIT VARYING
  PRIMARY KEY (account_id, season_id)
);
```

When any of these lands, it gets its own spec + plan. The `outbox` table + audit log infrastructure built in this migration are reused as-is.

## Repository / Persistence Layer

A new `AccountRepository` in `GachaAccount/src/` replaces the JSON-file repository. Public surface:

```cpp
class AccountRepository {
public:
    // Connection pool — 16 libpqxx connections
    explicit AccountRepository(ConnectionPool& pool);

    // Existence / lookup
    std::optional<AccountId> FindByUsername(std::string_view username);
    bool UsernameExists(std::string_view username);

    // Account lifecycle
    Result<AccountId> Create(const NewAccountSpec& spec);
    void              Delete(AccountId id);              // dev-only; cascades
    std::unique_ptr<Account> Load(AccountId id);          // applies snapshots + tail events

    // Per-RPC transaction wrapper
    class Transaction {
      public:
        Transaction(AccountRepository& repo, AccountId id);
        ~Transaction();                                    // auto-rollback if not committed

        // Append events to ES aggregates (with optimistic concurrency)
        void AppendEvent(AggregateKind kind, Event ev);

        // Flush dirty relational tables (driven by Account's dirty marks)
        void FlushRelational(Account& account);

        // Emit cross-service side effects to outbox
        void EmitToOutbox(OutboxRecord rec);

        // Audit non-ES mutations
        void RecordAudit(AuditEntry e);

        // Commit or rollback
        void Commit();
        void Rollback();
    };

    Transaction Begin(AccountId id);
};
```

**Connection pooling.** libpqxx does not ship a built-in pool. A thin custom pool (~50 LOC) wraps `pqxx::connection` checkout/return with a semaphore. Pool size 16 is sufficient for one Account service with 64-stripe parallelism (most reads served from in-memory cache).

**Transaction boundary = RPC handler.** Each write handler calls `repo.Begin(playerId)` at entry, mutates the cached `Account`, and calls `Commit()` on success. Read handlers do not open a transaction (or open a read-only one). On `Rollback()`, the cached Account must be reverted — either by re-fetching from DB on the next access, or by snapshot/restore of pre-mutation state (decided in implementation).

**Dirty-flag flush.** Two layers of tracking on `Account`:
- **Scalar-table dirty bits:** one bit per scalar table (`accounts`, `pity_state` per slot).
- **Row-per-entity dirty sets:** per sub-collection, a set of changed IDs typed to match the table's PK. E.g., `unordered_set<string> dirtyCharacterIds` (TEXT content IDs), `unordered_set<uuid> dirtyWeaponInstanceIds`, `unordered_set<uuid> dirtyGearInstanceIds`, `unordered_set<string> dirtyQuestIds`, `unordered_set<string> dirtyWorldFlagAdds`, `unordered_set<string> dirtyWorldFlagRemoves`, `unordered_set<pair<string,int>> dirtyLoadoutKeys` (character_id, preset_id), `unordered_set<string> dirtyMaterialIds`, `unordered_set<int16> dirtyPartySlots`.

Mutations on `Account` go through setter methods that mark dirty automatically. Fields become private. There is no other way to mutate state — making it impossible for a developer to forget to dirty-flag.

```cpp
class Account {
    // ... fields private ...
public:
    // Scalar setters (mark account-row dirty)
    void SetCredits(int amount);
    void SetTickets(int amount);
    void AdvanceStoryXp(int delta);              // marks accounts dirty + emits progression event
    void SetPartySlot(int16_t idx, std::string character_id);
    // ...

    // Row-per-entity setters (mark id dirty)
    void SetOwnedCharacterLevel(const std::string& id, int level);
    void AddOwnedWeapon(OwnedWeapon w);          // generates UUID v7, marks weapon id dirty
    void RefineOwnedWeapon(uuids::uuid id);
    void AddOwnedGear(OwnedGear g);              // generates UUID v7
    void SetLoadoutSlot(const std::string& character_id, int16_t preset_id, GearSlot slot, uuids::uuid gear_id);
    void AddMaterial(const std::string& material_id, int quantity);
    void AddWorldFlag(const std::string& flag);
    void RemoveWorldFlag(const std::string& flag);
    // ...

    // Dirty access (used by Transaction::FlushRelational)
    const DirtyState& Dirty() const;
    void ClearDirty();
};
```

**Event sourcing append.** Handlers that mutate an ES aggregate (wallet / pulls / quest_claims / progression) emit events through the `Transaction`. The transaction loads the aggregate's current version (from snapshot + tail), constructs the event with `version = current + 1`, attempts INSERT, retries on unique violation. The cached Account's projection of the aggregate is updated synchronously after a successful append, so handler code can read the post-mutation state immediately.

**Order of operations within Commit():**
1. Append all queued events (with optimistic concurrency retry per event).
2. Flush dirty relational tables.
3. Insert outbox rows.
4. Insert audit log rows.
5. Update `accounts.next_event_seq` (per-aggregate counters held elsewhere — see implementation).
6. `COMMIT`.
7. Mark cache fields clean.

Step 7 happens AFTER successful commit. If commit fails, the cache is reverted (next access re-fetches from DB).

## Concurrency

Unchanged from current design except where noted:

- **`StripedMutex<64>`** continues to serialize per-player access. Acquired at handler entry, released after RPC response.
- **In-memory `Account` cache** continues to lazy-load on first access and idle-evict after 5 minutes. **Eviction no longer triggers a save** — writes are already durable. Eviction is pure RAM-free.
- **Postgres isolation level: `READ COMMITTED`** (default). The stripe lock provides the player-level serialization that would otherwise require `SERIALIZABLE`.
- **No `SELECT FOR UPDATE`, no advisory locks** — application-layer stripe lock makes them redundant.

Two writers for the same account from within the Account service is prevented by the stripe lock. Two writers from different services (Auth and Account) is prevented by service responsibility split — only Account writes account-stream events.

## Event Sourcing Patterns

### RNG capture — Pattern C

Per-account `xoshiro256++` state stored in the latest pull-stream snapshot. Each `pull_performed` event records the **state before** the pull, the **algorithm version**, the **resolved outcomes**, and the **state after**. Replay reads outcomes directly (authoritative); an offline auditor independently re-rolls from `rng_state_before` and verifies the outcomes match.

The algorithm version field lets the auditor know to skip re-roll verification on events from before a `GachaRNG` change (the outcomes remain authoritative on replay; only the *audit* loses verification capability for old events). Outcomes stay correct forever; this is the point of Pattern C.

### PRNG

**xoshiro256++** vendored as header-only in `ThirdParty/xoshiro/` (matches CLAUDE.md vendoring preference). Seeded via SplitMix64 from `account_id XOR created_at`. 256-bit state fits trivially in the pull-stream snapshot.

`std::mt19937` is removed from `GachaRNG`. Cross-platform replay is not safe with `std::mt19937` — `std::seed_seq` behavior and `std::uniform_int_distribution` diverge across libstdc++, libc++, and MSVC.

### Snapshots

Per-aggregate, per-account `snapshots` row. Written **asynchronously after the RPC response** (does not extend client latency). Cadence: every 200 events since last snapshot for this (account, aggregate), OR 7 days, whichever fires first. Only the latest snapshot is retained — older snapshots are deleted by the snapshot writer.

On load: `SELECT state, version, reducer_version FROM snapshots WHERE account_id = $1 AND aggregate_kind = $2`. If `reducer_version` mismatches the current `REDUCER_VERSION` constant, the snapshot is **ignored** and the aggregate replays from event 0. Otherwise: replay events with `version > snapshot.version`.

Snapshot atomicity: not required to be transactional with event append. Worst case after crash: a snapshot exists for version N but the latest event written is < N — load logic ignores snapshots ahead of the event log. Best case: snapshot stale by some events, replay applies the tail.

### Schema evolution

Events are **immutable**. Two evolution mechanisms:

1. **Weak schema (additive):** new optional fields with defaults. No version bump needed.
2. **Upcasters:** for breaking shape changes, a per-event-type function `(old_schema_version, json) → new_json` runs at deserialization. Bump `schema_version` in newly-written events. Old events get upcasted on read.

When **reducer logic changes** (not event shape — interpretation), bump the global `REDUCER_VERSION` constant. All existing snapshots become inert (loaded but ignored); state rebuilds from event 0 on next access and a fresh snapshot is written.

Never edit historical events. If a bug caused wrong state, write a **compensating event** (`pity_corrected`, `credits_admin_adjustment`) with `metadata.source: "compensation"`.

### Determinism enforcement (layered defense)

1. **Pure-function reducer signature.** Reducers take `const State&`, `const Event&`, `const Clock&` (injected), `RngState&` (from the event payload). No global access, no static state, no I/O.

2. **clang-tidy custom check** (`gacha-reducer-purity`) applied to `GachaAccount/src/reducers/`. Bans:
   - `std::chrono::system_clock::now`, `std::time`, `time()`, `clock_gettime`
   - `std::random_device`, free-function `rand()`, `std::mt19937` constructors
   - `std::filesystem::*`, `std::getenv`, `std::system`
   - Network / socket calls

3. **rapidcheck property tests** (vendored or vcpkg, decision deferred to implementation):
   - **Triple-replay equality:** `fold(initial, log) == fold(initial, log) == fold(initial, log)` for any random event log.
   - **Snapshot-equivalence:** `fold(initial, log) == fold(fold(initial, log[:k]), log[k:])` for any split point k.
   - **Domain invariants:** pity never exceeds hard-pity threshold; currency never negative; etc.

4. **Golden-file production replay** (CI). Anonymized event sequences captured from production land in `tests/golden-histories/`. CI replays each and fails on any divergence from the checked-in expected end state.

### Side-effect channels

Two channels, both terminate in the same Postgres transaction as the event append:

**Channel A — In-process descriptors (replay-skippable):**
The existing `CollectionReducer` already returns `vector<SideEffectVariant>` (UI toasts, telemetry events). Pattern is preserved and extended to wallet/pulls/quest_claims/progression reducers. The orchestrator dispatches descriptors in live mode and **skips them entirely in replay mode**. A `bool isReplay` parameter is threaded through the dispatch.

**Channel B — Postgres outbox table (cross-service / exactly-once-ish):**
Side effects that must reach another service (push notifications, friend-feed RPC, analytics) are written to the `outbox` table in the same transaction as the event. A polling relay thread in the GachaAccount process reads with `SELECT ... FOR UPDATE SKIP LOCKED`, dispatches, marks `dispatched_at`. Extraction to a separate worker process is a future scaling step, not part of this spec. Idempotency is the recipient's responsibility.

**State transitions are NOT side effects.** Granting a pulled character/weapon to inventory is part of the event's state transition and lives in the reducer. Notification "you got a 5★!" IS a side effect (Channel A). Push notification to the player's mobile device IS a side effect (Channel B).

### Sequence numbers

- Per-aggregate `version` is stored only in the `events` table (`UNIQUE (account_id, aggregate_kind, version)`). It is NOT duplicated on the `accounts` row.
- Application maintains `current_version` per (account, aggregate) in the in-memory cache, loaded by `SELECT MAX(version)` once when the aggregate is first touched. On append: `INSERT ... version = current_version + 1`. On `23505` unique violation: re-load aggregate, retry. Bounded retry (3 attempts) before failing the RPC.
- Global `BIGSERIAL sequence` column on `events` is for the future projection daemon / read-model cursor. Not consulted by write path.
- Gaps in `sequence` are expected (rolled-back transactions, in-flight commits). Projection daemon reads with `WHERE xid < pg_snapshot_xmin(pg_current_snapshot())` to skip gaps safely (per Event-Driven.io ordering guide).

### Action vocabulary scope

Every state change that must be reproducible from replay gets an event. Including:
- All player actions on the 4 aggregates.
- Cron / scheduled mutations on those aggregates (`daily_reset_applied`).
- Admin / support actions (`admin_adjustment_applied`, with `metadata.actor` and `metadata.reason`).

Excluded (these go to relational tables + audit log):
- Settings, party slots, equipment changes.
- World flag set/clear.
- Quest objective progress (until claim — claim is the event).
- Pity counters (derived from pull stream).
- Login streak / last_login_at.
- Cache eviction, internal operational state.

Naming convention:
- Past-tense verbs in the middle: `account.pull_performed`, `wallet.credits_spent`, `quest_claims.quest_reward_claimed`.
- Snake_case in payload field names.
- Namespaced by aggregate.
- Ban generic `updated`/`changed`/`modified`.

## Migration

Per project save policy (deletes saves on schema changes during dev, no migration code):

1. Stop GachaAccount.
2. Delete `bin/*/GachaAccount/data/accounts/` entirely.
3. Drop the dev Postgres database (`docker compose down -v` to nuke the volume).
4. Apply the migration files (Phase 1 in implementation plan) to a fresh database.
5. Restart GachaAccount; test accounts re-register on next login.

No JSON importer is built. No compatibility shim. Account tables start empty; users re-register; pull-streams start at version 1.

**Dev environment setup:**
- New `GachaServer/docker-compose.yml` with one `postgres:16` service, mounted volume for `pgdata`, exposed on `5432`.
- New `GachaServer/scripts/db-setup.bat` (Windows) and `db-setup.sh` (cross-platform) that wait for the container, apply migrations, and seed minimal data.
- Migrations live in `GachaServer/GachaAccount/migrations/`. Plain `.sql` files, numbered. Applied by a small custom runner (not Flyway / Liquibase — vendoring preference and minimum-viable for now).
- `connection_string` configured via `data/db.json` in the Account service's working dir, with a default for `postgres://gacha:gacha@localhost:5432/gacha`.

## Testing

Pyramid for selective-ES correctness:

1. **Reducer unit tests (most).** One test file per reducer. Table-driven: input state + event → expected output state + expected side effect descriptors. Fast.
2. **Reducer property tests** (rapidcheck). Three properties:
   - Triple-replay equality.
   - Snapshot equivalence (split-and-fold).
   - Domain invariants per reducer (currency non-negative, pity bounded, etc.).
3. **Schema migration golden-file tests.** For every event version: `tests/events/v{N}_{event_type}.json` is checked in. CI loads, upcasts to current, processes, asserts against a checked-in expected state file. Highest-leverage long-running ES test.
4. **Production-history replay** (when production exists). Anonymized real event sequences in `tests/golden-histories/` replayed in CI; fail on divergence.
5. **Integration tests** (testcontainer-postgres or local docker). Real Postgres, real `AccountRepository`, real transaction round-trip, real outbox dispatch verification. Fewest, slowest.

## Storage and Operations

**Partitioning:** `events` partitioned by `RANGE (created_at)` monthly. `pg_partman` extension manages partition creation and rotation. Initial partitions created in migration script.

**Compression:** LZ4 column compression on `events.data`, `events.metadata`, `snapshots.state`, `audit_log.before`, `audit_log.after`. Postgres TOAST defaults are conservative; LZ4 chosen for ~80% faster insert with query parity, per credativ benchmark.

**Retention:** Never truncate. After 12 months, detach old partitions to a cheaper tablespace on slower disk (no automatic deletion). Gambling-style retention for pull events is non-negotiable; HoYoverse / Riot patterns hide-in-UI-but-retain-server-side.

**Backup (initial, solo dev tier):**
- WAL-G continuous archiving to S3-compatible bucket (B2 / R2 cheaper than AWS S3; same protocol).
- Nightly full base backup via `wal-g backup-push`.
- Bucket lifecycle: 30 daily / 12 monthly / 3 yearly. Versioning + object-lock enabled.
- **Quarterly restore drill** to a throwaway VM. A backup that has never been restored is theoretical.
- Approximate cost: $5–20/month. RPO ≈ 1 minute. RTO ≈ 30 minutes unattended.

**Scaling later (when DAU justifies):**
- Synchronous standby replica in a second host (`synchronous_commit = remote_apply`): RPO → 0.
- Cross-region async replica for regional disaster recovery.
- Monthly partition detach when `events` exceeds ~50M rows (well past 10k DAU).
- Move full backups to a second cloud (the "2" in 3-2-1).

## Out of Scope

- **GachaAuth persistence.** Auth remains in-memory (sessions evaporate on restart). Acceptable for current scale; revisit in a dedicated spec when restart-survival becomes required.
- **GachaCombat persistence.** Combat service is a stub; persistence specced when the service grows beyond stub.
- **Valkey / Redis integration.** Deferred to the spec that introduces leaderboards or friend graphs.
- **Leaderboard schema, friend graph schema.** Deferred.
- **Event-sourcing additional aggregates** (combat history, social interactions, etc.). Add as needed in separate specs.
- **Distributed SQL** (Cockroach / Yugabyte / Spanner). Single Postgres handles target scale; revisit at 100k+ DAU or multi-region requirements.
- **Multi-tenant or sharded deployment.** Single-tenant single-server now; sharding can be added at the service layer when needed without re-architecting.

## Follow-ups before implementation plan

- **Schema review against industry patterns for games like ours.** Current schema is essentially a 1:1 mapping of today's `AccountData` shape into normalized tables. Worth a comparative pass against MMO/gacha-standard data layouts (player/character separation, inventory item-bag patterns, currency wallet conventions, item instance lineage) to surface any structural improvements before locking in the migration. This is the next discussion before writing-plans is invoked.

## Sources / research artifacts

This design is grounded in (synthesized in conversation prior to writing this doc):

- Greg Young, *Event Centric* (Dec 2025); various 2023–2025 talks and posts on selective ES.
- Chris Kiehl, *Don't Let the Internet Dupe You, Event Sourcing is Hard*.
- EventSourcingDB, *Event Sourcing is Not For Everyone* (Nov 2025).
- Kasey Speakman, *Event Storage in Postgres*; Tim Derzhavets, *Production-Ready Event Store in PostgreSQL* (2025).
- Marten documentation (snapshots, versioning, aggregate patterns).
- Kurrent / EventStoreDB documentation.
- credativ, *TOASTed JSONB Performance Benchmark*.
- Jack Vanlightly, *Demystifying Determinism in Durable Execution* (Nov 2025).
- Bitovi, *Replay Testing for Temporal Workflows*.
- EVE Online journal architecture (datacenterknowledge.com, eveonline.com news).
- Pokémon GO / Google Cloud architecture posts.
- Riot Globalizing Player Accounts (technology.riotgames.com).
- Tencent Games event-driven analytics (thenewstack.io).
- Severalnines / WAL-G / pgBackRest backup tool comparisons.
- Vigna, *PRNG Shootout* (xoshiro256++ properties).
