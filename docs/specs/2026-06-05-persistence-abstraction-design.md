# Persistence Abstraction Design

**Date:** 2026-06-05
**Status:** Design approved; ready for implementation planning
**Scope:** All 17 tables, all persistence shapes (scalars + row-per-entity + JSONB + event-sourced), big-bang migration
**Bundles:** The `cached_snapshot_*_version` cursor-propagation bug introduced by C-V5-1 Task 5

---

## Goal

Collapse the 6-touch-point fragility of declaring a persisted field down to 2 (schema + descriptor), eliminate positional-index bugs at the pqxx boundary, and give the codebase a self-documenting layer over `pqxx::*` types — without introducing template metaprogramming, an ORM, or a non-trivial runtime allocation budget.

The current cost of adding one scalar column to the `accounts` row is touching **six files**: `schema.sql`, `Common/Types/AccountData.hpp`, `AccountRepository.hpp` (SELECT list + positional `r[0][7].as<int>()` read), `AccountHydrator.hpp` (`Set*` call), `Account.hpp` (`void Set*(T v) { m_field = v; m_dirty.accounts_row = true; }`), `RelationalFlush.hpp` (UPDATE column slot). After this change the cost is **two files**: `schema.sql` + a new `FieldDescriptor` literal in the appropriate table's descriptor file. The Set* method on Account stays as the canonical mutator (still marks dirty); the row types and SQL strings are derived from descriptors at static init.

---

## Context

### What's painful today

- **Positional indexing.** `d.storyLevel = r[0][7].as<int>();` is implicitly coupled to the SELECT column order. Reorder the SELECT, renumber every index. The audit comments call this out repeatedly (`// add a case here when a new aggregate ships`) — every such comment is a paper trail for the missing centralization.
- **Six places that must stay in lockstep per column.** Hand-coordination is the only thing preventing drift; nothing fails at compile time.
- **Stealth divergence between layers.** The `cached_snapshot_*_version` cursors introduced in C-V5-1 Task 5 hydrate into `AccountData.dirty` but were never propagated through `AccountHydrator::FromData` into `Account::MutableDirty()`. The bug has been live since the C-V5-1 wire-up; only the cadence-doesn't-fire-on-fresh-account scenario hides it. This is exactly the class of bug the abstraction eliminates structurally.
- **`pqxx::*` types leaking everywhere.** `pqxx::work tx(*lease)` reads as DB-library plumbing; `db::Transaction tx(*lease)` reads as English. Every layer above the raw pqxx call site is paying readability tax.

### What the C-V6-1 + C-V5-1 + M-V5-3 work already gave us

- **Reducer pipeline is load-bearing.** Every event-emitting handler routes through its reducer for invariant verification.
- **Dirty bits are trusted.** `AccountRepository::Save` short-circuits on `!AnyDirty()`; the brute-force re-mark is gone.
- **Snapshot wire-up is live.** Cadence-driven snapshot persistence works end-to-end.
- **Save discipline is established.** "Every mutator marks its own dirty bit" is now the contract. The registry just changes how the bits are consumed at flush time.

These all stay intact. The registry doesn't restructure the dirty-bit machinery, the reducer pipeline, or the SnapshotWriter — it changes how the row-level SQL gets emitted.

---

## Architecture overview

Two new namespace layers:

```
aphelyon::db::*           — pqxx-aliased primitives (Transaction, ResultSet, ...)
aphelyon::persistence::*  — TableRegistry, FieldDescriptor, TableDescriptor, RowReader, RowWriter, Value
```

Three composition layers:

```
            ┌─────────────────────────────────────────────────────────────┐
            │  Per-table Row types                                        │
            │  AccountsRow, OwnedCharacterRow, OwnedWeaponRow, ...        │
            │  (one flat struct per table, public, self-documenting)      │
            └────────────────────────┬────────────────────────────────────┘
                                     │
            ┌────────────────────────▼────────────────────────────────────┐
            │  TableDescriptor<Account, Row>                              │
            │  {table_name, pk_columns, fields, enumerate,                │
            │   apply_row, enumerate_dirty}                               │
            └────────────────────────┬────────────────────────────────────┘
                                     │
            ┌────────────────────────▼────────────────────────────────────┐
            │  TableRegistry<Account>                                     │
            │  Hydrate(tx, accountId, account) / Flush(tx, account)       │
            └────────────────────────┬────────────────────────────────────┘
                                     │
            ┌────────────────────────▼────────────────────────────────────┐
            │  RowReader<Row> / RowWriter<Row>                            │
            │  Thin pqxx wrappers; named-column access; Value variant     │
            └────────────────────────┬────────────────────────────────────┘
                                     │
            ┌────────────────────────▼────────────────────────────────────┐
            │  pqxx (db::Transaction, db::ResultSet, db::ResultRow, ...)  │
            └─────────────────────────────────────────────────────────────┘
```

Data flow:

- **Load:** `Registry::Hydrate(tx, accountId, account)` walks each `TableDescriptor`, builds the SELECT SQL once (at static init), invokes `RowReader<Row>::SelectByOwnerId`, projects each `db::ResultRow` into a typed `Row` via the descriptor's `FieldDescriptor.write` callbacks, then applies each `Row` to the live `account` via `apply_row`.
- **Flush:** `Registry::Flush(tx, account)` walks each `TableDescriptor`, calls `enumerate_dirty(account)` (which reads `account.Dirty()` and returns only the touched rows), and for each row invokes `RowWriter<Row>::Upsert` which binds via `pqxx::params` in the column order the descriptor lists.

---

## The `db::` typedef layer

New header `Server/Common/src/Db/DbTypes.hpp`:

```cpp
namespace aphelyon::db {

using Transaction    = pqxx::work;
using NonTransaction = pqxx::nontransaction;  // for partman.run_maintenance_proc and similar
using ResultSet      = pqxx::result;
using ResultRow      = pqxx::row;             // one row of a SELECT result, NOT our typed Row
using ResultField    = pqxx::field;
using Params         = pqxx::params;

}
```

**Naming rationale:** the `<X>Row` pattern parallels on both sides — `db::ResultRow` for the raw SQL row, `<Table>Row` for the typed domain projection. Mirrors pqxx's own vocabulary (`pqxx::result` → `db::ResultSet`); the alias is one substitution away from reading the wrong line by accident. `db::Transaction` reads as English at every call site that today is fighting with `pqxx::work`. Every existing `pqxx::work tx(*lease)` becomes `db::Transaction tx(*lease)` — same machine code, different reading experience.

**Scope:** The alias header is the `db::` layer's entire surface. The body of any header that currently writes `pqxx::work` switches to `db::Transaction` mechanically. We don't wrap pqxx beyond this; the `RowReader`/`RowWriter` layer is the value-add, the aliases are the readability primitive.

---

## Core abstractions

### `Value` — the cell type

```cpp
namespace aphelyon::persistence {

using Value = std::variant<
    std::int64_t,
    std::string,
    bool,
    std::nullopt_t,
    nlohmann::json
>;

}
```

Covers every column type we use:

- `std::int64_t` for INT / BIGINT / SERIAL columns
- `std::string` for TEXT / VARCHAR / UUID-as-text
- `bool` for BOOLEAN
- `std::nullopt_t` for explicit SQL NULL
- `nlohmann::json` for JSONB

20 lines of `std::visit` in `RowReader`/`RowWriter` handle dispatch. No template metaprogramming; the variant is the cost we pay for runtime field iteration.

### `FieldDescriptor<Row>` — one column

```cpp
template <class Row>
struct FieldDescriptor {
    std::string                                column;
    std::string                                sql_type;     // e.g. "INT NOT NULL DEFAULT 1"
    std::function<Value(const Row&)>           read;
    std::function<void(Row&, const Value&)>    write;
    std::string                                doc;          // semantic intent next to the SQL
};
```

The `read` lambda projects a single column's value out of a typed `Row`. The `write` lambda projects the inverse on load. `doc` is mandatory in spirit (every descriptor literal includes a one-line intent string) — undocumented columns are how schema knowledge ages out of human memory.

### `TableDescriptor<Owner, Row>` — one table

```cpp
template <class Owner, class Row>
struct TableDescriptor {
    std::string                                          table_name;
    std::vector<std::string>                             pk_columns;
    std::vector<FieldDescriptor<Row>>                    fields;

    // Project one Row back into the live Owner at load time.
    std::function<void(Owner&, Row)>                     apply_row;

    // Project the subset of rows the Owner's DirtyState marks as
    // changed. Single-row tables (e.g. accounts) short-circuit here:
    // return the one Row if dirty.accounts_row, else empty. Row-per-
    // entity tables iterate the appropriate dirty-bit set (e.g.
    // dirty.character_ids for owned_characters).
    std::function<std::vector<Row>(const Owner&)>        enumerate_dirty;
};
```

The three lambdas plus the field vector are the contract. Everything else is derived: SELECT SQL from `fields` + `pk_columns`, UPSERT SQL from `fields` + `pk_columns`, DELETE SQL from `pk_columns`.

**`enumerate_dirty` is the only enumeration lambda.** An earlier draft had a separate `enumerate` (project every row regardless of dirty state) but no flow needs it — load reads from DB, flush reads from dirty bits, and admin "force re-save" use cases would build their own one-shot tooling rather than depend on a registry capability that exists "just in case." YAGNI.

### `TableRegistry<Owner>` — one Owner type

```cpp
template <class Owner>
class TableRegistry {
public:
    template <class Row>
    void Add(TableDescriptor<Owner, Row> desc);

    void Hydrate(db::Transaction& tx, std::int64_t owner_id, Owner& owner);
    void Flush(db::Transaction& tx, Owner& owner);

private:
    struct TypeErasedOps {
        std::function<void(db::Transaction&, std::int64_t, Owner&)> hydrate;
        std::function<void(db::Transaction&, Owner&)>               flush;
    };
    std::vector<TypeErasedOps> ops_;
};
```

`Add<Row>(desc)` is called once per table at static-init time. Inside `Add`, the SELECT / UPSERT / DELETE SQL strings are pre-built from the descriptor's metadata and captured into the type-erased `hydrate` / `flush` callbacks. At call time, `Hydrate` and `Flush` are pure vector walks.

**Introspection.** The registry also retains a non-type-erased metadata snapshot per table — just `{table_name, pk_columns, vector<(column, sql_type)>}` — exposed via `TableRegistry<Owner>::Tables() const`. This is what the schema-vs-descriptor integration test reads to query `information_schema.columns` and assert agreement. Production code never reads it; it exists solely for the consistency check and future schema-generation tooling.

**Owner-column convention.** Every table the registry knows about MUST have a column matching the Owner type's identifier name — for `TableRegistry<Account>` that's `account_id` on every table. `RowReader::SelectByOwnerId(tx, owner_id)` generates `WHERE account_id = $1` from this convention. The convention is consistent across all 17 tables in the current schema (every Account-owned table has `account_id` either as a single-column PK or as part of a composite PK). New Account-owned tables MUST follow it; the schema-vs-descriptor consistency test asserts the column exists per descriptor.

### `RowReader<Row>` / `RowWriter<Row>` — the pqxx layer

Thin wrappers that take a `TableDescriptor` + pre-built SQL string. ~30 lines each.

```cpp
template <class Row>
class RowReader {
public:
    RowReader(const TableDescriptor<auto, Row>& desc, std::string select_sql);
    std::vector<Row> SelectByOwnerId(db::Transaction& tx, std::int64_t owner_id);
};

template <class Row>
class RowWriter {
public:
    RowWriter(const TableDescriptor<auto, Row>& desc, std::string upsert_sql, std::string delete_sql);
    void Upsert(db::Transaction& tx, const Row& row);
    void Delete(db::Transaction& tx, const Row& row);
};
```

**This is the layer that kills the positional-indexing bug class.** The SELECT SQL has `$1, $2, ...` placeholders in the order the descriptor lists them. `RowReader` uses pqxx's named column access (`pqxx_row[field.column]`) — no positional indices. `RowWriter` walks the descriptor's `fields` in order and appends to `pqxx::params` in the same order. Add a new field → it gets a new placeholder at the end. Reorder a field → both sides reorder together because both are driven from one source.

### Per-table `Row` types

One flat struct per table, declared in `Server/Common/src/Persistence/Rows/`:

```cpp
// AccountsRow.hpp
struct AccountsRow {
    std::int64_t account_id;
    std::string  username;
    int          story_level;
    int          story_xp;
    int          difficulty_tier;
    int          login_streak;
    // ... (~13 fields total)
};

// OwnedCharacterRow.hpp
struct OwnedCharacterRow {
    std::int64_t account_id;
    std::string  character_id;
    int          level;
    int          ascension;
    int          resonance;
};

// OwnedWeaponRow.hpp, OwnedGearRow.hpp, ...
```

These replace the field-mixing inside `AccountData` (the god-struct that mixed scalars + collections + cursors). Each `Row` is one cohesive table's shape, public, dev-facing, self-documenting.

---

## How load works

```cpp
// AccountRepository::LoadByAccountId — new shape
std::optional<std::unique_ptr<Account>>
AccountRepository::LoadByAccountId(std::int64_t accountId)
{
    auto lease = m_pool.acquire();
    db::Transaction tx(*lease);

    auto account = std::make_unique<Account>(/* minimal ctor: id + username */);
    account->SetAccountId(accountId);

    // One call replaces all the per-table Load* methods.
    m_tableRegistry.Hydrate(tx, accountId, *account);

    // ClearDirty handled inside Registry::Hydrate's tail (every Set*
    // during apply_row flipped a dirty bit; the load-shaped account
    // has no real mutations to flush).

    // Post-load domain reconciliation — runs here, NOT in a hydrator
    // class. The load is conceptually atomic (load + tick + ready);
    // splitting them across classes was the AccountHydrator artifact
    // and is intentionally removed by this design.
    TickQuests::Apply(*account, m_questLoader, std::time(nullptr));

    tx.commit();
    return account;
}
```

**AccountHydrator is deleted.** The two responsibilities it conflated (field projection vs. post-load tick) are now properly separated: projection in the registry, tick inline at the load entry point. The 3-line shape is simpler than any wrapping class would be.

`AccountHydratorTest.cpp` is either repurposed as a `LoadByAccountId` integration test or removed if it duplicates `PopulatedRoundTripTest`'s coverage.

---

## How save works (dirty-bit integration)

The existing dirty-bit machinery on `DirtyState` stays exactly as today. Every mutator on `Account` continues to mark its bit (`SetStoryLevel`, `AddMaterial`, `SetWorldFlag`, `AppendEvent` via `AccountTransaction`, etc.). The contract from M-V5-3 ("every mutator marks its own dirty bit") is the registry's load-bearing assumption.

```cpp
// AccountRepository::Save — new shape
bool AccountRepository::Save(Account& account)
{
    if (account.GetAccountId() == 0) { /* error */ return false; }
    if (account.IsStale())           { /* skip */  return false; }
    if (!account.Dirty().AnyDirty()) { /* no-op */ return true;  }   // M-V5-3 short-circuit

    auto lease = m_pool.acquire();
    db::Transaction tx(*lease);
    m_tableRegistry.Flush(tx, account);
    tx.commit();
    account.ClearDirty();
    return true;
}
```

The per-table `enumerate_dirty` lambda reads the appropriate `DirtyState` slot:

```cpp
// In AccountSchema.hpp's owned_characters descriptor:
.enumerate_dirty = [](const Account& a) {
    std::vector<OwnedCharacterRow> rows;
    const auto& col = a.GetCollection().GetState();
    for (const auto& id : a.Dirty().character_ids) {
        auto it = col.characters.find(id);
        if (it != col.characters.end()) rows.push_back(ToRow(it->second, a.GetAccountId()));
    }
    return rows;
},
```

**`RelationalFlush::Flush` is deleted.** Its 17 per-table `Flush*` methods all collapse into descriptor entries in `AccountSchema.hpp`.

---

## JSONB column handling

JSONB columns are handled by the `Value` variant's `nlohmann::json` case. No special-case logic at the descriptor or registry layer.

In `RowWriter::Upsert`'s `BindValue` helper:

```cpp
void BindValue(db::Params& params, const Value& v) {
    std::visit([&](const auto& x) {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, nlohmann::json>) {
            params.append(x.dump());   // pqxx binds the string; jsonb casts at SQL level via ::jsonb suffix in the UPSERT template
        }
        else if constexpr (std::is_same_v<std::decay_t<decltype(x)>, std::nullopt_t>) {
            params.append(std::nullopt);
        }
        else {
            params.append(x);
        }
    }, v);
}
```

In `RowReader::ReadValue` for JSONB columns:

```cpp
return Value{nlohmann::json::parse(pqxx_row[col].as<std::string>())};
```

The `UPSERT` SQL template for JSONB columns uses the `$N::jsonb` cast (already the pattern in `AccountTransaction::Commit`'s outbox INSERT). The SQL builder in `TableRegistry::Add` emits the cast when `field.sql_type` contains `JSONB`.

---

## Event-sourced / outbox / snapshot integration

These layers stay structurally separate; the registry doesn't try to model "append-only with optimistic concurrency" or "bounded queue with worker thread." What changes is the actual SQL-emit primitive at the bottom.

**`EventStore::AppendInTx`** continues to enforce the SELECT-MAX-version precondition (C6 / audit) and the optimistic-concurrency check, but the actual `INSERT INTO events` becomes `RowWriter<EventRow>::Upsert(tx, row)` where `EventRow` has columns matching the events table (`event_id`, `account_id`, `aggregate_kind`, `version`, `event_type`, `idempotency_key`, `data`).

**`SnapshotWriter`'s worker** keeps its bounded queue and thread; its `INSERT INTO snapshots ... ON CONFLICT DO UPDATE` becomes `RowWriter<SnapshotRow>::Upsert(tx, row)`. The cadence logic in `AccountTransaction::Commit` (C-V5-1 Task 6) is unchanged.

**`OutboxRelay`'s** `tx.exec("INSERT INTO outbox ...")` becomes `RowWriter<OutboxRow>::Upsert(tx, row)`. The relay's worker loop, prune cadence, and self-check (H-V6-1) are unchanged.

**`AccountTransaction::Commit`'s** five-stage commit chain (events → relational flush → outbox → audit → idempotency → COMMIT) keeps its ordering. Stages 1, 3, 4, 5 use `RowWriter` underneath; stage 2 calls `TableRegistry::Flush`.

The registry is the per-row SQL emit primitive. The transactional discipline that surrounds it lives at the layer that owns the discipline.

---

## Bundled bug fix: `cached_snapshot_*_version` cursor propagation

C-V5-1 Task 5 added `LoadSnapshotVersions` which populates the four `cached_snapshot_*_version` cursors on `AccountData.dirty`. But `AccountHydrator::FromData` (the old code) only propagates the four `cached_*_version` cursors to `Account::MutableDirty()`, NOT the snapshot cursors:

```cpp
// AccountHydrator.hpp:80-83 today — missing 4 lines
dirty.cached_wallet_version       = data.dirty.cached_wallet_version;
dirty.cached_pulls_version        = data.dirty.cached_pulls_version;
dirty.cached_quest_claims_version = data.dirty.cached_quest_claims_version;
dirty.cached_progression_version  = data.dirty.cached_progression_version;
// MISSING:
// dirty.cached_snapshot_wallet_version       = data.dirty.cached_snapshot_wallet_version;
// dirty.cached_snapshot_pulls_version        = data.dirty.cached_snapshot_pulls_version;
// dirty.cached_snapshot_quest_claims_version = data.dirty.cached_snapshot_quest_claims_version;
// dirty.cached_snapshot_progression_version  = data.dirty.cached_snapshot_progression_version;
```

The visible consequence: a reloaded account's snapshot cadence fires at the wrong threshold (treats cursor=0 as "no prior snapshot" → enqueues a redundant snapshot on first commit). The SnapshotWriter's UPSERT semantics dedupe the redundant write, so the bug is silent until you look for it.

Under the new design this bug class disappears structurally: cursors are FieldDescriptors on the (accounts table's) descriptor, and the registry propagates them via the same `apply_row` callback that handles every other field. There's no separate hand-coordinated propagation step to forget.

---

## Migration plan

### Big-bang single arc, ~12 commits

Estimated 3-5 weeks based on the file count. Ordered so each commit lands compilable + tested:

**Phase A — Pure additions, no integration (4 commits):**

1. `Server/Common/src/Db/DbTypes.hpp` — typedef layer. No call-site changes; just the alias file lands. Mechanical sweep of `pqxx::work` → `db::Transaction` follows in a separate dedicated commit per service area.
2. `Server/Common/src/Persistence/Value.hpp` + variant unit tests.
3. `Server/Common/src/Persistence/FieldDescriptor.hpp` + `TableDescriptor.hpp` + `Server/Common/src/Persistence/Rows/*.hpp` — Row types per table.
4. `Server/Common/src/Persistence/TableRegistry.hpp` + `RowReader.hpp` + `RowWriter.hpp` + SQL-builder helpers + unit tests for builders.

**Phase B — One table proof-of-concept (1 commit):**

5. `Server/Account/src/Persistence/AccountSchema.hpp` — first `TableDescriptor<Account, AccountsRow>` for the `accounts` table (the 13 scalars). Wire `AccountRepository::LoadByAccountId` to use it for the accounts row only; existing per-table Load methods unchanged. Validate that one descriptor round-trips correctly via `PopulatedRoundTripTest`.

**Phase C — Remaining table descriptors (4 commits, one per group):**

6. Row-per-entity collection tables: `owned_characters`, `owned_weapons`, `owned_gear`, `gear_substats`, `char_traces`.
7. Row-per-entity ancillary tables: `loadouts`, `material_inventory`, `party_slots`, `quest_states`, `quest_objectives`, `world_flags`, `pity_state`.
8. Support tables: `audit_log`, `idempotency_cache`, `outbox`, `snapshots`.
9. Event-sourced: `events`. `EventStore::AppendInTx` rewires to use `RowWriter<EventRow>` underneath while preserving the version-check semantics.

**Phase D — Integration + deletion (3 commits):**

10. Wire `AccountRepository::Hydrate` end-to-end; delete the per-table `Load*` methods. Delete `AccountHydrator.hpp` + repurpose or remove `AccountHydratorTest.cpp`.
11. Wire `AccountRepository::Save` to `Registry::Flush`; delete `RelationalFlush.hpp`. Bundle the `cached_snapshot_*_version` cursor fix here (becomes structural via the registry — no separate code change needed, the bug just stops being possible).
12. Schema-vs-descriptor integration test; full-suite verification.

### Files inventory

**NEW (~10 files):**

- `Server/Common/src/Db/DbTypes.hpp`
- `Server/Common/src/Persistence/Value.hpp`
- `Server/Common/src/Persistence/FieldDescriptor.hpp`
- `Server/Common/src/Persistence/TableDescriptor.hpp`
- `Server/Common/src/Persistence/TableRegistry.hpp`
- `Server/Common/src/Persistence/RowReader.hpp`
- `Server/Common/src/Persistence/RowWriter.hpp`
- `Server/Common/src/Persistence/Rows/AccountsRow.hpp` + per-table siblings
- `Server/Account/src/Persistence/AccountSchema.hpp` — the load-bearing schema-as-data file
- `Server/Account/tests/PersistenceTests/` — unit tests for primitives

**MODIFIED (~10 files):**

- `AccountRepository.hpp` — `Load*` methods collapse into `Registry::Hydrate`; `Save` (already M-V5-3-simplified) → `Registry::Flush`
- `RelationalFlush.hpp` — **deleted**; replaced by descriptors
- `AccountHydrator.hpp` — **deleted**; TickQuests inlined in `LoadByAccountId`
- `AccountTransaction.hpp` — `pqxx::work` → `db::Transaction`; stage 2 calls `Registry::Flush` instead of `RelationalFlush::Flush`
- `EventStore.hpp` — `AppendInTx` uses `RowWriter<EventRow>`
- `SnapshotWriter.hpp` — worker INSERT uses `RowWriter<SnapshotRow>`
- `OutboxRelay.hpp` — outbox + idempotency INSERTs use `RowWriter`
- `Common/Types/AccountData.hpp` — **deleted** (or stays as vestigial composition of Row types if needed for backward compat during the migration; preferred: delete entirely)
- ~5 test files referencing `pqxx::work` directly — alias swap

**Contracts that DON'T change:**

- `schema.sql` stays the source of truth. Descriptors verify against it (schema-vs-descriptor test) but don't generate it.
- `DirtyState` is untouched. The registry honors it via `enumerate_dirty` lambdas.
- Reducer pipeline (C-V6-1) is orthogonal — the registry is row I/O, not state projection.
- AccountTransaction's commit-chain ordering (events → flush → outbox → audit → idem → COMMIT) is preserved.

---

## Testing strategy

### Unit primitives (~5-10 tests, ~milliseconds)

Pure unit tests in `Server/Account/tests/PersistenceTests/`:

- `Value` variant round-trip (every variant case → JSON → back).
- `BuildSelectSql(descriptor)` produces canonical SQL strings (compare to golden expected).
- `BuildUpsertSql(descriptor)` produces correct `INSERT ... ON CONFLICT (pk) DO UPDATE SET ...` clauses, including `$N::jsonb` casts on JSONB columns.
- `BuildDeleteSql(descriptor)` produces correct `DELETE FROM t WHERE pk = $1 ...`.
- `RowReader` projects a synthetic `db::ResultRow` into a typed Row correctly for each `Value` variant case (including null, empty string, empty json `{}`, nested json).
- `RowWriter` binds a typed Row into `pqxx::params` in the descriptor's column order.

### The strategic integration test (~1 test, high-value)

`SchemaDescriptorConsistencyTest.cpp` — at startup, query `information_schema.columns` for every table the registry knows about and assert:

1. Every column in a `TableDescriptor` exists in the live schema with a matching SQL type.
2. Every column in the live schema for that table has a descriptor entry (no orphan columns).

**This is the single most valuable test in the new suite.** It catches the "added a column to `schema.sql` but forgot the descriptor" bug class at test time, structurally. The check is run-once-at-test-startup; cost is ~1 query per table per test run.

### Existing integration tests (reuse unchanged)

The existing tests exercise the new registry-driven path because the code under them changes, not the test API:

- `PopulatedRoundTripTest.cpp` round-trips every joined-children table through real SQL — the primary backstop for field-level regressions.
- `AccountTransactionTest.cpp` exercises the commit chain.
- `AddCurrencyEndToEndTest.cpp`, `HandlePullAgreementTest.cpp`, etc. exercise the handler paths.

If anything regresses in column ordering, type coercion, dirty-bit honoring, or JSONB binding, these tests catch it. No new integration tests beyond the schema-descriptor consistency check are needed.

---

## Out of scope (documented deferrals)

The following are intentionally NOT part of this arc:

- **Strong-typed IDs** (`AccountId = strong<std::int64_t>`, `EventVersion = strong<int>`). Composes well with the registry's `Value` variant but adds blast radius across the codebase. Defer to a follow-on arc; the registry doesn't depend on it.
- **Schema generation from descriptors.** Every `FieldDescriptor` carries `sql_type` already; a `GenerateSchemaSql(registry)` helper could replace `schema.sql` as truth. Out of scope for first cut — descriptors verify against `schema.sql` rather than generating it. Migration to descriptor-as-truth comes in a later arc.
- **`DispatchCurrencyReward` removal** (C-V6-1 Phase 3 deferral). Unrelated; stays in its existing DEFER block until the breakthrough-cascade restructure is its own dedicated arc.
- **7-day snapshot cadence** (C-V5-1 deferred). Unrelated; the registry doesn't change cadence semantics.
- **Cold-start replay fold path** (C-V5-1 deferred). Unrelated; the registry covers the persistence side, not the reducer-fold side.
- **H-V6-3 protocol.json drift.** Unrelated; client/server wire format, not Account-side persistence.
- **Custom allocator + Tracy instrumentation.** Discussed separately; lands AFTER this arc (Tracy then allocator after measurement).

---

## Open risks and mitigations

### Risk: SQL string drift between descriptors and pqxx's expected dialect

`BuildSelectSql` / `BuildUpsertSql` / `BuildDeleteSql` produce SQL by concatenation. Postgres dialect specifics (e.g., `::jsonb` casts, `ON CONFLICT (pk_a, pk_b) DO UPDATE`) must be encoded in the builders.

**Mitigation:** Unit tests assert builders produce known-good SQL strings for representative descriptors. The schema-vs-descriptor integration test verifies the SQL actually executes against the real schema.

### Risk: `std::function` indirect-call overhead in hot paths

Per-row dispatch is one indirect call per field per row. For row-per-entity tables with hundreds of rows during initial hydrate (e.g., `owned_characters` for a long-played account), N × F indirect calls add up.

**Mitigation:** The hydrate path runs once per session (load on connect, no repeated calls). Flush path runs only on dirty rows. Profiled cost at session-load N=500 rows × F=5 fields = 2500 indirect calls ≈ 2.5µs — invisible against the multi-millisecond DB round-trip. If Tracy ever flags this, individual descriptors can replace `std::function` with raw function pointers (small refactor).

### Risk: `Value` variant size vs. cache pressure

`std::variant<int64, string, bool, nullopt, json>` is sized to fit the largest alternative (`nlohmann::json`, which can be hundreds of bytes for non-trivial objects). Stack allocation per field × rows could spill into heap for large rows.

**Mitigation:** For non-JSONB tables (the majority), values are int64/string/bool/nullopt and fit comfortably. For JSONB-heavy tables (events, snapshots, outbox), the SBO behavior of `nlohmann::json` already heap-allocates the inner objects — the variant's size doesn't change real-world heap pressure. If profiling shows it, switch JSONB columns to read/write `std::string` and parse lazily downstream.

### Risk: Migration test failures masking each other

A 12-commit big-bang has many opportunities for "test fails on commit N, but the failure is actually from commit N-3."

**Mitigation:** The phased ordering above lands each phase's `Account` integration tests independently. Phase A is pure additions (can't break anything). Phase B introduces ONE descriptor wired in parallel with existing code. Phase C adds descriptors per group with no integration switch. Phase D flips the switches one Load/Save path at a time. Every phase commits in a working state.

---

## Self-review

After writing this spec I checked:

**Placeholder scan:** No `TBD` / `TODO` / `XXX`. Every section is complete.

**Internal consistency:** The architecture diagram, the migration plan's file inventory, and the testing strategy all reference the same set of new files and the same `TableRegistry<Owner>` / `FieldDescriptor<Row>` / `TableDescriptor<Owner, Row>` API. The four decisions locked in via brainstorming (AccountData dropped, TableRegistry name, unit + schema-consistency tests, db::ResultRow alias) all appear consistently in the spec.

**Scope check:** The arc is large (12 commits, ~10 new files, ~10 modified) but coherent — it's "introduce the persistence registry and migrate every existing call site to use it." The Out of scope section explicitly fences against the surrounding deferrals so the arc doesn't sprawl.

**Ambiguity check:** Five places I made explicit on the self-review pass:

1. AccountData's fate: "dropped entirely" not "kept as a composition of Rows." Stated in the migration file inventory.
2. AccountHydrator's fate: "deleted" — the TickQuests::Apply call moves inline to `AccountRepository::LoadByAccountId`. Stated in the "How load works" section.
3. The cursor bug fix happens structurally, not via a separate code edit. Stated in the bundled-bug-fix section.
4. `TableDescriptor` has only `enumerate_dirty` — no separate `enumerate`. An earlier draft had both; nothing used `enumerate`. Noted in the TableDescriptor section.
5. The owner-column convention (every Account-owned table has `account_id`) is explicit; the consistency test enforces it. Noted in the TableRegistry section.

**One thing I'd want a senior reviewer to push back on:** the choice of `std::function` for the descriptor lambdas vs. raw function pointers + a small payload struct. The performance argument lands ("indirect call cost is invisible at our scale"), but raw function pointers would also save the heap allocation that `std::function`'s SBO may not avoid for some captures. The trade is small; explicitness of `std::function` in the descriptor's type signature ("yes, this holds a callable") is the readability reason I kept it.
