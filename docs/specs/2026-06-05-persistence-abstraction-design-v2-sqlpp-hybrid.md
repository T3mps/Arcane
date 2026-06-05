# Persistence Abstraction Design (v2 — sqlpp23 Hybrid)

**Date:** 2026-06-05
**Status:** Design approved; ready for spike → implementation planning
**Supersedes:** `2026-06-05-persistence-abstraction-design.md` (v1 hand-rolled approach)
**Scope:** All 17 tables, all persistence shapes (scalars + row-per-entity + JSONB + event-sourced), big-bang migration
**Prerequisites:** C++20 → C++23 language-standard bump
**Bundles:** The `cached_snapshot_*_version` cursor-propagation bug from C-V5-1 Task 5

---

## Goal

Same goal as v1: collapse the 6-touch-point fragility of adding a persisted field to 2 (schema + descriptor), eliminate positional-index bugs, give the codebase a self-documenting layer over the database. Achieved here via the **Repository + Data Mapper** pattern (Fowler PEAA): `AccountRepository` is the Repository façade, our `TableRegistry<Owner>` + `TableDescriptor` is the Data Mapper, **sqlpp23** is the typed row-IO primitive underneath.

The hand-rolled design (v1) is superseded because sqlpp23 gives us **compile-time SQL safety** as a category-of-bug elimination — column typos, type mismatches, schema-vs-code drift, and the `r[0][7].as<int>()` positional-indexing class all become compile errors rather than test failures. The orchestration layer we designed in v1 (TableRegistry, TableDescriptor, the Owner+DirtyState discipline) survives unchanged; only the row-IO primitive at the bottom of each descriptor changes.

---

## What changed from v1

| Aspect | v1 (hand-rolled) | v2 (sqlpp23 hybrid) |
|---|---|---|
| Row-IO primitive | `RowReader<Row>` / `RowWriter<Row>` building SQL strings | sqlpp23 typed queries |
| Column reference | `row["name"].as<int>()` | `row.story_level` (typed member) |
| SQL safety | Runtime; verified by schema-vs-descriptor consistency test | Compile-time; column typos and type mismatches are compile errors |
| `Value` variant | Yes — runtime variant carrying all column types | No — sqlpp23's typed columns make it redundant |
| SQL string builders | `BuildSelectSql` / `BuildUpsertSql` / `BuildDeleteSql` | None — sqlpp23 builds SQL at compile time |
| DB driver | libpqxx (with libpq transitively) | libpq directly + sqlpp23-connector-postgresql |
| Language standard | C++20 (current) | C++23 (one-line `cppdialect` bump) |
| JSONB column handling | `Value` variant case + `nlohmann::json` round-trip | Custom sqlpp23 type trait (see "JSONB type trait" section) |
| Orchestration (TableRegistry, TableDescriptor, enumerate_dirty, apply_row) | Designed in v1 | **Unchanged from v1** |
| AccountHydrator deletion + TickQuests inlining | Designed in v1 | **Unchanged from v1** |
| AccountData removal | Designed in v1 | **Unchanged from v1** |
| `cached_snapshot_*_version` cursor bug fix | Structural via registry | **Unchanged from v1 — still structural via registry** |
| Deferrals (strong-typed IDs, schema gen, etc.) | Listed in v1 | **Unchanged from v1** |

Read v1's "Data Mapper pattern" section, "Out of scope" section, and "Migration plan" file inventory as the still-load-bearing portions. This document covers only the deltas.

---

## Prerequisite: C++20 → C++23 language bump

One-line change per project in `Server/premake5.lua`:

```lua
-- Was: cppdialect "C++20"
cppdialect "C++23"
```

MSVC in VS2026 (this codebase's compiler) implements C++23. The bump unlocks more than just sqlpp23 enablement — `std::expected` monadic ops, `if consteval`, deducing `this`, `std::print`, multidimensional subscript, `auto(x)` decay copy, and ranges-to are all available. No existing code breaks (C++23 is a strict superset of C++20 for our usage).

---

## Architecture (revised)

```
Handler                                  ← writes plain C++; doesn't see sqlpp23
    ↓
AccountRepository (Repository facade)    ← Owner-level CRUD API
    ↓
TableRegistry<Account> (Data Mapper)     ← Owner+DirtyState orchestration
    ↓
TableDescriptor<Account, Row>            ← per-table metadata + IO lambdas
    ↓ (the IO lambdas wrap sqlpp23 typed queries)
sqlpp23 typed queries + connector        ← compile-time SQL safety lives here
    ↓
libpq + sqlpp23-connector-postgresql     ← wire-level Postgres communication
```

Every layer has one clear job. sqlpp23 syntax is confined to the `select_by_owner` / `upsert` lambdas inside each `TableDescriptor`. Handlers, the Repository, and the TableRegistry never see sqlpp23.

---

## Dependency changes

**Removed:** libpqxx. Every `#include <pqxx/pqxx>` and every `pqxx::*` type goes away.

**Added (vendored, not vcpkg):** sqlpp23 + sqlpp23-connector-postgresql, dropped into `ThirdParty/sqlpp23/`. Consistent with CLAUDE.md's vendoring preference — sqlpp23 is not in vcpkg as of mid-2026 (open port request, unknown timeline) and vendoring gives us a stable, controlled build via premake5. The core is header-only; the connector compiles as a small static lib through our existing premake flow.

**Stays:** libpq (was a transitive dependency of libpqxx; becomes a direct dependency of sqlpp23-connector-postgresql via the same vcpkg-triplet path).

**Updated `vcpkg-triplets/x64-windows-static.cmake`:** libpqxx removed from the install set. libpq stays.

---

## JSONB type trait — design with optimization and dev experience as first-class concerns

**The dev experience requirement:** a descriptor entry for a JSONB column must read identically to a scalar column. A dev declaring a new JSONB column should not have to understand the trait machinery.

**The optimization requirement:** the JSONB read/write path must avoid unnecessary string copies. Postgres returns JSONB as text; we parse with `nlohmann::json`. The bind path serializes `nlohmann::json` to text, then Postgres casts to JSONB. We control both sides.

### The trait, layered for clarity

Three small, single-purpose files. Each has one job.

> **Code samples below are illustrative, not literal.** sqlpp23's extension API (`value_type_of`, `bind_traits`, the connector's bind/extract entry points) uses symbol names that may differ from what I've sketched here. The Phase A spike confirms the actual API shapes by getting the trait to compile and round-trip against the live DB; the *structure* (tag type → library trait specialization → connector specialization → descriptor consumes it transparently) is what the spec commits to, not the exact symbol names.

```cpp
// ThirdParty/aphelyon-sql-types/include/aphelyon/sql/types/jsonb.hpp
//
// Aphelyon JSONB column type for sqlpp23.
//
// Represents a PostgreSQL JSONB column. Round-trips through
// nlohmann::json on the C++ side, via Postgres's TEXT bind protocol
// with `$N::jsonb` cast on the SQL side. From the descriptor author's
// perspective: declaring a JSONB column is identical to declaring a
// TEXT or INT column — sqlpp23 dispatches the trait automatically.
//
// Design notes:
//   - Read path uses std::string_view to avoid copying the connector's
//     internal text buffer before nlohmann::json::parse consumes it.
//     nlohmann supports parse(string_view) natively; the parse itself
//     allocates the json tree, but the source string is borrowed.
//   - Write path constructs the dump string once via j.dump() and
//     moves it into the bind buffer. No defensive copy.
//   - The trait is symmetric: ToJsonb(const nlohmann::json&) and
//     FromJsonb(std::string_view) are the only two functions the
//     dev-facing API surfaces. Both are inline; the compiler inlines
//     them through the sqlpp23 dispatch.

#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace aphelyon::sql::types {

// Tag type used as the sqlpp23 column-data-type marker. Empty struct;
// purely a type-system handle. The actual marshalling lives in the
// trait specializations below.
struct jsonb {};

// Public dev-facing helpers. Descriptor authors NEVER call these
// directly — sqlpp23's binding machinery does. Exposed here so the
// few places that DO need explicit conversion (e.g. the upfront spike
// or a debug query) have a documented entry point.
[[nodiscard]] inline std::string ToJsonb(const nlohmann::json& j) {
    // Single allocation. nlohmann::json::dump appends to its internal
    // string buffer and returns by value (RVO).
    return j.dump();
}

[[nodiscard]] inline nlohmann::json FromJsonb(std::string_view sv) {
    // Single allocation for the json tree; the source string_view
    // is borrowed (no copy of the wire bytes).
    return nlohmann::json::parse(sv);
}

} // namespace aphelyon::sql::types
```

```cpp
// ThirdParty/aphelyon-sql-types/include/aphelyon/sql/types/jsonb_trait.hpp
//
// sqlpp23 trait specializations that teach the library how to bind
// and extract our jsonb column type. This file is the "boring
// adapter" — it has no dev-facing API beyond the two specializations.
// Descriptor authors include the higher-level jsonb.hpp header and
// never touch this one directly.

#pragma once
#include "aphelyon/sql/types/jsonb.hpp"
#include <sqlpp23/sqlpp23.h>

namespace sqlpp {

// value_type_of: tells sqlpp23 what C++ type a column of our jsonb
// data type yields in a result row. Reading row.some_jsonb_col
// returns a nlohmann::json.
template <>
struct value_type_of<aphelyon::sql::types::jsonb> {
    using type = nlohmann::json;
};

// bind/extract specialization for the postgresql connector.
// Connector-specific because the bind protocol differs across
// connectors (e.g. SQLite would use a different specialization).
// Located here so the JSONB story lives in one file; if we ever
// add a second connector, the new specialization sits next to this
// one in a sibling header.

namespace postgresql {

template <>
struct bind_traits<aphelyon::sql::types::jsonb> {
    static void bind_param(prepared_statement_t& stmt, std::size_t index, const nlohmann::json& value) {
        // Marshal to string once; sqlpp23-connector-postgresql binds the
        // resulting std::string via libpq's text protocol. Postgres
        // performs the JSONB cast server-side via the `$N::jsonb` token
        // that the query builder emits for jsonb columns.
        stmt.bind_text(index, aphelyon::sql::types::ToJsonb(value));
    }

    static nlohmann::json extract(const result_row_t& row, std::size_t index) {
        // Borrow the connector's text buffer for the duration of the
        // parse. nlohmann::json::parse copies what it needs into its
        // own tree; the source string_view does not outlive this call.
        return aphelyon::sql::types::FromJsonb(row.get_text_view(index));
    }
};

} // namespace postgresql
} // namespace sqlpp
```

```cpp
// Server/Account/src/Persistence/AccountSchema.hpp (excerpt)
//
// A descriptor entry for a JSONB column reads exactly like any other
// column. The dev declaring it doesn't see the trait machinery.

inline const TableDescriptor<Account, EventsRow> kEventsTable = {
    .table_name = "events",
    .pk_columns = {"account_id", "version"},
    .doc        = "Append-only event log; one row per emitted event.",

    .apply_row = [](Account& a, EventsRow row) {
        // Events aren't projected back into Account directly;
        // they're consumed via the event-sourcing layer.
    },

    .enumerate_dirty = [](const Account&) {
        // Events are appended via AccountTransaction, not via Save.
        return std::vector<EventsRow>{};
    },

    // sqlpp23 syntax confined to the IO lambdas
    .select_by_owner = [](db::Transaction& tx, std::int64_t owner_id) {
        const auto t = sql::Events{};
        return tx(select(all_of(t)).from(t).where(t.account_id == owner_id));
    },
    .upsert = [](db::Transaction& tx, const EventsRow& row) {
        // Events use append-only INSERT; on conflict on PK is an error
        // (the EventStore's optimistic-concurrency check should have
        // prevented duplicates).
        const auto t = sql::Events{};
        tx(insert_into(t).set(
            t.event_id        = row.event_id,
            t.account_id      = row.account_id,
            t.aggregate_kind  = row.aggregate_kind,
            t.version         = row.version,
            t.event_type      = row.event_type,
            t.idempotency_key = row.idempotency_key,
            t.data            = row.data         // <-- nlohmann::json bound transparently via the trait
        ));
    },

    .fields = {
        { .column = "event_id",        .doc = "UUID v7; sortable by time." },
        { .column = "data",            .doc = "Event payload; shape depends on event_type." },
        // ... no special-case markup for the jsonb column; the trait
        // dispatches on the table class's typed-column declaration.
    },
};
```

### Why this layout

**Three files, one responsibility each:**

- `jsonb.hpp` — the tag type + two public helpers. ~30 lines. The "what is JSONB to Aphelyon" definition.
- `jsonb_trait.hpp` — sqlpp23 + connector specializations. ~40 lines. The "how does sqlpp23 talk to JSONB" adapter.
- `AccountSchema.hpp` — descriptor entries. JSONB columns declared the same way as scalars.

**Optimization considerations baked in:**

1. **No defensive copies on read.** `extract` borrows the connector's text buffer via `string_view`; `nlohmann::json::parse(string_view)` consumes it without an intermediate copy.
2. **Single allocation on write.** `j.dump()` produces one `std::string`; it moves into the bind buffer; no second copy on the way to libpq.
3. **Inlined dispatch.** `ToJsonb`/`FromJsonb` are inline functions; the compiler inlines them through the sqlpp23 trait machinery. The runtime cost is effectively the `nlohmann::json` parse/dump itself, which is the irreducible cost of JSONB.
4. **No `std::function` indirection for JSONB columns specifically** (a regression risk noted in v1's `Value` variant). The bind path is a direct call through sqlpp23's compile-time-resolved trait machinery.

**Forward-compatibility:**

If sqlpp23 ever ships first-class JSONB support, we delete `jsonb_trait.hpp`, change `jsonb` to alias the library type in `jsonb.hpp`, and every descriptor stays unchanged. The migration cost when that day comes: ~10 minutes.

---

## Upfront spike (~2-3 days)

Before committing to the full migration, validate the foundational assumptions on a small isolated branch:

1. **Vendor sqlpp23 + sqlpp23-connector-postgresql** into `ThirdParty/sqlpp23/` with a premake5 build that produces a connector static lib alongside the existing libs.
2. **C++23 dialect bump** in `Server/premake5.lua` — verify a clean full build of every existing service (Auth, Account, Combat, Tests).
3. **One trivial typed query end-to-end:** `SELECT account_id, username FROM accounts WHERE account_id = $1` via sqlpp23, against the live dev DB.
4. **Transaction integration:** open a `sqlpp::postgresql::transaction_t`, run two queries, commit, verify state.
5. **JSONB type trait round-trip:** write a non-trivial nested `nlohmann::json` value into `events.data` via the trait, read it back, assert equality. **This is the highest-risk surface — if it doesn't work cleanly, the entire pivot is reconsidered.**

**Exit criteria:** all five items pass with code we'd actually keep. If the JSONB trait can't be made clean in 2-3 days, fall back to sqlpp11 (same situation, more online examples to draw from). If neither works cleanly, the v1 hand-rolled spec resurrects.

The spike artifacts (vendoring layout, premake fragment, trait headers) carry forward into Phase A of the implementation. Spike work is not throwaway.

---

## Migration plan (revised)

Big-bang single arc, ~13-14 commits (Phase A=3 + B=3 + C=4 + D=3-4). Adjusted from v1 to reflect:

- Adding the spike + dialect bump as Phase A
- Removing the SQL-builder helpers (don't need them; sqlpp23 builds SQL)
- Removing the `Value` variant (don't need it; sqlpp23 has typed columns)
- Adding the libpqxx → libpq + sqlpp23-connector swap

**Phase A — Foundation (3 commits):**

1. C++20 → C++23 dialect bump in premake5; full build verification across all services.
2. Vendor sqlpp23 + sqlpp23-connector-postgresql under `ThirdParty/`; premake5 build for the connector.
3. JSONB type trait header + spike test (round-trip a nested json through the trait against the live DB).

**Phase B — Pure additions, no integration (3 commits):**

4. `Server/Common/src/Db/DbTypes.hpp` — typedef layer (`db::Transaction = sqlpp::postgresql::transaction_t`, etc).
5. `Server/Common/src/Persistence/{FieldDescriptor,TableDescriptor,TableRegistry}.hpp` — orchestration primitives (unchanged from v1 in shape).
6. `Server/Common/src/Persistence/Rows/*.hpp` — per-table Row types.

**Phase C — Per-table descriptors with sqlpp23-generated table classes (4 commits):**

7. Generated table classes for all 17 tables (via sqlpp11gen-equivalent or manual headers from `schema.sql`); committed under `Server/Account/src/Persistence/sql/`.
8. `Server/Account/src/Persistence/AccountSchema.hpp` — TableDescriptor literals for the accounts row + collection tables (characters/weapons/gear/loadouts/party/pity/quests/materials/world_flags).
9. Support-table descriptors (audit_log, idempotency_cache, outbox, snapshots).
10. Events table descriptor; EventStore::AppendInTx rewired to use the typed sqlpp23 INSERT through the descriptor's `upsert` lambda.

**Phase D — Cutover (3-4 commits):**

11. Wire `AccountRepository::LoadByAccountId` to use `Registry::Hydrate`; delete the per-table `Load*` methods. Delete `AccountHydrator.hpp` (TickQuests inlined). Delete `Common/Types/AccountData.hpp` (Row types replace it).
12. Wire `AccountRepository::Save` and `AccountTransaction::Commit`'s relational-flush stage to `Registry::Flush`; delete `RelationalFlush.hpp`. Bundle the `cached_snapshot_*_version` cursor fix (structural via the registry).
13. Wire `SnapshotWriter` + `OutboxRelay` + idempotency UPSERT through the registry — each goes through the relevant descriptor's `upsert` lambda (typed sqlpp23 INSERT ... ON CONFLICT), matching the events table cutover from commit 10.
14. libpqxx removal sweep + schema-vs-descriptor consistency test (now asserting against `information_schema.columns` AND against sqlpp23's generated table classes).

---

## Testing strategy

Unchanged from v1 conceptually:

1. **Unit primitives (~5-10 tests):** sqlpp23 round-trip per Row type (write a Row → INSERT → SELECT → reconstruct → compare); JSONB trait round-trip with edge cases (empty object, nested arrays, unicode keys, large payloads); per-table SELECT/UPSERT smoke tests in a fresh test DB.
2. **Strategic integration test:** schema-vs-descriptor consistency — `information_schema.columns` agrees with sqlpp23's generated table classes AND each TableDescriptor's metadata. Catches schema drift at test time, structurally.
3. **Existing integration tests:** reused unchanged. `PopulatedRoundTripTest`, `AccountTransactionTest`, `AddCurrencyEndToEndTest`, all the agreement tests — they exercise the new registry-driven path because the code under them changes, not the test API.

The compile-time safety reduces the value of some integration tests (column typos won't survive `cmake build`), but the existing tests still catch behavior regressions (dirty-bit honoring, transaction semantics, idempotency cache hits, etc.) — keep them.

---

## Open risks (new under v2)

### JSONB trait edge cases

The trait covers `nlohmann::json` round-trip. Edge cases worth pinning in tests:

- Empty object `{}` and empty array `[]`
- Nested objects with non-ASCII keys (UTF-8)
- Large payloads (kilobytes — events.data fits this profile)
- SQL injection sanity: a json value containing literal `'); DROP TABLE accounts; --` must survive the round-trip without affecting query parsing (sqlpp23's parameterized binding handles this, but worth a regression test)

### sqlpp23 maturity

sqlpp23 has fewer GitHub stars / less battle-testing than sqlpp11. The migration target is the right one strategically (Roland Bock's active development), but obscure bugs may land us reporting upstream. Mitigation: the spike validates the foundational surface; if the spike trips on something fundamental, fall back to sqlpp11.

### Library vendoring discipline

sqlpp23 + connector live under `ThirdParty/`. We pin to a specific git tag/SHA. If sqlpp23 makes API-breaking changes between vendored versions, the upgrade cost is real but bounded (the descriptors' IO lambdas are the only sqlpp23-syntax sites; the rest of the codebase doesn't see sqlpp23 directly).

### libpq direct dependency

We've used libpq transitively (via libpqxx) but never directly. The sqlpp23-connector-postgresql build needs libpq headers + lib at the right paths. vcpkg already installs libpq for libpqxx; after libpqxx is removed, libpq stays via an explicit vcpkg dep declaration.

---

## What's deferred (carried forward from v1)

Unchanged from v1's "Out of scope" section:

- Strong-typed IDs (`AccountId`, `EventVersion`, etc.)
- Schema generation from descriptors (descriptors verify against `schema.sql`; not yet the source of truth)
- DispatchCurrencyReward removal (C-V6-1 Phase 3 deferral)
- 7-day snapshot cadence + cold-start replay fold (C-V5-1 deferrals)
- H-V6-3 protocol.json drift
- Custom allocator + Tracy (lands AFTER this arc)

---

## Self-review

After writing this v2 spec I checked:

**Placeholder scan:** No TBD / TODO. Every section is complete.

**Internal consistency:** The architecture diagram, the migration plan's commits, the testing strategy, and the dependency-change table all reference the same set of libraries (sqlpp23 + sqlpp23-connector-postgresql + libpq + nlohmann::json + spdlog + xoshiro + Catch2 + rapidcheck). libpqxx is referenced only as something being removed — it appears in the dependency-change table's "before" column, the "Removed" prose line, the `vcpkg-triplets` bullet that drops it from the install set, the open-risks "libpq direct dependency" entry (libpq stays after libpqxx leaves), and Phase D's commit-14 removal sweep. Every mention is part of the same removal arc; there is no path where libpqxx survives the migration. The architecture diagram shows sqlpp23 between TableDescriptor and the connector — matches every other section's wording.

**Scope check:** Arc is large (13-14 commits, vendored library, language-standard bump, every SQL call site touched) but bounded — fits a 5-7 week scope per the earlier estimate. Phase A's 3-day spike is the gate that confirms the foundational assumptions before the big-bang migration begins.

**Ambiguity check:** Three places I made explicit on this pass:

1. **The JSONB trait's three-file layout** is justified by single-responsibility separation, not just "more files." `jsonb.hpp` is the type definition; `jsonb_trait.hpp` is the connector adapter; `AccountSchema.hpp` consumes both without seeing either's internals.
2. **The spike's exit criteria** specifically include JSONB round-trip working cleanly. If the JSONB trait can't be made clean, the pivot to sqlpp23 is reconsidered. This is the only foundational risk where "fall back to a different choice" is the documented response.
3. **C++23 bump rationale is independent of sqlpp23.** Even if sqlpp23 turned out unworkable and we fell back to sqlpp11, C++23 stays — `std::expected` monadic ops, `std::print`, deducing `this`, multidimensional subscript are wins in their own right.

**Research-pass result:** The hybrid landed in well-trodden territory — Repository (façade) + Data Mapper (TableRegistry) + typed-SQL primitive (sqlpp23) is the canonical Fowler PEAA composition. The "what to do about JSONB" question is the one place we add original work (the type trait), and the design is bounded to ~70 lines across two small files; the dev-facing surface is "declare a JSONB column like any other column."

**One thing I'd want a senior reviewer to push back on:** the choice to vendor sqlpp23 rather than wait for the vcpkg port. Vendoring couples us to a manual upgrade discipline (we pin a SHA, we have to bump it deliberately, security advisories on the underlying libpq don't auto-flow through). The counter: vcpkg's port timeline is unknown, and the codebase already vendors several deps (spdlog, nlohmann, xoshiro, premake5 itself) — the operational discipline for vendored deps is established. If the vcpkg port lands during Phase A, we switch to vcpkg without rewriting any descriptor code.
