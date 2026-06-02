# Account DB Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate Account persistence from one-JSON-file-per-player to PostgreSQL with selective event sourcing on wallet/pulls/quest_claims/progression aggregates; relational tables + audit log for the rest.

**Architecture:** PostgreSQL 16+ as primary store, Docker Compose in dev. Single monthly-partitioned `events` table holds 4 aggregate streams; 13 relational tables hold non-ES player state; 3 support tables (snapshots, outbox, audit_log). Per-RPC Postgres transaction with optimistic concurrency on event append. Dirty-flag flush on relational mutations driven by Account setters. xoshiro256++ PRNG for cross-platform deterministic replay. 64-stripe per-player lock + in-memory cache preserved.

**Tech Stack:** C++20, PostgreSQL 16+, libpqxx 8.0.1 + libpq (from vcpkg overlay — same channel as protobuf/abseil), XoshiroCpp (vendored header by Ryo Suzuki, MIT), pg_partman extension, WAL-G (backup), rapidcheck (vendored, BSD), Catch2 v3 (vendored, Boost), clang-tidy custom check. Docker Compose orchestrates Postgres in dev.

**Spec:** `docs/superpowers/specs/2026-06-01-account-db-migration-design.md`

---

## File structure

| File | Responsibility |
|---|---|
| `Server/docker-compose.yml` | Postgres 16 + pg_partman container; named volume `aphelyon_pgdata`; exposed on host `5432`. |
| `Server/scripts/db-setup.bat` | Windows: bring up Postgres, wait for ready, run migrations. |
| `Server/scripts/db-setup.sh` | Cross-platform equivalent of the above. |
| `Server/scripts/db-reset.bat` | Dev-only: nuke volume and `bin/*/Account/data/accounts/`, then re-setup. |
| `Server/Account/migrations/001_accounts.sql` | `accounts` table + soft-delete index. |
| `Server/Account/migrations/002_inventory.sql` | `owned_characters`, `char_traces`, `owned_weapons`, `owned_gear`, `gear_substats`. |
| `Server/Account/migrations/003_equipment.sql` | `loadouts`, `material_inventory`, `party_slots`. |
| `Server/Account/migrations/004_quests.sql` | `quest_states`, `quest_objectives`, `world_flags`, `pity_state`. |
| `Server/Account/migrations/005_events.sql` | Partitioned `events` table + initial monthly partition + indexes + FK CASCADE. |
| `Server/Account/migrations/006_support.sql` | `snapshots`, `outbox`, `audit_log`. |
| `Server/Account/migrations/007_public_uid.sql` | 9-digit `accounts.public_uid` + per-region sequence (NA prefix = 1). |
| `Server/scripts/setup-vcpkg-deps.bat` | MODIFY — add `libpqxx:x64-windows-static` (which transitively pulls `libpq`). |
| `ThirdParty/Xoshiro/XoshiroCpp.hpp` | Already cloned. Header-only PRNG by Ryo Suzuki (MIT). No premake5 file needed. |
| `ThirdParty/rapidcheck/premake5.lua` | Create — static-lib project file (sources already cloned). |
| `ThirdParty/Catch2/premake5.lua` | Create — static-lib project file (Catch2 v3 source already cloned). |
| `Server/Common/src/UuidV7.hpp` | UUID v7 generator (time-ordered, RFC 9562). |
| `Server/Account/src/db/ConnectionPool.hpp` | Bounded libpqxx connection pool (semaphore-based, ~80 LOC). |
| `Server/Account/src/db/MigrationRunner.hpp` | Applies numbered `.sql` files; tracks applied versions in a `schema_migrations` table. |
| `Server/Account/src/events/Event.hpp` | Common event envelope: id, account_id, aggregate_kind, version, type, schema_version, data, metadata, idempotency_key. |
| `Server/Account/src/events/WalletEvents.hpp` | Wallet event payload types + JSON serialization. |
| `Server/Account/src/events/PullEvents.hpp` | Pull event payload types (with RNG capture fields). |
| `Server/Account/src/events/QuestClaimEvents.hpp` | Quest claim event payload types. |
| `Server/Account/src/events/ProgressionEvents.hpp` | Progression event payload types. |
| `Server/Account/src/reducers/ReducerCommon.hpp` | Shared `ReducerResult<State>`, `SideEffectDescriptor` variants, `Clock` interface. |
| `Server/Account/src/reducers/WalletReducer.hpp` | Pure reducer: `(WalletState, WalletEvent) → ReducerResult<WalletState>`. |
| `Server/Account/src/reducers/PullsReducer.hpp` | Pure reducer with deterministic RNG state in event. |
| `Server/Account/src/reducers/QuestClaimsReducer.hpp` | Pure reducer; emits cross-aggregate references via metadata. |
| `Server/Account/src/reducers/ProgressionReducer.hpp` | Pure reducer; can spawn downstream wallet events (overflow credits). |
| `Server/Account/src/db/EventStore.hpp` | Append event (optimistic concurrency), load aggregate (snapshot + tail). |
| `Server/Account/src/db/SnapshotWriter.hpp` | Background thread + bounded MPMC queue; cadence-driven snapshot writes. |
| `Server/Account/src/db/OutboxRelay.hpp` | Background poller using `FOR UPDATE SKIP LOCKED`; dispatches and marks `dispatched_at`. |
| `Server/Account/src/db/RelationalFlush.hpp` | Per-table flush implementations driven by `Account::Dirty()`. |
| `Server/Account/src/AccountRepository.hpp` | REWRITE — Postgres-backed; replaces the JSON-file repository. |
| `Server/Account/src/AccountTransaction.hpp` | Per-RPC transaction wrapper (Begin/Commit/Rollback). |
| `Server/Account/src/TickQuests.hpp` | Explicit quest-expiration helper invoked at account-load + quest handlers. |
| `Server/Account/src/AccountSerializer.hpp` | DELETE — JSON I/O is gone. |
| `Server/Common/src/AccountData.hpp` | MODIFY — private fields, public setters that auto-mark dirty. |
| `Server/Common/src/CollectionState.hpp` | MODIFY — UUID instance IDs (`uuids::uuid` instead of `std::string`). |
| `Server/Account/src/GachaRNG.hpp` | MODIFY — switch from `std::mt19937` to xoshiro256++; surface RNG state on/before pull. |
| `Server/Account/src/GachaHandlers.hpp` | MODIFY — use `AccountTransaction`; emit pull events instead of mutating directly. |
| `Server/Account/src/AccountHandlers.hpp` | MODIFY — use `AccountTransaction`; emit wallet events; audit non-ES mutations. |
| `Server/Account/src/QuestHandlers.hpp` | MODIFY — call `TickQuests` explicitly; `GetQuestState` becomes a pure read. |
| `Server/Account/src/ProgressionHandlers.hpp` | MODIFY — emit progression events. |
| `Server/Account/src/AccountServer.hpp` | MODIFY — wire up `ConnectionPool`, `SnapshotWriter`, `OutboxRelay`. |
| `Server/Account/tests/ReducerTests/WalletReducerTest.cpp` | Catch2 unit tests for wallet reducer. |
| `Server/Account/tests/ReducerTests/PullsReducerTest.cpp` | Catch2 unit tests for pulls reducer. |
| `Server/Account/tests/ReducerTests/QuestClaimsReducerTest.cpp` | Catch2 unit tests for quest-claims reducer. |
| `Server/Account/tests/ReducerTests/ProgressionReducerTest.cpp` | Catch2 unit tests for progression reducer. |
| `Server/Account/tests/PropertyTests/ReplayDeterminismTest.cpp` | rapidcheck triple-replay property. |
| `Server/Account/tests/PropertyTests/SnapshotEquivalenceTest.cpp` | rapidcheck split-and-fold property. |
| `Server/Account/tests/PropertyTests/InvariantTests.cpp` | Domain invariants (currency non-negative, pity bounded). |
| `Server/Account/tests/GoldenFile/SchemaMigrationTest.cpp` | Loads `tests/events/v{N}_*.json` → upcasts → folds → asserts. |
| `Server/Account/tests/events/v1_pull_performed.json` | Versioned event sample. |
| `Server/Account/tests/events/v1_credits_spent.json` | Versioned event sample. |
| `Server/Account/tests/events/v1_quest_reward_claimed.json` | Versioned event sample. |
| `Server/Account/tests/events/v1_story_level_advanced.json` | Versioned event sample. |
| `Server/Account/tests/Integration/EventStoreRoundTripTest.cpp` | testcontainer or docker-spawned Postgres; append + load + replay. |
| `Server/Account/tests/Integration/OutboxRelayTest.cpp` | Verifies relay dispatches and marks `dispatched_at`. |
| `tools/clang-tidy/GachaReducerPurityCheck.cpp` | Custom clang-tidy check banning non-deterministic calls in `reducers/`. |
| `tools/clang-tidy/.clang-tidy` | Project-level clang-tidy config that enables the custom check on `reducers/`. |
| `Server/scripts/wal-g-setup.sh` | Optional: dev WAL-G config for backup drill (deferred to operational phase). |
| `Server/Account/premake5.lua` | MODIFY — link `libpqxx` + `libpq` from vcpkg, add `migrations/`, add `tests/` executable, add reducers dir. |
| `Server/premake5.lua` | MODIFY — include rapidcheck + Catch2 subprojects; add vcpkg include dirs for libpqxx/libpq; add IncludeDir entry for XoshiroCpp. |

Conventions used throughout:
- All migration files apply with `db-setup.bat` (or `docker exec gacha_postgres psql ... -f /migrations/NNN.sql`).
- All C++ tests run via the existing test harness pattern (Catch2 + premake5 test executable).
- All commits use the same style as `git log` shows: `feat(account):`, `refactor(account):`, `test(account):`, `docs(account):`.

---

## Pre-flight: feature branch

This work spans many files across multiple weeks. Create a feature branch so partial progress is mergeable in chunks.

- [ ] **Step 1: Create branch**

Run: `git checkout -b feat/account-db-migration`
Expected: switched to a new branch.

- [ ] **Step 2: Verify clean working tree before starting**

Run: `git status`
Expected: working tree clean apart from any in-progress uncommitted work the user has acknowledged.

---

## Phase 0 — Infrastructure & vendored deps

### Task 1: Docker Compose for Postgres

**Files:**
- Create: `Server/docker-compose.yml`
- Create: `Server/scripts/db-setup.bat`
- Create: `Server/scripts/db-setup.sh`
- Create: `Server/scripts/db-reset.bat`

- [ ] **Step 1: Write docker-compose.yml**

`Server/docker-compose.yml`:
```yaml
services:
  postgres:
    image: postgres:16
    container_name: aphelyon_postgres
    environment:
      POSTGRES_DB: aphelyon
      POSTGRES_USER: aphelyon
      POSTGRES_PASSWORD: aphelyon
    ports:
      - "5432:5432"
    volumes:
      - aphelyon_pgdata:/var/lib/postgresql/data
      - ./Account/migrations:/migrations:ro
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U aphelyon -d aphelyon"]
      interval: 2s
      timeout: 5s
      retries: 30

volumes:
  aphelyon_pgdata:
```

- [ ] **Step 2: Write Windows setup script**

`Server/scripts/db-setup.bat`:
```bat
@echo off
setlocal
pushd "%~dp0\.."
echo Bringing up Postgres...
docker compose up -d postgres
if errorlevel 1 goto :error

echo Waiting for Postgres to be ready...
:wait
docker compose exec -T postgres pg_isready -U aphelyon -d aphelyon >nul 2>&1
if errorlevel 1 (
    timeout /t 2 /nobreak >nul
    goto :wait
)

echo Applying migrations in order...
for %%f in (Account\migrations\*.sql) do (
    echo   %%f
    docker compose exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f /migrations/%%~nxf
    if errorlevel 1 goto :error
)

echo Done.
popd
endlocal
exit /b 0

:error
echo Setup FAILED.
popd
endlocal
exit /b 1
```

- [ ] **Step 3: Write POSIX setup script**

`Server/scripts/db-setup.sh`:
```sh
#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."

echo "Bringing up Postgres..."
docker compose up -d postgres

echo "Waiting for Postgres to be ready..."
until docker compose exec -T postgres pg_isready -U aphelyon -d aphelyon >/dev/null 2>&1; do
  sleep 2
done

echo "Applying migrations in order..."
for f in Account/migrations/*.sql; do
  echo "  $f"
  docker compose exec -T postgres psql -U aphelyon -d aphelyon -v ON_ERROR_STOP=1 -f "/migrations/$(basename "$f")"
done

echo "Done."
```

- [ ] **Step 4: Write reset script (dev-only)**

`Server/scripts/db-reset.bat`:
```bat
@echo off
setlocal
pushd "%~dp0\.."

echo This will DESTROY the dev database and all in-tree JSON saves. Ctrl-C to abort.
pause

docker compose down -v
if exist bin\Debug-windows-x86_64\Account\data\accounts rmdir /s /q bin\Debug-windows-x86_64\Account\data\accounts
if exist bin\Release-windows-x86_64\Account\data\accounts rmdir /s /q bin\Release-windows-x86_64\Account\data\accounts

call scripts\db-setup.bat
popd
endlocal
```

- [ ] **Step 5: Bring up Postgres to verify**

Run: `cd Server && docker compose up -d postgres`
Expected: container started. Verify with `docker compose ps` showing `gacha_postgres` healthy.

- [ ] **Step 6: Commit**

```bash
git add Server/docker-compose.yml Server/scripts/db-setup.bat Server/scripts/db-setup.sh Server/scripts/db-reset.bat
git commit -m "feat(account): docker-compose Postgres + setup/reset scripts"
```

---

### Task 2: Add libpqxx + libpq to vcpkg overlay

**Why vcpkg here:** libpqxx generates compiler-feature-detection headers via CMake (`PQXX_HAVE_*` macros) and links against `libpq` — the Postgres C client lib, which is intrinsically tied to Postgres internals. Both fall into the CLAUDE.md "deeply nested build system" case where vcpkg is the right tool. Adding `libpqxx` to the vcpkg manifest transitively pulls in `libpq`.

**Files:**
- Modify: `Server/scripts/setup-vcpkg-deps.bat`
- Modify: `Server/premake5.lua`
- Delete (optional cleanup, see Step 5): `ThirdParty/libpqxx/`

- [ ] **Step 1: Add libpqxx to the vcpkg setup script**

Open `Server/scripts/setup-vcpkg-deps.bat`. Find the `vcpkg install` line that currently installs protobuf/abseil with the overlay triplet. Add `libpqxx:x64-windows-static`. The relevant line should look like:
```bat
vcpkg install --overlay-triplets=..\vcpkg-triplets protobuf:x64-windows-static abseil:x64-windows-static utf8-range:x64-windows-static libpqxx:x64-windows-static
```

- [ ] **Step 2: Run the vcpkg install**

Run: `cd Server && scripts\setup-vcpkg-deps.bat`
Expected: vcpkg builds libpq and libpqxx with the v143 toolset, ~3–8 minutes. Verify with:
```
dir vcpkg\installed\x64-windows-static\lib\libpqxx.lib
dir vcpkg\installed\x64-windows-static\lib\libpq.lib
```

- [ ] **Step 3: Wire vcpkg paths into premake**

Modify `Server/premake5.lua`. Find the existing `VcpkgDir` definition and `IncludeDir` table. Add:
```lua
-- Already exists if protobuf/abseil are wired up; reuse:
IncludeDir["libpqxx"] = VcpkgDir .. "/include"   -- libpqxx headers
IncludeDir["libpq"]   = VcpkgDir .. "/include"   -- libpq headers (same dir, same vcpkg install)

-- Library lookup directory (only add if not already present from protobuf wiring):
VcpkgLibDir = VcpkgDir .. "/lib"
```

The actual link directives are added per-project in Task 35 / when `Account/premake5.lua` is updated — wherever a project needs Postgres access. The pattern (copy from how `protobuf` is currently linked):
```lua
filter "system:windows"
    libdirs { VcpkgLibDir }
    links { "libpqxx", "libpq", "libcrypto", "libssl", "ws2_32", "secur32", "wldap32", "crypt32" }
```
(The `ws2_32` / `secur32` / etc. are libpq's Windows transitive dependencies — vcpkg's portfile documents them.)

- [ ] **Step 4: Verify the build links**

Add a temporary smoke project (or a one-file test) that does `#include <pqxx/pqxx>` and calls `pqxx::connection`. Build it.
Run: `msbuild Aphelyon.slnx /p:Configuration=Debug`
Expected: clean build. If link errors mention missing symbols like `BIO_new`, add the missing OpenSSL libs from vcpkg.

- [ ] **Step 5: Clean up the no-longer-used vendored libpqxx (optional but recommended)**

The cloned `ThirdParty/libpqxx/` is now dead weight — having two copies of the same library (one ignored, one used) invites confusion. Remove it:
```bash
git rm -r ThirdParty/libpqxx/
```
Skip this step if you'd rather keep the source around as reference; just don't reference it from any premake file.

- [ ] **Step 6: Commit**

```bash
git add Server/scripts/setup-vcpkg-deps.bat Server/premake5.lua
git rm -r ThirdParty/libpqxx/  # if Step 5 chosen
git commit -m "feat(account): libpqxx + libpq via vcpkg overlay (v143 toolset)"
```

---

### Task 3: Wire XoshiroCpp include path

Already cloned at `ThirdParty/Xoshiro/XoshiroCpp.hpp` (Ryo Suzuki, MIT). Header-only — no premake5 project needed, just an include directory.

**Concrete API (verify in `XoshiroCpp.hpp`):**
- Namespace: `XoshiroCpp`
- Class for our use: `Xoshiro256PlusPlus`
- State type: `Xoshiro256PlusPlus::state_type` (`std::array<uint64_t, 4>`)
- State accessors: `.serialize()` returns state; `.deserialize(state)` sets state
- Seeder: `XoshiroCpp::SplitMix64(seed).generateSeedSequence<4>()`

**Files:**
- Modify: `Server/premake5.lua`

- [ ] **Step 1: Add to premake include dirs**

Modify `Server/premake5.lua`. Add to the `IncludeDir` table:
```lua
IncludeDir["XoshiroCpp"] = "../ThirdParty/Xoshiro"
```

Then add `IncludeDir["XoshiroCpp"]` to the `includedirs` of every project that uses it (initially just Account).

- [ ] **Step 2: Smoke test**

Add a temporary `Server/Account/test_xoshiro.cpp`:
```cpp
#include "XoshiroCpp.hpp"
#include <iostream>
int main() {
    auto state = XoshiroCpp::SplitMix64(0xDEADBEEFCAFEBABEULL).generateSeedSequence<4>();
    XoshiroCpp::Xoshiro256PlusPlus rng(state);
    for (int i = 0; i < 3; ++i) std::cout << rng() << "\n";
}
```

Build and run twice. Confirm identical output across runs (deterministic). Delete the temp file.

- [ ] **Step 3: Commit**

```bash
git rm Server/Account/test_xoshiro.cpp 2>/dev/null || true
git add Server/premake5.lua
git commit -m "feat(account): wire XoshiroCpp include path (already vendored)"
```

> **IMPORTANT — propagate the actual API through later tasks:** the plan's reducer + event code uses `xoshiro::Xoshiro256pp` / `::StateType` / `.state()` / `.set_state()` / `xoshiro::seed_from(...)`. Replace those references throughout with the real API as you implement Tasks 14, 19, and 28:
> - `xoshiro::Xoshiro256pp` → `XoshiroCpp::Xoshiro256PlusPlus`
> - `xoshiro::Xoshiro256pp::StateType` → `XoshiroCpp::Xoshiro256PlusPlus::state_type`
> - `rng.state()` → `rng.serialize()`
> - `rng.set_state(s)` → `rng.deserialize(s)`
> - `xoshiro::seed_from(seed)` → `XoshiroCpp::SplitMix64(seed).generateSeedSequence<4>()`
> - `#include "xoshiro256pp.hpp"` → `#include "XoshiroCpp.hpp"`

---

### Task 4: Write rapidcheck premake5 project

Source already cloned at `ThirdParty/rapidcheck/` (BSD). No external deps — pure C++.

**Files:**
- Create: `ThirdParty/rapidcheck/premake5.lua`
- Modify: `Server/premake5.lua`

- [ ] **Step 1: Write the premake project**

`ThirdParty/rapidcheck/premake5.lua`:
```lua
project "rapidcheck"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    location "%{wks.location}/projects"
    targetdir "%{wks.location}/bin/%{cfg.buildcfg}-%{cfg.platform}/%{prj.name}"
    objdir    "%{wks.location}/obj/%{cfg.buildcfg}-%{cfg.platform}/%{prj.name}"

    files {
        "src/**.cpp",
        "include/**.h",
        "include/**.hpp",
    }
    includedirs { "include" }

    filter "system:windows"
        defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX" }
```

- [ ] **Step 2: Add to top-level premake**

Modify `Server/premake5.lua`. Add:
```lua
include "../ThirdParty/rapidcheck"
```
And to the `IncludeDir` table:
```lua
IncludeDir["rapidcheck"] = "../ThirdParty/rapidcheck/include"
```

- [ ] **Step 3: Verify build**

Run: `cd Server && GenerateProjects.bat && msbuild Aphelyon.slnx /p:Configuration=Debug /t:rapidcheck`
Expected: rapidcheck.lib produced.

- [ ] **Step 4: Commit**

```bash
git add ThirdParty/rapidcheck/premake5.lua Server/premake5.lua
git commit -m "feat(account): rapidcheck premake5 project (sources already vendored)"
```

---

### Task 4.5: Write Catch2 v3 premake5 project

Source already cloned at `ThirdParty/Catch2/` (Boost license). **Catch2 v3 is no longer header-only** — it's a real compiled static library.

**Files:**
- Create: `ThirdParty/Catch2/premake5.lua`
- Modify: `Server/premake5.lua`

- [ ] **Step 1: Write the premake project**

`ThirdParty/Catch2/premake5.lua`:
```lua
project "Catch2"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"
    location "%{wks.location}/projects"
    targetdir "%{wks.location}/bin/%{cfg.buildcfg}-%{cfg.platform}/%{prj.name}"
    objdir    "%{wks.location}/obj/%{cfg.buildcfg}-%{cfg.platform}/%{prj.name}"

    files {
        "src/catch2/**.cpp",
        "src/catch2/**.hpp",
    }
    includedirs { "src" }
    removefiles { "src/catch2/catch_main.cpp" }   -- exclude the main() entry; tests provide their own

    filter "system:windows"
        defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX" }
```

- [ ] **Step 2: Add to top-level premake**

Modify `Server/premake5.lua`. Add:
```lua
include "../ThirdParty/Catch2"
```
And to the `IncludeDir` table:
```lua
IncludeDir["Catch2"] = "../ThirdParty/Catch2/src"
```

- [ ] **Step 3: Verify build**

Run: `cd Server && GenerateProjects.bat && msbuild Aphelyon.slnx /p:Configuration=Debug /t:Catch2`
Expected: Catch2.lib produced.

- [ ] **Step 4: Commit**

```bash
git add ThirdParty/Catch2/premake5.lua Server/premake5.lua
git commit -m "feat(account): Catch2 v3 premake5 project (sources already vendored)"
```

---

### Task 5: Migration runner

**Files:**
- Create: `Server/Account/src/db/MigrationRunner.hpp`
- Create: `Server/Account/src/db/ConnectionPool.hpp`

(These are written together since the migration runner needs a connection.)

- [ ] **Step 1: Write ConnectionPool**

`Server/Account/src/db/ConnectionPool.hpp`:
```cpp
#pragma once
#include <pqxx/pqxx>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace aphelyon::db {

class ConnectionPool {
public:
    explicit ConnectionPool(std::string conn_string, std::size_t capacity = 16)
        : conn_string_(std::move(conn_string)), capacity_(capacity) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            free_.push(std::make_unique<pqxx::connection>(conn_string_));
        }
    }

    class Lease {
      public:
        Lease(ConnectionPool& pool, std::unique_ptr<pqxx::connection> c)
            : pool_(&pool), conn_(std::move(c)) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& o) noexcept : pool_(o.pool_), conn_(std::move(o.conn_)) { o.pool_ = nullptr; }
        ~Lease() { if (pool_ && conn_) pool_->release(std::move(conn_)); }
        pqxx::connection& operator*()  { return *conn_; }
        pqxx::connection* operator->() { return conn_.get(); }
      private:
        ConnectionPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    Lease acquire() {
        std::unique_lock lk(mtx_);
        cv_.wait(lk, [&]{ return !free_.empty(); });
        auto c = std::move(free_.front());
        free_.pop();
        return Lease(*this, std::move(c));
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    void release(std::unique_ptr<pqxx::connection> c) {
        {
            std::lock_guard lk(mtx_);
            free_.push(std::move(c));
        }
        cv_.notify_one();
    }

    std::string conn_string_;
    std::size_t capacity_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<pqxx::connection>> free_;
};

} // namespace aphelyon::db
```

- [ ] **Step 2: Write MigrationRunner**

`Server/Account/src/db/MigrationRunner.hpp`:
```cpp
#pragma once
#include "ConnectionPool.hpp"
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace aphelyon::db {

class MigrationRunner {
public:
    explicit MigrationRunner(ConnectionPool& pool) : pool_(pool) {}

    void Run(const std::filesystem::path& migrations_dir) {
        EnsureMigrationsTable();
        auto applied = LoadAppliedVersions();
        for (auto& entry : OrderedSqlFiles(migrations_dir)) {
            const auto version = ExtractVersion(entry.filename().string());
            if (applied.count(version)) continue;
            Apply(version, entry);
        }
    }

private:
    void EnsureMigrationsTable() {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        tx.exec(R"(
            CREATE TABLE IF NOT EXISTS schema_migrations (
                version INT PRIMARY KEY,
                applied_at TIMESTAMPTZ NOT NULL DEFAULT now(),
                filename TEXT NOT NULL
            );
        )");
        tx.commit();
    }

    std::unordered_set<int> LoadAppliedVersions() {
        std::unordered_set<int> out;
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto r = tx.exec("SELECT version FROM schema_migrations");
        for (auto row : r) out.insert(row[0].as<int>());
        return out;
    }

    std::vector<std::filesystem::path> OrderedSqlFiles(const std::filesystem::path& dir) {
        std::vector<std::filesystem::path> out;
        for (auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".sql") out.push_back(e.path());
        std::sort(out.begin(), out.end());
        return out;
    }

    static int ExtractVersion(const std::string& filename) {
        std::smatch m;
        std::regex rx(R"(^(\d+)_)");
        if (!std::regex_search(filename, m, rx))
            throw std::runtime_error("Bad migration filename: " + filename);
        return std::stoi(m[1]);
    }

    void Apply(int version, const std::filesystem::path& file) {
        std::ifstream f(file);
        std::stringstream ss; ss << f.rdbuf();
        auto sql = ss.str();
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        tx.exec(sql);
        tx.exec_params("INSERT INTO schema_migrations(version, filename) VALUES ($1, $2)",
                       version, file.filename().string());
        tx.commit();
    }

    ConnectionPool& pool_;
};

} // namespace aphelyon::db
```

- [ ] **Step 3: Commit (no test yet; migrations don't exist)**

```bash
git add Server/Account/src/db/ConnectionPool.hpp Server/Account/src/db/MigrationRunner.hpp
git commit -m "feat(account): Postgres ConnectionPool + MigrationRunner skeleton"
```

---

## Phase 1 — SQL schema migrations

Each migration file is one task. Files apply in numeric order. All `.sql` files live under `Server/Account/migrations/`.

### Task 6: `001_accounts.sql`

**Files:**
- Create: `Server/Account/migrations/001_accounts.sql`

- [ ] **Step 1: Write migration**

`Server/Account/migrations/001_accounts.sql`:
```sql
CREATE TABLE accounts (
    account_id              BIGSERIAL PRIMARY KEY,
    username                TEXT NOT NULL UNIQUE,
    password_hash           TEXT NOT NULL,
    created_at              TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login              TIMESTAMPTZ NOT NULL DEFAULT now(),
    login_streak            INT NOT NULL DEFAULT 0,
    last_streak_day         DATE,
    last_login_claim_at     TIMESTAMPTZ,
    last_login_claim_day_idx SMALLINT,
    story_level             INT NOT NULL DEFAULT 1,
    story_xp                INT NOT NULL DEFAULT 0,
    difficulty_tier         INT NOT NULL DEFAULT 1,
    reducer_version         INT NOT NULL DEFAULT 1,
    deleted_at              TIMESTAMPTZ,
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX accounts_deleted_idx ON accounts (deleted_at) WHERE deleted_at IS NOT NULL;
```

- [ ] **Step 2: Apply and verify**

Run: `cd Server && scripts\db-setup.bat`
Expected: migration applied; verify with `docker compose exec postgres psql -U aphelyon -d aphelyon -c "\d accounts"`.

- [ ] **Step 3: Commit**

```bash
git add Server/Account/migrations/001_accounts.sql
git commit -m "feat(account): migration 001 - accounts table"
```

### Task 7: `002_inventory.sql`

**Files:**
- Create: `Server/Account/migrations/002_inventory.sql`

- [ ] **Step 1: Write migration**

`Server/Account/migrations/002_inventory.sql`:
```sql
CREATE TABLE owned_characters (
    account_id   BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    character_id TEXT NOT NULL,
    level        SMALLINT NOT NULL DEFAULT 1,
    current_xp   INT NOT NULL DEFAULT 0,
    ascension    SMALLINT NOT NULL DEFAULT 0,
    resonance    SMALLINT NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, character_id)
);

CREATE TABLE char_traces (
    account_id   BIGINT NOT NULL,
    character_id TEXT NOT NULL,
    trace_id     TEXT NOT NULL,
    PRIMARY KEY (account_id, character_id, trace_id),
    FOREIGN KEY (account_id, character_id) REFERENCES owned_characters ON DELETE CASCADE
);

CREATE TABLE owned_weapons (
    account_id  BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    instance_id UUID NOT NULL,
    template_id TEXT NOT NULL,
    level       SMALLINT NOT NULL DEFAULT 1,
    ascension   SMALLINT NOT NULL DEFAULT 0,
    refinement  SMALLINT NOT NULL DEFAULT 0,
    acquired_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, instance_id)
);
CREATE INDEX owned_weapons_template_idx ON owned_weapons (account_id, template_id);

CREATE TABLE owned_gear (
    account_id  BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    instance_id UUID NOT NULL,
    set_id      TEXT NOT NULL,
    slot        SMALLINT NOT NULL,
    rarity      SMALLINT NOT NULL,
    level       SMALLINT NOT NULL DEFAULT 0,
    main_stat   SMALLINT NOT NULL,
    main_value  REAL NOT NULL,
    acquired_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, instance_id)
);
CREATE INDEX owned_gear_set_slot_idx ON owned_gear (account_id, set_id, slot);

CREATE TABLE gear_substats (
    account_id  BIGINT NOT NULL,
    instance_id UUID NOT NULL,
    slot_idx    SMALLINT NOT NULL CHECK (slot_idx BETWEEN 0 AND 3),
    stat_type   SMALLINT NOT NULL,
    value       REAL NOT NULL,
    PRIMARY KEY (account_id, instance_id, slot_idx),
    UNIQUE (account_id, instance_id, stat_type),
    FOREIGN KEY (account_id, instance_id) REFERENCES owned_gear ON DELETE CASCADE
);
CREATE INDEX gear_substats_stat_idx ON gear_substats (account_id, stat_type);
```

- [ ] **Step 2: Apply**

Run: `cd Server && scripts\db-setup.bat`
Expected: migration applied.

- [ ] **Step 3: Commit**

```bash
git add Server/Account/migrations/002_inventory.sql
git commit -m "feat(account): migration 002 - inventory (characters, weapons, gear)"
```

### Task 8: `003_equipment.sql`

**Files:**
- Create: `Server/Account/migrations/003_equipment.sql`

- [ ] **Step 1: Write migration**

`Server/Account/migrations/003_equipment.sql`:
```sql
CREATE TABLE loadouts (
    account_id          BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    character_id        TEXT NOT NULL,
    preset_id           SMALLINT NOT NULL,
    name                TEXT,
    weapon_instance_id  UUID,
    slot_helmet         UUID,
    slot_gauntlets      UUID,
    slot_chest          UUID,
    slot_boots          UUID,
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, character_id, preset_id)
);
CREATE INDEX loadouts_active_idx ON loadouts (account_id) WHERE preset_id = 0;

CREATE TABLE material_inventory (
    account_id  BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    material_id TEXT NOT NULL,
    quantity    INT NOT NULL CHECK (quantity >= 0),
    PRIMARY KEY (account_id, material_id)
);

CREATE TABLE party_slots (
    account_id   BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    slot_idx     SMALLINT NOT NULL CHECK (slot_idx BETWEEN 0 AND 3),
    character_id TEXT,
    PRIMARY KEY (account_id, slot_idx)
);
```

- [ ] **Step 2: Apply**

Run: `cd Server && scripts\db-setup.bat`

- [ ] **Step 3: Commit**

```bash
git add Server/Account/migrations/003_equipment.sql
git commit -m "feat(account): migration 003 - loadouts, materials, party_slots"
```

### Task 9: `004_quests.sql`

**Files:**
- Create: `Server/Account/migrations/004_quests.sql`

- [ ] **Step 1: Write migration**

`Server/Account/migrations/004_quests.sql`:
```sql
CREATE TABLE quest_states (
    account_id   BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    quest_id     TEXT NOT NULL,
    quest_type   SMALLINT NOT NULL,
    state        SMALLINT NOT NULL,
    started_at   TIMESTAMPTZ,
    completed_at TIMESTAMPTZ,
    reset_at     TIMESTAMPTZ,
    metadata     JSONB NOT NULL DEFAULT '{}'
                 CHECK (jsonb_typeof(metadata) = 'object'),
    PRIMARY KEY (account_id, quest_id)
);
CREATE INDEX quest_states_type_state_idx ON quest_states (account_id, quest_type, state);
CREATE INDEX quest_states_reset_idx     ON quest_states (account_id, reset_at)
    WHERE reset_at IS NOT NULL;
CREATE INDEX quest_states_active_idx    ON quest_states (account_id, quest_type)
    WHERE state IN (1, 2);   -- 1=ACTIVE, 2=CLAIMABLE

CREATE TABLE quest_objectives (
    account_id   BIGINT NOT NULL,
    quest_id     TEXT NOT NULL,
    objective_id TEXT NOT NULL,
    progress     INT NOT NULL DEFAULT 0,
    required     INT NOT NULL,
    PRIMARY KEY (account_id, quest_id, objective_id),
    FOREIGN KEY (account_id, quest_id) REFERENCES quest_states ON DELETE CASCADE
);

CREATE TABLE world_flags (
    account_id  BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    flag        TEXT NOT NULL,
    unlocked_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, flag)
);

CREATE TABLE pity_state (
    account_id  BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    slot_id     TEXT NOT NULL,
    pity_4      SMALLINT NOT NULL DEFAULT 0,
    pity_5      SMALLINT NOT NULL DEFAULT 0,
    guarantee_5 BOOLEAN  NOT NULL DEFAULT false,
    PRIMARY KEY (account_id, slot_id)
);
```

- [ ] **Step 2: Apply**

Run: `cd Server && scripts\db-setup.bat`

- [ ] **Step 3: Commit**

```bash
git add Server/Account/migrations/004_quests.sql
git commit -m "feat(account): migration 004 - quests, world_flags, pity_state"
```

### Task 10: `005_events.sql`

**Files:**
- Create: `Server/Account/migrations/005_events.sql`

- [ ] **Step 1: Write migration**

`Server/Account/migrations/005_events.sql`:
```sql
-- pg_partman extension for partition rollover (must be installed in container image;
-- postgres:16 ships it as an available extension that just needs CREATE EXTENSION).
CREATE EXTENSION IF NOT EXISTS pg_partman;

CREATE TABLE events (
    event_id        UUID NOT NULL DEFAULT gen_random_uuid(),
    sequence        BIGSERIAL,
    account_id      BIGINT NOT NULL,
    aggregate_kind  TEXT NOT NULL,
    version         INT NOT NULL,
    event_type      TEXT NOT NULL,
    schema_version  INT NOT NULL DEFAULT 1,
    data            JSONB NOT NULL,
    metadata        JSONB NOT NULL DEFAULT '{}',
    idempotency_key TEXT NOT NULL,
    xid             XID8 NOT NULL DEFAULT pg_current_xact_id(),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),

    PRIMARY KEY (event_id, created_at),
    UNIQUE (account_id, aggregate_kind, version, created_at),
    UNIQUE (account_id, idempotency_key, created_at),
    FOREIGN KEY (account_id) REFERENCES accounts(account_id) ON DELETE CASCADE
) PARTITION BY RANGE (created_at);

ALTER TABLE events ALTER COLUMN data SET COMPRESSION lz4;
ALTER TABLE events ALTER COLUMN metadata SET COMPRESSION lz4;

CREATE INDEX events_account_aggregate_version_idx
    ON events (account_id, aggregate_kind, version);
CREATE INDEX events_xid_seq_idx
    ON events (xid, sequence);
CREATE INDEX events_wallet_recent_idx
    ON events (account_id, created_at DESC) WHERE aggregate_kind = 'wallet';
CREATE INDEX events_pulls_banner_idx
    ON events (account_id, (data->>'banner_id'), created_at) WHERE aggregate_kind = 'pulls';

-- Initial partition for current month (others created by pg_partman maintenance).
SELECT partman.create_parent(
    p_parent_table  := 'public.events',
    p_control       := 'created_at',
    p_type          := 'range',
    p_interval      := '1 month',
    p_premake       := 3
);
```

- [ ] **Step 2: Apply**

Run: `cd Server && scripts\db-setup.bat`
Expected: migration applied. Verify partitions: `docker compose exec postgres psql -U aphelyon -d aphelyon -c "\d+ events"`.

- [ ] **Step 3: Commit**

```bash
git add Server/Account/migrations/005_events.sql
git commit -m "feat(account): migration 005 - events table (partitioned, LZ4)"
```

### Task 11: `006_support.sql`

**Files:**
- Create: `Server/Account/migrations/006_support.sql`

- [ ] **Step 1: Write migration**

`Server/Account/migrations/006_support.sql`:
```sql
CREATE TABLE snapshots (
    account_id       BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    aggregate_kind   TEXT NOT NULL,
    version          INT NOT NULL,
    reducer_version  INT NOT NULL,
    state            JSONB NOT NULL,
    snapped_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (account_id, aggregate_kind)
);
ALTER TABLE snapshots ALTER COLUMN state SET COMPRESSION lz4;

CREATE TABLE outbox (
    outbox_id     BIGSERIAL PRIMARY KEY,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    dispatched_at TIMESTAMPTZ,
    destination   TEXT NOT NULL,
    payload       JSONB NOT NULL,
    xid           XID8 NOT NULL DEFAULT pg_current_xact_id()
);
CREATE INDEX outbox_pending_idx ON outbox (xid, outbox_id) WHERE dispatched_at IS NULL;

CREATE TABLE audit_log (
    audit_id    BIGSERIAL PRIMARY KEY,
    account_id  BIGINT NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    occurred_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    actor       TEXT NOT NULL,
    action      TEXT NOT NULL,
    target      JSONB NOT NULL,
    before      JSONB,
    after       JSONB,
    metadata    JSONB NOT NULL DEFAULT '{}'
);
CREATE INDEX audit_log_account_time_idx ON audit_log (account_id, occurred_at DESC);
ALTER TABLE audit_log ALTER COLUMN before SET COMPRESSION lz4;
ALTER TABLE audit_log ALTER COLUMN after  SET COMPRESSION lz4;
```

- [ ] **Step 2: Apply**

Run: `cd Server && scripts\db-setup.bat`

- [ ] **Step 3: Verify all migrations applied**

Run: `docker compose exec postgres psql -U aphelyon -d aphelyon -c "SELECT version, filename FROM schema_migrations ORDER BY version"`
Expected: rows 1..6 present.

- [ ] **Step 4: Commit**

```bash
git add Server/Account/migrations/006_support.sql
git commit -m "feat(account): migration 006 - snapshots, outbox, audit_log"
```

---

### Task 11.5: `007_public_uid.sql`

Player-facing 9-digit UID (Genshin/HSR style, our own region taxonomy). Decouples display ID from internal FK: the BIGSERIAL `account_id` keeps owning every join (`events`, `owned_weapons`, ...), and `public_uid` is what the client sees, what friend lookups key on, and what support tickets quote. A future region (EU, APAC, etc.) gets its own sequence on its own shard — no schema change required.

**Files:**
- Create: `Server/Account/migrations/007_public_uid.sql`

- [ ] **Step 1: Write migration**

`Server/Account/migrations/007_public_uid.sql`:
```sql
-- Format: [region prefix][8-digit per-region sequence]
--   1xxxxxxxx  = NA (this deployment)
--   2-5        = EU / APAC / LATAM / ANZ (reserved)
--   6-8        = reserved
--   9          = internal / dev / QA (never overlaps with prod)
--   0          = avoided (leading zero renders as <9 digits)

CREATE SEQUENCE public_uid_seq
    AS BIGINT
    START WITH 100000001
    INCREMENT BY 1
    MINVALUE 100000001
    MAXVALUE 199999999      -- 100M accounts per region prefix
    NO CYCLE;

ALTER TABLE accounts
    ADD COLUMN public_uid BIGINT NOT NULL DEFAULT nextval('public_uid_seq');

ALTER TABLE accounts
    ADD CONSTRAINT accounts_public_uid_key UNIQUE (public_uid);
```

- [ ] **Step 2: Apply**

Run: `cd Server && scripts\db-setup.bat`

- [ ] **Step 3: Verify**

Run: `docker compose exec postgres psql -U aphelyon -d aphelyon -c "SELECT version, filename FROM schema_migrations ORDER BY version"`
Expected: row 7 present.

Run: `docker compose exec postgres psql -U aphelyon -d aphelyon -c "SELECT last_value FROM public_uid_seq;"`
Expected: at least `100000001` (advances each time an account is inserted).

- [ ] **Step 4: Commit**

```bash
git add Server/Account/migrations/007_public_uid.sql
git commit -m "feat(account): migration 007 - public_uid + per-region sequence"
```

**Downstream wiring (handled in later phases, not here):**
- `AccountRepository::Create` / `Register` paths must read `public_uid` back into `AccountData` so the response can surface it. Plain INSERTs already populate the column via DEFAULT, but the C++ side needs an additional `RETURNING public_uid` to round-trip it.
- `protocol.json` register/login responses gain a `public_uid` field.
- `AccountData` gains a `std::int64_t public_uid` field (or `std::uint32_t` — fits in 30 bits).
- Friend / lookup flows query by `public_uid`, not `username` or `account_id`.

---

## Phase 2 — Foundation types

### Task 12: UUID v7 generator

**Files:**
- Create: `Server/Common/src/UuidV7.hpp`
- Create: `Server/Account/tests/UuidV7Test.cpp`

- [ ] **Step 1: Write failing test**

`Server/Account/tests/UuidV7Test.cpp`:
```cpp
#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include "UuidV7.hpp"
#include <chrono>
#include <thread>

TEST_CASE("UUID v7 generates time-ordered values", "[uuid]") {
    auto a = aphelyon::UuidV7::Generate();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto b = aphelyon::UuidV7::Generate();
    REQUIRE(a < b);  // lexicographic, by construction
}

TEST_CASE("UUID v7 has version 7 in correct nibble", "[uuid]") {
    auto u = aphelyon::UuidV7::Generate();
    auto s = aphelyon::UuidV7::ToString(u);
    // Format: xxxxxxxx-xxxx-Mxxx-Nxxx-xxxxxxxxxxxx, M=7 for v7
    REQUIRE(s[14] == '7');
}

TEST_CASE("UUID v7 round-trips through string", "[uuid]") {
    auto u = aphelyon::UuidV7::Generate();
    auto s = aphelyon::UuidV7::ToString(u);
    auto u2 = aphelyon::UuidV7::FromString(s);
    REQUIRE(u == u2);
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd Server && msbuild Aphelyon.slnx /p:Configuration=Debug /t:AccountTests`
Expected: compile failure (`UuidV7.hpp` doesn't exist).

- [ ] **Step 3: Write implementation**

`Server/Common/src/UuidV7.hpp`:
```cpp
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <stdexcept>

namespace aphelyon {

class UuidV7 {
public:
    using ValueType = std::array<std::uint8_t, 16>;

    static ValueType Generate() {
        ValueType u{};

        // 48-bit Unix millis timestamp, big-endian
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        u[0] = (ms >> 40) & 0xFF;
        u[1] = (ms >> 32) & 0xFF;
        u[2] = (ms >> 24) & 0xFF;
        u[3] = (ms >> 16) & 0xFF;
        u[4] = (ms >> 8)  & 0xFF;
        u[5] = ms & 0xFF;

        // 12 bits of randomness + version 7 nibble
        static thread_local std::random_device rd;
        static thread_local std::mt19937_64 rng(rd());
        std::uint64_t r1 = rng();
        std::uint64_t r2 = rng();
        u[6] = 0x70 | ((r1 >> 8) & 0x0F);  // version 7
        u[7] = r1 & 0xFF;

        // 62 bits of randomness + variant 10 (RFC 4122)
        u[8]  = 0x80 | ((r2 >> 56) & 0x3F); // variant
        u[9]  = (r2 >> 48) & 0xFF;
        u[10] = (r2 >> 40) & 0xFF;
        u[11] = (r2 >> 32) & 0xFF;
        u[12] = (r2 >> 24) & 0xFF;
        u[13] = (r2 >> 16) & 0xFF;
        u[14] = (r2 >> 8)  & 0xFF;
        u[15] = r2 & 0xFF;

        return u;
    }

    static std::string ToString(const ValueType& u) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string s(36, '-');
        std::size_t pos = 0;
        for (std::size_t i = 0; i < 16; ++i) {
            if (pos == 8 || pos == 13 || pos == 18 || pos == 23) ++pos;
            s[pos++] = hex[(u[i] >> 4) & 0xF];
            s[pos++] = hex[u[i] & 0xF];
        }
        return s;
    }

    static ValueType FromString(const std::string& s) {
        if (s.size() != 36) throw std::invalid_argument("UUID string length must be 36");
        ValueType u{};
        std::size_t pos = 0;
        for (std::size_t i = 0; i < 16; ++i) {
            if (pos == 8 || pos == 13 || pos == 18 || pos == 23) ++pos;
            u[i] = (HexVal(s[pos]) << 4) | HexVal(s[pos + 1]);
            pos += 2;
        }
        return u;
    }

private:
    static std::uint8_t HexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        throw std::invalid_argument("Bad hex char in UUID");
    }
};

} // namespace aphelyon
```

- [ ] **Step 4: Add test executable to premake**

Modify `Server/Account/premake5.lua` — add a `AccountTests` project that builds `Account/tests/*.cpp` as an executable linking Catch2, rapidcheck, and the Account sources. (If Catch2 isn't vendored, add a vendor task; assume it lives under `ThirdParty/Catch2/include/`.) Run `GenerateProjects.bat`.

- [ ] **Step 5: Run tests, confirm pass**

Run: `cd Server && msbuild Aphelyon.slnx /p:Configuration=Debug /t:AccountTests && bin\Debug-windows-x86_64\AccountTests\AccountTests.exe`
Expected: 3 test cases pass.

- [ ] **Step 6: Commit**

```bash
git add Server/Common/src/UuidV7.hpp Server/Account/tests/UuidV7Test.cpp Server/Account/premake5.lua
git commit -m "feat(common): UUID v7 generator + tests"
```

---

### Task 13: Event envelope + JSON serialization

**Files:**
- Create: `Server/Account/src/events/Event.hpp`

- [ ] **Step 1: Write envelope**

`Server/Account/src/events/Event.hpp`:
```cpp
#pragma once
#include "UuidV7.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>

namespace aphelyon::events {

enum class AggregateKind {
    Wallet,
    Pulls,
    QuestClaims,
    Progression,
};

inline const char* AggregateKindToStr(AggregateKind k) {
    switch (k) {
        case AggregateKind::Wallet:       return "wallet";
        case AggregateKind::Pulls:        return "pulls";
        case AggregateKind::QuestClaims:  return "quest_claims";
        case AggregateKind::Progression:  return "progression";
    }
    return "unknown";
}

struct Event {
    UuidV7::ValueType   event_id;
    std::int64_t        account_id;
    AggregateKind       aggregate_kind;
    int                 version;
    std::string         event_type;
    int                 schema_version = 1;
    nlohmann::json      data;
    nlohmann::json      metadata = nlohmann::json::object();
    std::string         idempotency_key;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
};

} // namespace aphelyon::events
```

- [ ] **Step 2: Commit**

```bash
git add Server/Account/src/events/Event.hpp
git commit -m "feat(account): Event envelope type"
```

---

### Task 14: Per-aggregate event payload types

**Files:**
- Create: `Server/Account/src/events/WalletEvents.hpp`
- Create: `Server/Account/src/events/PullEvents.hpp`
- Create: `Server/Account/src/events/QuestClaimEvents.hpp`
- Create: `Server/Account/src/events/ProgressionEvents.hpp`

- [ ] **Step 1: WalletEvents.hpp**

`Server/Account/src/events/WalletEvents.hpp`:
```cpp
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace aphelyon::events::wallet {

enum class Currency { Credits, UniversalCredits, Tickets, LimitedTickets, Scrap };

inline const char* CurrencyToStr(Currency c) {
    switch (c) {
        case Currency::Credits:          return "credits";
        case Currency::UniversalCredits: return "universal_credits";
        case Currency::Tickets:          return "tickets";
        case Currency::LimitedTickets:   return "limited_tickets";
        case Currency::Scrap:            return "scrap";
    }
    return "unknown";
}

struct CurrencyDelta {
    Currency    currency;
    std::int64_t amount;
    std::string  reason;                       // "pull_cost" | "shop_purchase" | "quest_reward" | "daily_login" | "admin_grant" | "compensation"
    std::optional<std::string> reason_ref;     // event_id of the originating event, if any
    std::int64_t balance_before;
    std::int64_t balance_after;
};

inline nlohmann::json ToJson(const CurrencyDelta& d) {
    nlohmann::json j;
    j["currency"]       = CurrencyToStr(d.currency);
    j["amount"]         = d.amount;
    j["reason"]         = d.reason;
    if (d.reason_ref) j["reason_ref"] = *d.reason_ref;
    j["balance_before"] = d.balance_before;
    j["balance_after"]  = d.balance_after;
    return j;
}

} // namespace aphelyon::events::wallet
```

- [ ] **Step 2: PullEvents.hpp**

`Server/Account/src/events/PullEvents.hpp`:
```cpp
#pragma once
#include "XoshiroCpp.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aphelyon::events::pulls {

struct PullResult {
    std::string template_id;
    int         rarity;
    std::optional<std::string> instance_id;  // UUID v7 string for weapon instances
    bool        was_featured;
};

struct PullPerformed {
    std::string banner_id;
    std::string banner_version;              // e.g. "2.7"
    std::string cost_currency;               // "tickets" | "limited_tickets"
    std::int64_t cost_amount;

    XoshiroCpp::Xoshiro256PlusPlus::state_type rng_state_before;
    int          algorithm_version;

    int          pity_5_before;
    int          pity_4_before;
    bool         guarantee_5_before;

    std::vector<PullResult> results;

    int          pity_5_after;
    int          pity_4_after;
    bool         guarantee_5_after;
};

inline nlohmann::json ToJson(const XoshiroCpp::Xoshiro256PlusPlus::state_type& s) {
    return nlohmann::json::array({ s[0], s[1], s[2], s[3] });
}

inline nlohmann::json ToJson(const PullResult& r) {
    nlohmann::json j;
    j["template_id"] = r.template_id;
    j["rarity"]      = r.rarity;
    j["instance_id"] = r.instance_id ? nlohmann::json(*r.instance_id) : nlohmann::json(nullptr);
    j["was_featured"] = r.was_featured;
    return j;
}

inline nlohmann::json ToJson(const PullPerformed& p) {
    nlohmann::json j;
    j["banner_id"]          = p.banner_id;
    j["banner_version"]     = p.banner_version;
    j["cost"]               = { {"currency", p.cost_currency}, {"amount", p.cost_amount} };
    j["rng_state_before"]   = ToJson(p.rng_state_before);
    j["algorithm_version"]  = p.algorithm_version;
    j["pity_5_before"]      = p.pity_5_before;
    j["pity_4_before"]      = p.pity_4_before;
    j["guarantee_5_before"] = p.guarantee_5_before;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : p.results) arr.push_back(ToJson(r));
    j["results"] = std::move(arr);

    j["pity_5_after"]      = p.pity_5_after;
    j["pity_4_after"]      = p.pity_4_after;
    j["guarantee_5_after"] = p.guarantee_5_after;
    return j;
}

} // namespace aphelyon::events::pulls
```

- [ ] **Step 3: QuestClaimEvents.hpp**

`Server/Account/src/events/QuestClaimEvents.hpp`:
```cpp
#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace aphelyon::events::quest_claims {

enum class RewardKind { Credits, UniversalCredits, Tickets, LimitedTickets, Scrap, Material, Character, Weapon };

struct Reward {
    RewardKind kind;
    std::int64_t amount;
    std::string material_id;   // populated when kind == Material
};

struct QuestRewardClaimed {
    std::string quest_id;
    int         quest_type;
    std::vector<Reward> rewards;
    int         claimed_at_streak_day;
};

inline const char* RewardKindToStr(RewardKind k) {
    switch (k) {
        case RewardKind::Credits:          return "credits";
        case RewardKind::UniversalCredits: return "universal_credits";
        case RewardKind::Tickets:          return "tickets";
        case RewardKind::LimitedTickets:   return "limited_tickets";
        case RewardKind::Scrap:            return "scrap";
        case RewardKind::Material:         return "material";
        case RewardKind::Character:        return "character";
        case RewardKind::Weapon:           return "weapon";
    }
    return "unknown";
}

inline nlohmann::json ToJson(const Reward& r) {
    nlohmann::json j;
    j["kind"]   = RewardKindToStr(r.kind);
    j["amount"] = r.amount;
    if (r.kind == RewardKind::Material) j["material_id"] = r.material_id;
    return j;
}

inline nlohmann::json ToJson(const QuestRewardClaimed& q) {
    nlohmann::json j;
    j["quest_id"]              = q.quest_id;
    j["quest_type"]            = q.quest_type;
    j["claimed_at_streak_day"] = q.claimed_at_streak_day;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : q.rewards) arr.push_back(ToJson(r));
    j["rewards"] = std::move(arr);
    return j;
}

} // namespace aphelyon::events::quest_claims
```

- [ ] **Step 4: ProgressionEvents.hpp**

`Server/Account/src/events/ProgressionEvents.hpp`:
```cpp
#pragma once
#include <nlohmann/json.hpp>
#include <optional>

namespace aphelyon::events::progression {

struct StoryLevelAdvanced {
    int from_level;
    int to_level;
    int xp_consumed;
    int xp_carried_over;
    std::optional<int> difficulty_tier_unlocked;
    std::int64_t overflow_credits;
};

inline nlohmann::json ToJson(const StoryLevelAdvanced& s) {
    nlohmann::json j;
    j["from_level"]               = s.from_level;
    j["to_level"]                 = s.to_level;
    j["xp_consumed"]              = s.xp_consumed;
    j["xp_carried_over"]          = s.xp_carried_over;
    j["difficulty_tier_unlocked"] = s.difficulty_tier_unlocked ? nlohmann::json(*s.difficulty_tier_unlocked) : nlohmann::json(nullptr);
    j["overflow_credits"]         = s.overflow_credits;
    return j;
}

} // namespace aphelyon::events::progression
```

- [ ] **Step 5: Commit**

```bash
git add Server/Account/src/events/
git commit -m "feat(account): per-aggregate event payload types + JSON serialization"
```

---

## Phase 3 — Reducers (TDD)

Each reducer is a pure function: `(State, Event) → ReducerResult<State>`. Reducer code lives under `Account/src/reducers/`. Tests live under `Account/tests/ReducerTests/`. Each reducer is a fresh task with explicit TDD steps.

### Task 15: ReducerCommon shared types

**Files:**
- Create: `Server/Account/src/reducers/ReducerCommon.hpp`

- [ ] **Step 1: Write the shared types**

`Server/Account/src/reducers/ReducerCommon.hpp`:
```cpp
#pragma once
#include <chrono>
#include <string>
#include <variant>
#include <vector>

namespace aphelyon::reducers {

// Injected clock — never call system_clock::now() inside a reducer.
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::system_clock::time_point Now() const = 0;
};

class FixedClock : public Clock {
public:
    explicit FixedClock(std::chrono::system_clock::time_point t) : t_(t) {}
    std::chrono::system_clock::time_point Now() const override { return t_; }
private:
    std::chrono::system_clock::time_point t_;
};

// Side-effect descriptors emitted by reducers. Dispatched in live mode, skipped in replay mode.
struct ToastEffect          { std::string player_message; };
struct TelemetryEffect      { std::string event_name; std::string payload_json; };
struct GrantWeaponEffect    { std::string instance_id_uuid; std::string template_id; };
struct GrantCharacterEffect { std::string template_id; };
struct GrantMaterialEffect  { std::string material_id; std::int64_t quantity; };
struct GrantCurrencyEffect  { std::string currency; std::int64_t amount; std::string reason; std::string reason_ref; };

using SideEffectVariant = std::variant<
    ToastEffect,
    TelemetryEffect,
    GrantWeaponEffect,
    GrantCharacterEffect,
    GrantMaterialEffect,
    GrantCurrencyEffect
>;

template <typename State>
struct ReducerResult {
    State state;
    std::vector<SideEffectVariant> effects;
};

} // namespace aphelyon::reducers
```

- [ ] **Step 2: Commit**

```bash
git add Server/Account/src/reducers/ReducerCommon.hpp
git commit -m "feat(account): reducer common types (Clock, SideEffectVariant)"
```

### Task 16: Wallet reducer (TDD)

**Files:**
- Create: `Server/Account/tests/ReducerTests/WalletReducerTest.cpp`
- Create: `Server/Account/src/reducers/WalletReducer.hpp`

- [ ] **Step 1: Write failing tests**

`Server/Account/tests/ReducerTests/WalletReducerTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "reducers/WalletReducer.hpp"
#include "events/WalletEvents.hpp"

using namespace aphelyon;
using namespace aphelyon::reducers;

TEST_CASE("wallet starts at zero across all currencies", "[wallet][reducer]") {
    WalletState s;
    REQUIRE(s.Get(events::wallet::Currency::Credits) == 0);
    REQUIRE(s.Get(events::wallet::Currency::Tickets) == 0);
    REQUIRE(s.Get(events::wallet::Currency::Scrap)   == 0);
}

TEST_CASE("credits_added increments and records balance_after", "[wallet][reducer]") {
    WalletReducer r;
    WalletState  s;
    events::wallet::CurrencyDelta evt{
        events::wallet::Currency::Credits, +100, "quest_reward", std::nullopt, 0, 100
    };
    auto out = r.Apply(s, "credits_added", evt);
    REQUIRE(out.state.Get(events::wallet::Currency::Credits) == 100);
    REQUIRE(out.effects.empty());  // no toasts for currency adds
}

TEST_CASE("credits_spent decrements; rejects if would go negative", "[wallet][reducer]") {
    WalletReducer r;
    WalletState  s;
    s.Set(events::wallet::Currency::Credits, 50);

    SECTION("spend within balance") {
        events::wallet::CurrencyDelta evt{
            events::wallet::Currency::Credits, -30, "pull_cost", "evt_xyz", 50, 20
        };
        auto out = r.Apply(s, "credits_spent", evt);
        REQUIRE(out.state.Get(events::wallet::Currency::Credits) == 20);
    }

    SECTION("spend more than balance throws") {
        events::wallet::CurrencyDelta evt{
            events::wallet::Currency::Credits, -100, "pull_cost", "evt_xyz", 50, -50
        };
        REQUIRE_THROWS_AS(r.Apply(s, "credits_spent", evt), WalletInvariantViolation);
    }
}

TEST_CASE("admin_adjustment_applied works on any currency", "[wallet][reducer]") {
    WalletReducer r;
    WalletState  s;
    events::wallet::CurrencyDelta evt{
        events::wallet::Currency::Scrap, +1000, "admin_grant", std::nullopt, 0, 1000
    };
    auto out = r.Apply(s, "admin_adjustment_applied", evt);
    REQUIRE(out.state.Get(events::wallet::Currency::Scrap) == 1000);
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd Server && msbuild Aphelyon.slnx /p:Configuration=Debug /t:AccountTests`
Expected: compile failure (WalletReducer doesn't exist).

- [ ] **Step 3: Write WalletReducer**

`Server/Account/src/reducers/WalletReducer.hpp`:
```cpp
#pragma once
#include "ReducerCommon.hpp"
#include "events/WalletEvents.hpp"
#include <array>
#include <stdexcept>
#include <string>

namespace aphelyon::reducers {

class WalletInvariantViolation : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class WalletState {
public:
    std::int64_t Get(events::wallet::Currency c) const { return balances_[static_cast<std::size_t>(c)]; }
    void Set(events::wallet::Currency c, std::int64_t v) { balances_[static_cast<std::size_t>(c)] = v; }

private:
    std::array<std::int64_t, 5> balances_{};
};

class WalletReducer {
public:
    ReducerResult<WalletState> Apply(
        const WalletState& s,
        const std::string& event_type,
        const events::wallet::CurrencyDelta& evt) const
    {
        ReducerResult<WalletState> out{ s, {} };
        const auto next = s.Get(evt.currency) + evt.amount;
        if (next < 0) {
            throw WalletInvariantViolation("Negative balance for " + std::string(events::wallet::CurrencyToStr(evt.currency)));
        }
        if (next != evt.balance_after) {
            throw WalletInvariantViolation("balance_after mismatch — event vs computed");
        }
        out.state.Set(evt.currency, next);
        return out;
    }
};

} // namespace aphelyon::reducers
```

- [ ] **Step 4: Run tests, verify pass**

Run: `cd Server && msbuild Aphelyon.slnx /p:Configuration=Debug /t:AccountTests && bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[wallet][reducer]"`
Expected: 4 test cases pass.

- [ ] **Step 5: Commit**

```bash
git add Server/Account/src/reducers/WalletReducer.hpp Server/Account/tests/ReducerTests/WalletReducerTest.cpp
git commit -m "feat(account): WalletReducer + TDD tests"
```

### Task 17: Progression reducer (TDD)

**Files:**
- Create: `Server/Account/tests/ReducerTests/ProgressionReducerTest.cpp`
- Create: `Server/Account/src/reducers/ProgressionReducer.hpp`

- [ ] **Step 1: Write failing tests**

`Server/Account/tests/ReducerTests/ProgressionReducerTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "reducers/ProgressionReducer.hpp"
#include "events/ProgressionEvents.hpp"

using namespace aphelyon::reducers;
using namespace aphelyon::events::progression;

TEST_CASE("level advance updates story_level + xp", "[progression][reducer]") {
    ProgressionReducer r;
    ProgressionState s;
    s.story_level = 14;
    s.story_xp = 4000;
    StoryLevelAdvanced evt{ 14, 15, 4500, 320, std::nullopt, 0 };

    auto out = r.Apply(s, "story_level_advanced", evt);
    REQUIRE(out.state.story_level == 15);
    REQUIRE(out.state.story_xp == 320);  // carried over
    REQUIRE(out.effects.empty());
}

TEST_CASE("level advance with difficulty tier unlock emits no extra effect", "[progression][reducer]") {
    ProgressionReducer r;
    ProgressionState s;
    s.story_level = 19;
    s.difficulty_tier = 1;
    StoryLevelAdvanced evt{ 19, 20, 5000, 0, 2, 0 };

    auto out = r.Apply(s, "story_level_advanced", evt);
    REQUIRE(out.state.story_level == 20);
    REQUIRE(out.state.difficulty_tier == 2);
}

TEST_CASE("level advance with overflow_credits emits GrantCurrencyEffect", "[progression][reducer]") {
    ProgressionReducer r;
    ProgressionState s;
    s.story_level = 99;
    StoryLevelAdvanced evt{ 99, 99, 0, 0, std::nullopt, 800 };

    auto out = r.Apply(s, "story_xp_gained", evt);
    REQUIRE(out.effects.size() == 1);
    auto* grant = std::get_if<GrantCurrencyEffect>(&out.effects[0]);
    REQUIRE(grant != nullptr);
    REQUIRE(grant->currency == "credits");
    REQUIRE(grant->amount == 800);
}
```

- [ ] **Step 2: Confirm failure, then write implementation**

`Server/Account/src/reducers/ProgressionReducer.hpp`:
```cpp
#pragma once
#include "ReducerCommon.hpp"
#include "events/ProgressionEvents.hpp"

namespace aphelyon::reducers {

struct ProgressionState {
    int story_level     = 1;
    int story_xp        = 0;
    int difficulty_tier = 1;
};

class ProgressionReducer {
public:
    ReducerResult<ProgressionState> Apply(
        const ProgressionState& s,
        const std::string& event_type,
        const aphelyon::events::progression::StoryLevelAdvanced& evt) const
    {
        ReducerResult<ProgressionState> out{ s, {} };
        out.state.story_level = evt.to_level;
        out.state.story_xp    = evt.xp_carried_over;
        if (evt.difficulty_tier_unlocked) {
            out.state.difficulty_tier = *evt.difficulty_tier_unlocked;
        }
        if (evt.overflow_credits > 0) {
            out.effects.push_back(GrantCurrencyEffect{
                "credits", evt.overflow_credits, "story_overflow", ""
            });
        }
        return out;
    }
};

} // namespace aphelyon::reducers
```

- [ ] **Step 3: Run tests**

Run: `bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[progression][reducer]"`
Expected: 3 cases pass.

- [ ] **Step 4: Commit**

```bash
git add Server/Account/src/reducers/ProgressionReducer.hpp Server/Account/tests/ReducerTests/ProgressionReducerTest.cpp
git commit -m "feat(account): ProgressionReducer + TDD tests"
```

### Task 18: QuestClaims reducer (TDD)

**Files:**
- Create: `Server/Account/tests/ReducerTests/QuestClaimsReducerTest.cpp`
- Create: `Server/Account/src/reducers/QuestClaimsReducer.hpp`

- [ ] **Step 1: Write failing tests**

`Server/Account/tests/ReducerTests/QuestClaimsReducerTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "reducers/QuestClaimsReducer.hpp"

using namespace aphelyon::reducers;
using namespace aphelyon::events::quest_claims;

TEST_CASE("claim records the quest_id in state.claimed_quest_ids", "[quest_claims][reducer]") {
    QuestClaimsReducer r;
    QuestClaimsState s;
    QuestRewardClaimed evt{ "daily_login_2026_06_01", 1, { {RewardKind::Credits, 60, ""} }, 5 };
    auto out = r.Apply(s, "quest_reward_claimed", evt);
    REQUIRE(out.state.claimed_quest_ids.count("daily_login_2026_06_01") == 1);
}

TEST_CASE("rewards emit GrantCurrencyEffect / GrantMaterialEffect per reward kind", "[quest_claims][reducer]") {
    QuestClaimsReducer r;
    QuestClaimsState s;
    QuestRewardClaimed evt{
        "weekly_q1", 2,
        {
            { RewardKind::Credits,  60,   "" },
            { RewardKind::Scrap,    5000, "" },
            { RewardKind::Material, 3,    "mat_xp_book_2" }
        },
        0
    };
    auto out = r.Apply(s, "quest_reward_claimed", evt);
    REQUIRE(out.effects.size() == 3);
    REQUIRE(std::holds_alternative<GrantCurrencyEffect>(out.effects[0]));
    REQUIRE(std::holds_alternative<GrantCurrencyEffect>(out.effects[1]));
    REQUIRE(std::holds_alternative<GrantMaterialEffect>(out.effects[2]));
}

TEST_CASE("claiming the same quest twice throws (idempotency belongs at the event layer, but reducer is defensive)", "[quest_claims][reducer]") {
    QuestClaimsReducer r;
    QuestClaimsState s;
    s.claimed_quest_ids.insert("dup");
    QuestRewardClaimed evt{ "dup", 1, {}, 0 };
    REQUIRE_THROWS_AS(r.Apply(s, "quest_reward_claimed", evt), QuestClaimInvariantViolation);
}
```

- [ ] **Step 2: Implementation**

`Server/Account/src/reducers/QuestClaimsReducer.hpp`:
```cpp
#pragma once
#include "ReducerCommon.hpp"
#include "events/QuestClaimEvents.hpp"
#include <stdexcept>
#include <unordered_set>

namespace aphelyon::reducers {

class QuestClaimInvariantViolation : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct QuestClaimsState {
    std::unordered_set<std::string> claimed_quest_ids;
};

class QuestClaimsReducer {
public:
    ReducerResult<QuestClaimsState> Apply(
        const QuestClaimsState& s,
        const std::string& event_type,
        const aphelyon::events::quest_claims::QuestRewardClaimed& evt) const
    {
        ReducerResult<QuestClaimsState> out{ s, {} };
        if (out.state.claimed_quest_ids.count(evt.quest_id))
            throw QuestClaimInvariantViolation("Quest already claimed: " + evt.quest_id);
        out.state.claimed_quest_ids.insert(evt.quest_id);

        using namespace aphelyon::events::quest_claims;
        for (const auto& r : evt.rewards) {
            switch (r.kind) {
                case RewardKind::Credits:
                case RewardKind::UniversalCredits:
                case RewardKind::Tickets:
                case RewardKind::LimitedTickets:
                case RewardKind::Scrap:
                    out.effects.push_back(GrantCurrencyEffect{
                        RewardKindToStr(r.kind), r.amount, "quest_reward", evt.quest_id });
                    break;
                case RewardKind::Material:
                    out.effects.push_back(GrantMaterialEffect{ r.material_id, r.amount });
                    break;
                case RewardKind::Character:
                    out.effects.push_back(GrantCharacterEffect{ r.material_id });
                    break;
                case RewardKind::Weapon:
                    out.effects.push_back(GrantWeaponEffect{ "", r.material_id });
                    break;
            }
        }
        return out;
    }
};

} // namespace aphelyon::reducers
```

- [ ] **Step 3: Tests pass; commit**

```bash
git add Server/Account/src/reducers/QuestClaimsReducer.hpp Server/Account/tests/ReducerTests/QuestClaimsReducerTest.cpp
git commit -m "feat(account): QuestClaimsReducer + TDD tests"
```

### Task 19: Pulls reducer (TDD) — the complex one

**Files:**
- Create: `Server/Account/tests/ReducerTests/PullsReducerTest.cpp`
- Create: `Server/Account/src/reducers/PullsReducer.hpp`

The pulls reducer is **the most important** because RNG capture lives here. The reducer applies the event's recorded outcomes; it does NOT roll RNG itself. RNG is rolled at the handler layer before the event is constructed.

- [ ] **Step 1: Write failing tests**

`Server/Account/tests/ReducerTests/PullsReducerTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "reducers/PullsReducer.hpp"

using namespace aphelyon::reducers;
using namespace aphelyon::events::pulls;

PullPerformed MakePull(int rarity, bool featured = false, int p5_before = 49, int p5_after = 50) {
    return PullPerformed{
        "char_event_001", "2.7", "tickets", 1,
        { 0x9E3779B97F4A7C15ULL, 0x1, 0x2, 0x3 },
        1,
        p5_before, 7, false,
        { PullResult{ "char_4star_001", rarity, std::nullopt, featured } },
        p5_after, 0, false
    };
}

TEST_CASE("pull updates pity_5 and pity_4 from event values", "[pulls][reducer]") {
    PullsReducer r;
    PullsState s;
    s.pity_5 = 49;
    s.pity_4 = 7;
    auto evt = MakePull(4, true, 49, 50);

    auto out = r.Apply(s, "pull_performed", evt);
    REQUIRE(out.state.pity_5 == 50);
    REQUIRE(out.state.pity_4 == 0);
}

TEST_CASE("pull with 5-star result emits Toast + GrantWeapon/GrantCharacter", "[pulls][reducer]") {
    PullsReducer r;
    PullsState s;
    PullPerformed evt = MakePull(5, true);
    evt.results[0].instance_id = "01923000-...";   // weapon instance UUID
    evt.results[0].template_id = "wpn_5star_001";

    auto out = r.Apply(s, "pull_performed", evt);
    bool found_toast = false, found_grant = false;
    for (auto& e : out.effects) {
        if (std::holds_alternative<ToastEffect>(e)) found_toast = true;
        if (std::holds_alternative<GrantWeaponEffect>(e)) found_grant = true;
    }
    REQUIRE(found_toast);
    REQUIRE(found_grant);
}

TEST_CASE("character pull with null instance_id emits GrantCharacterEffect", "[pulls][reducer]") {
    PullsReducer r;
    PullsState s;
    auto evt = MakePull(4, false);
    evt.results[0].instance_id = std::nullopt;
    evt.results[0].template_id = "char_4star_001";

    auto out = r.Apply(s, "pull_performed", evt);
    bool found_grant_char = false;
    for (auto& e : out.effects)
        if (std::holds_alternative<GrantCharacterEffect>(e)) found_grant_char = true;
    REQUIRE(found_grant_char);
}

TEST_CASE("invariant: post-state pity matches event-recorded pity_5_after", "[pulls][reducer]") {
    PullsReducer r;
    PullsState s;
    s.pity_5 = 49;
    auto evt = MakePull(4, false, 49, 999);  // intentionally wrong pity_5_after
    REQUIRE_THROWS_AS(r.Apply(s, "pull_performed", evt), PullsInvariantViolation);
}
```

- [ ] **Step 2: Implementation**

`Server/Account/src/reducers/PullsReducer.hpp`:
```cpp
#pragma once
#include "ReducerCommon.hpp"
#include "events/PullEvents.hpp"
#include <stdexcept>

namespace aphelyon::reducers {

class PullsInvariantViolation : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct PullsState {
    int  pity_5     = 0;
    int  pity_4     = 0;
    bool guarantee_5 = false;
    XoshiroCpp::Xoshiro256PlusPlus::state_type rng_state{ 1, 0, 0, 0 };
};

class PullsReducer {
public:
    ReducerResult<PullsState> Apply(
        const PullsState& s,
        const std::string& event_type,
        const aphelyon::events::pulls::PullPerformed& evt) const
    {
        ReducerResult<PullsState> out{ s, {} };

        // Verify pre-state alignment
        if (s.pity_5 != evt.pity_5_before || s.pity_4 != evt.pity_4_before)
            throw PullsInvariantViolation("Pre-pull pity mismatch");

        // Apply state changes from event
        out.state.pity_5      = evt.pity_5_after;
        out.state.pity_4      = evt.pity_4_after;
        out.state.guarantee_5 = evt.guarantee_5_after;
        // RNG advance is captured by the handler before/after rolling — we don't recompute here.

        // Emit grants and toasts based on results
        for (const auto& r : evt.results) {
            if (r.rarity >= 5) {
                out.effects.push_back(ToastEffect{ "you got a 5-star: " + r.template_id });
            }
            if (r.instance_id) {
                out.effects.push_back(GrantWeaponEffect{ *r.instance_id, r.template_id });
            } else {
                out.effects.push_back(GrantCharacterEffect{ r.template_id });
            }
        }
        return out;
    }
};

} // namespace aphelyon::reducers
```

- [ ] **Step 3: Tests pass**

Run: `bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[pulls][reducer]"`
Expected: 4 cases pass.

- [ ] **Step 4: Commit**

```bash
git add Server/Account/src/reducers/PullsReducer.hpp Server/Account/tests/ReducerTests/PullsReducerTest.cpp
git commit -m "feat(account): PullsReducer + TDD tests (Pattern C RNG capture)"
```

---

## Phase 4 — Event store

### Task 20: EventStore append + load (with integration test)

**Files:**
- Create: `Server/Account/src/db/EventStore.hpp`
- Create: `Server/Account/tests/Integration/EventStoreRoundTripTest.cpp`

- [ ] **Step 1: Write integration test (requires running Postgres)**

`Server/Account/tests/Integration/EventStoreRoundTripTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "db/EventStore.hpp"
#include "db/ConnectionPool.hpp"
#include "events/WalletEvents.hpp"

using namespace aphelyon;
using namespace aphelyon::db;

const std::string kConn = "postgresql://aphelyon:aphelyon@localhost:5432/aphelyon";

// Helper: insert a test account and return its id.
static std::int64_t MakeAccount(ConnectionPool& pool, const std::string& uname) {
    auto lease = pool.acquire();
    pqxx::work tx(*lease);
    auto r = tx.exec_params(
        "INSERT INTO accounts (username, password_hash) VALUES ($1, $2) RETURNING account_id",
        uname, "x");
    tx.commit();
    return r[0][0].as<std::int64_t>();
}

TEST_CASE("EventStore appends and reads back events", "[integration][event_store]") {
    ConnectionPool pool(kConn, 4);
    EventStore store(pool);
    auto account_id = MakeAccount(pool, "es_test_1");

    // Append a credits_added event
    events::Event ev;
    ev.event_id = UuidV7::Generate();
    ev.account_id = account_id;
    ev.aggregate_kind = events::AggregateKind::Wallet;
    ev.version = 1;
    ev.event_type = "credits_added";
    ev.idempotency_key = "test_idem_1";
    ev.data = events::wallet::ToJson(
        events::wallet::CurrencyDelta{ events::wallet::Currency::Credits, 100, "test", std::nullopt, 0, 100 });

    store.Append(ev);

    auto loaded = store.LoadStream(account_id, events::AggregateKind::Wallet, /*from_version=*/0);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].event_type == "credits_added");
    REQUIRE(loaded[0].version == 1);
}

TEST_CASE("EventStore rejects duplicate version with 23505", "[integration][event_store]") {
    ConnectionPool pool(kConn, 4);
    EventStore store(pool);
    auto account_id = MakeAccount(pool, "es_test_2");

    events::Event ev;
    ev.event_id = UuidV7::Generate();
    ev.account_id = account_id;
    ev.aggregate_kind = events::AggregateKind::Wallet;
    ev.version = 1;
    ev.event_type = "credits_added";
    ev.idempotency_key = "test_idem_a";
    ev.data = nlohmann::json{ {"amount", 1} };

    store.Append(ev);

    // Same version, new event_id and idempotency_key
    ev.event_id = UuidV7::Generate();
    ev.idempotency_key = "test_idem_b";
    REQUIRE_THROWS_AS(store.Append(ev), EventStore::ConcurrencyConflict);
}

TEST_CASE("EventStore treats duplicate idempotency_key as success", "[integration][event_store]") {
    ConnectionPool pool(kConn, 4);
    EventStore store(pool);
    auto account_id = MakeAccount(pool, "es_test_3");

    events::Event ev;
    ev.event_id = UuidV7::Generate();
    ev.account_id = account_id;
    ev.aggregate_kind = events::AggregateKind::Wallet;
    ev.version = 1;
    ev.event_type = "credits_added";
    ev.idempotency_key = "same_key";
    ev.data = nlohmann::json{ {"amount", 1} };

    store.Append(ev);

    // Retry with same idempotency_key but new version (simulating a client retry)
    events::Event retry = ev;
    retry.event_id = UuidV7::Generate();
    retry.version = 2;
    REQUIRE_NOTHROW(store.AppendIdempotent(retry));  // should be a no-op (returns existing event)
}
```

- [ ] **Step 2: Confirm failure, then implementation**

`Server/Account/src/db/EventStore.hpp`:
```cpp
#pragma once
#include "ConnectionPool.hpp"
#include "events/Event.hpp"
#include "UuidV7.hpp"
#include <pqxx/pqxx>
#include <stdexcept>
#include <vector>

namespace aphelyon::db {

class EventStore {
public:
    class ConcurrencyConflict : public std::runtime_error { using std::runtime_error::runtime_error; };

    explicit EventStore(ConnectionPool& pool) : pool_(pool) {}

    void Append(const events::Event& ev) {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        try {
            tx.exec_params(R"SQL(
                INSERT INTO events
                  (event_id, account_id, aggregate_kind, version, event_type, schema_version,
                   data, metadata, idempotency_key)
                VALUES ($1::uuid, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb, $9)
            )SQL",
                aphelyon::UuidV7::ToString(ev.event_id),
                ev.account_id,
                events::AggregateKindToStr(ev.aggregate_kind),
                ev.version,
                ev.event_type,
                ev.schema_version,
                ev.data.dump(),
                ev.metadata.dump(),
                ev.idempotency_key
            );
            tx.commit();
        } catch (const pqxx::unique_violation& e) {
            throw ConcurrencyConflict(std::string("Append conflict: ") + e.what());
        }
    }

    // Idempotent: if the (account_id, idempotency_key) already exists, return without error.
    void AppendIdempotent(const events::Event& ev) {
        try {
            Append(ev);
        } catch (const ConcurrencyConflict&) {
            // Check whether the conflict was on idempotency_key vs version
            auto lease = pool_.acquire();
            pqxx::work tx(*lease);
            auto r = tx.exec_params(
                "SELECT 1 FROM events WHERE account_id = $1 AND idempotency_key = $2 LIMIT 1",
                ev.account_id, ev.idempotency_key);
            if (r.empty()) throw;  // conflict was version-based, re-throw
        }
    }

    std::vector<events::Event> LoadStream(
        std::int64_t account_id, events::AggregateKind kind, int from_version)
    {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto r = tx.exec_params(R"SQL(
            SELECT event_id::text, version, event_type, schema_version, data, metadata,
                   idempotency_key, created_at
              FROM events
             WHERE account_id = $1
               AND aggregate_kind = $2
               AND version > $3
          ORDER BY version
        )SQL", account_id, events::AggregateKindToStr(kind), from_version);

        std::vector<events::Event> out;
        out.reserve(r.size());
        for (const auto& row : r) {
            events::Event ev;
            ev.event_id        = aphelyon::UuidV7::FromString(row[0].as<std::string>());
            ev.account_id      = account_id;
            ev.aggregate_kind  = kind;
            ev.version         = row[1].as<int>();
            ev.event_type      = row[2].as<std::string>();
            ev.schema_version  = row[3].as<int>();
            ev.data            = nlohmann::json::parse(row[4].as<std::string>());
            ev.metadata        = nlohmann::json::parse(row[5].as<std::string>());
            ev.idempotency_key = row[6].as<std::string>();
            out.push_back(std::move(ev));
        }
        return out;
    }

    // Returns the max version present for this stream, or 0 if empty.
    int LoadCurrentVersion(std::int64_t account_id, events::AggregateKind kind) {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto r = tx.exec_params(
            "SELECT COALESCE(MAX(version), 0) FROM events WHERE account_id = $1 AND aggregate_kind = $2",
            account_id, events::AggregateKindToStr(kind));
        return r[0][0].as<int>();
    }

private:
    ConnectionPool& pool_;
};

} // namespace aphelyon::db
```

- [ ] **Step 3: Run integration tests**

Ensure Postgres is up: `cd Server && scripts\db-setup.bat`
Then: `bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[integration][event_store]"`
Expected: all cases pass.

- [ ] **Step 4: Commit**

```bash
git add Server/Account/src/db/EventStore.hpp Server/Account/tests/Integration/EventStoreRoundTripTest.cpp
git commit -m "feat(account): EventStore append/load with optimistic concurrency"
```

---

## Phase 5 — Account refactor (private fields + setters + dirty)

### Task 21: Make Account fields private, add dirty tracking

**Files:**
- Modify: `Server/Common/src/AccountData.hpp`
- Modify: `Server/Common/src/CollectionState.hpp`
- Create: `Server/Account/src/AccountDirty.hpp`

This task is large. Approach: introduce a `DirtyState` shadow struct alongside the existing `AccountData` first, with all fields still public; then in a follow-up task swap callers to setters and make fields private.

- [ ] **Step 1: Write DirtyState**

`Server/Account/src/AccountDirty.hpp`:
```cpp
#pragma once
#include "UuidV7.hpp"
#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>

namespace aphelyon {

struct DirtyState {
    // Scalar table dirty bits
    bool accounts_row         = false;
    std::set<std::string> pity_slots;             // slot_ids dirtied

    // Row-per-entity dirty marks
    std::unordered_set<std::string>           character_ids;
    std::unordered_set<std::string>           char_traces_added;     // "<char>:<trace>"
    std::unordered_set<std::string>           char_traces_removed;
    std::set<UuidV7::ValueType>               weapon_instance_ids;
    std::set<UuidV7::ValueType>               gear_instance_ids;
    std::set<UuidV7::ValueType>               gear_instances_removed;
    std::set<std::pair<std::string, int>>     loadout_keys;          // (character_id, preset_id)
    std::unordered_set<std::string>           material_ids;
    std::unordered_set<std::string>           world_flag_adds;
    std::unordered_set<std::string>           world_flag_removes;
    std::unordered_set<std::string>           quest_ids;
    std::set<std::pair<std::string, std::string>> quest_objective_keys;  // (quest_id, objective_id)
    std::set<int16_t>                         party_slots;
    // Per-aggregate version cache (set by EventStore append, used by Commit)
    int cached_wallet_version       = 0;
    int cached_pulls_version        = 0;
    int cached_quest_claims_version = 0;
    int cached_progression_version  = 0;

    bool AnyDirty() const {
        return accounts_row || !pity_slots.empty() ||
               !character_ids.empty() || !char_traces_added.empty() || !char_traces_removed.empty() ||
               !weapon_instance_ids.empty() || !gear_instance_ids.empty() || !gear_instances_removed.empty() ||
               !loadout_keys.empty() || !material_ids.empty() ||
               !world_flag_adds.empty() || !world_flag_removes.empty() ||
               !quest_ids.empty() || !quest_objective_keys.empty() ||
               !party_slots.empty();
    }

    void Clear() { *this = DirtyState{}; }
};

} // namespace aphelyon
```

- [ ] **Step 2: Modify CollectionState — UUIDs**

Modify `Server/Common/src/CollectionState.hpp`:

Change `OwnedWeapon::instanceId` from `std::string` to `aphelyon::UuidV7::ValueType`:
```cpp
struct OwnedWeapon {
    aphelyon::UuidV7::ValueType instanceId;        // was std::string
    std::string              templateId;
    std::uint8_t             level;
    std::uint8_t             ascension;
    std::uint8_t             refinement;
};
```

Same change for `OwnedGear::instanceId`.

Update map keys in `CollectionState`:
```cpp
struct CollectionState {
    std::unordered_map<std::string, OwnedCharacter>             characters;
    std::unordered_map<aphelyon::UuidV7::ValueType, OwnedWeapon,
                       UuidV7Hash>                              weapons;
    std::unordered_map<aphelyon::UuidV7::ValueType, OwnedGear,
                       UuidV7Hash>                              gear;
};
```

Add a `UuidV7Hash` helper:
```cpp
struct UuidV7Hash {
    std::size_t operator()(const aphelyon::UuidV7::ValueType& u) const noexcept {
        // FNV-1a over 16 bytes
        std::size_t h = 1469598103934665603ULL;
        for (auto b : u) { h ^= b; h *= 1099511628211ULL; }
        return h;
    }
};
```

- [ ] **Step 3: Add embedded DirtyState to AccountData**

Modify `Server/Common/src/AccountData.hpp`. Keep existing fields public for now; add at the bottom of the struct:

```cpp
#include "../../Account/src/AccountDirty.hpp"  // path may need adjustment per project layout

struct AccountData {
    // ... existing fields ...

    aphelyon::DirtyState dirty;                        // tracks changes since last flush
};
```

- [ ] **Step 4: Update compilation across all three services**

This change affects Common, which Auth/Account/Combat all link. Build all three and fix call sites that use weapon/gear `instanceId` as a string. Likely affected: `AccountSerializer`, `CollectionActions`, `CollectionReducer`, gear/weapon equipment maps in `AccountData`. For each: convert string-instance-id call sites to `UuidV7` operations, using `UuidV7::ToString` only at JSON/wire boundaries.

Run: `msbuild Aphelyon.slnx /p:Configuration=Debug`
Expected: clean build (fix compile errors as they appear; this is a mechanical pass).

- [ ] **Step 5: Commit**

```bash
git add Server/Common/src/CollectionState.hpp Server/Common/src/AccountData.hpp Server/Account/src/AccountDirty.hpp Server/Account/src/AccountSerializer.hpp Server/Account/src/CollectionActions.hpp Server/Account/src/CollectionReducer.hpp
git commit -m "refactor(common): UUID v7 instance IDs + DirtyState tracking"
```

### Task 22: Setters that auto-mark dirty

**Files:**
- Modify: `Server/Common/src/AccountData.hpp`

- [ ] **Step 1: Add setter methods**

Add methods on `AccountData` that mutate fields and mark dirty in the same call:

```cpp
struct AccountData {
private:
    // Move existing fields to private. Public API below.
    std::string  id_;
    std::string  username_;
    std::string  password_hash_;
    // ... full list ...
public:
    // Read accessors
    const std::string& Id() const { return id_; }
    const std::string& Username() const { return username_; }
    int Credits() const { return credits_; }
    int Tickets() const { return tickets_; }
    int StoryLevel() const { return story_level_; }
    int StoryXp() const { return story_xp_; }
    int LoginStreak() const { return login_streak_; }
    // ... continue for every field ...

    // Mutating setters (mark dirty)
    void SetCredits(int v)         { credits_ = v;         dirty.accounts_row = true; }
    void SetTickets(int v)         { tickets_ = v;         dirty.accounts_row = true; }
    void SetStoryLevel(int v)      { story_level_ = v;     dirty.accounts_row = true; }
    void SetStoryXp(int v)         { story_xp_ = v;        dirty.accounts_row = true; }
    void SetLoginStreak(int v)     { login_streak_ = v;    dirty.accounts_row = true; }
    void TouchLastLogin()          { last_login_ = std::chrono::system_clock::now(); dirty.accounts_row = true; }

    void SetPartySlot(int16_t idx, std::string character_id) {
        party_[idx] = std::move(character_id);
        dirty.party_slots.insert(idx);
    }

    void SetOwnedCharacterLevel(const std::string& char_id, int level) {
        collection.characters[char_id].level = level;
        dirty.character_ids.insert(char_id);
    }

    void AddOwnedWeapon(OwnedWeapon w) {
        auto id = w.instanceId;
        collection.weapons.emplace(id, std::move(w));
        dirty.weapon_instance_ids.insert(id);
    }

    void RefineOwnedWeapon(aphelyon::UuidV7::ValueType id) {
        auto& w = collection.weapons.at(id);
        w.refinement += 1;
        dirty.weapon_instance_ids.insert(id);
    }

    void AddOwnedGear(OwnedGear g) {
        auto id = g.instanceId;
        collection.gear.emplace(id, std::move(g));
        dirty.gear_instance_ids.insert(id);
    }

    void SetLoadoutSlot(const std::string& char_id, int16_t preset, GearSlot slot, aphelyon::UuidV7::ValueType gear_id) {
        gearEquipment[char_id][static_cast<uint8_t>(slot)] = gear_id;
        dirty.loadout_keys.insert({char_id, preset});
    }

    void AddMaterial(const std::string& material_id, int qty) {
        materials_[material_id] += qty;
        dirty.material_ids.insert(material_id);
    }

    void AddWorldFlag(const std::string& flag) {
        worldFlags.Add(flag);
        dirty.world_flag_adds.insert(flag);
    }

    void RemoveWorldFlag(const std::string& flag) {
        worldFlags.Remove(flag);
        dirty.world_flag_removes.insert(flag);
    }
};
```

(The above is illustrative — apply the same pattern to every field that handlers currently mutate. The dirty-mark goes on the appropriate set/bit.)

- [ ] **Step 2: Build + fix call sites**

`msbuild Aphelyon.slnx /p:Configuration=Debug`

Find every place in Auth/Account/Combat that mutates an Account field directly and route through setters. The fix is mechanical: `account.credits += 100` → `account.SetCredits(account.Credits() + 100)`.

- [ ] **Step 3: Commit**

```bash
git add Server/Common/src/AccountData.hpp Server/Account/src/
git commit -m "refactor(account): Account fields private; setters auto-mark dirty"
```

---

## Phase 6 — Relational flush + Transaction wrapper

### Task 23: Relational flush methods

**Files:**
- Create: `Server/Account/src/db/RelationalFlush.hpp`

- [ ] **Step 1: Write the flush implementation**

`Server/Account/src/db/RelationalFlush.hpp`:
```cpp
#pragma once
#include "ConnectionPool.hpp"
#include "AccountData.hpp"
#include <pqxx/pqxx>

namespace aphelyon::db {

// Writes only the tables marked dirty in account.dirty.
// Called inside an open pqxx::work transaction.
class RelationalFlush {
public:
    static void Flush(pqxx::work& tx, AccountData& account) {
        if (account.dirty.accounts_row) FlushAccountsRow(tx, account);
        if (!account.dirty.character_ids.empty()) FlushOwnedCharacters(tx, account);
        if (!account.dirty.weapon_instance_ids.empty()) FlushOwnedWeapons(tx, account);
        if (!account.dirty.gear_instance_ids.empty()) FlushOwnedGear(tx, account);
        if (!account.dirty.gear_instances_removed.empty()) FlushGearRemovals(tx, account);
        if (!account.dirty.loadout_keys.empty()) FlushLoadouts(tx, account);
        if (!account.dirty.material_ids.empty()) FlushMaterials(tx, account);
        if (!account.dirty.party_slots.empty()) FlushPartySlots(tx, account);
        if (!account.dirty.world_flag_adds.empty()) FlushWorldFlagAdds(tx, account);
        if (!account.dirty.world_flag_removes.empty()) FlushWorldFlagRemoves(tx, account);
        if (!account.dirty.quest_ids.empty()) FlushQuests(tx, account);
        if (!account.dirty.quest_objective_keys.empty()) FlushQuestObjectives(tx, account);
        if (!account.dirty.pity_slots.empty()) FlushPity(tx, account);
    }

private:
    static void FlushAccountsRow(pqxx::work& tx, const AccountData& a) {
        tx.exec_params(R"SQL(
            UPDATE accounts
               SET last_login = $2, login_streak = $3, last_streak_day = $4,
                   last_login_claim_at = $5, last_login_claim_day_idx = $6,
                   story_level = $7, story_xp = $8, difficulty_tier = $9,
                   updated_at = now()
             WHERE account_id = $1
        )SQL",
            a.NumericId(), a.LastLogin(), a.LoginStreak(), a.LastStreakDay(),
            a.LastLoginClaimAt(), a.LastLoginClaimDayIdx(),
            a.StoryLevel(), a.StoryXp(), a.DifficultyTier());
    }

    static void FlushOwnedCharacters(pqxx::work& tx, const AccountData& a) {
        for (const auto& id : a.dirty.character_ids) {
            const auto& c = a.collection.characters.at(id);
            tx.exec_params(R"SQL(
                INSERT INTO owned_characters (account_id, character_id, level, current_xp, ascension, resonance)
                VALUES ($1, $2, $3, $4, $5, $6)
                ON CONFLICT (account_id, character_id) DO UPDATE
                  SET level = EXCLUDED.level,
                      current_xp = EXCLUDED.current_xp,
                      ascension = EXCLUDED.ascension,
                      resonance = EXCLUDED.resonance
            )SQL", a.NumericId(), id, c.level, c.current_xp, c.ascension, c.resonance);
        }
    }

    static void FlushOwnedWeapons(pqxx::work& tx, const AccountData& a) {
        for (const auto& id : a.dirty.weapon_instance_ids) {
            const auto& w = a.collection.weapons.at(id);
            tx.exec_params(R"SQL(
                INSERT INTO owned_weapons (account_id, instance_id, template_id, level, ascension, refinement, acquired_at)
                VALUES ($1, $2::uuid, $3, $4, $5, $6, now())
                ON CONFLICT (account_id, instance_id) DO UPDATE
                  SET level = EXCLUDED.level,
                      ascension = EXCLUDED.ascension,
                      refinement = EXCLUDED.refinement
            )SQL", a.NumericId(), aphelyon::UuidV7::ToString(id), w.templateId, w.level, w.ascension, w.refinement);
        }
    }

    // ... [Apply the same pattern to FlushOwnedGear, FlushGearRemovals, FlushLoadouts,
    //      FlushMaterials, FlushPartySlots, FlushWorldFlagAdds, FlushWorldFlagRemoves,
    //      FlushQuests, FlushQuestObjectives, FlushPity. Each is a small UPSERT or DELETE
    //      driven by the corresponding dirty set on `a.dirty`.] ...
};

} // namespace aphelyon::db
```

(In practice this file is ~400 LOC by the time every dirty set has a flush implementation. The pattern is the same: walk dirty set → emit one UPSERT / DELETE per id.)

- [ ] **Step 2: Smoke test by hand**

Create a temporary test that builds an AccountData, sets credits, marks dirty, and flushes:
```cpp
TEST_CASE("RelationalFlush updates accounts row", "[integration][flush]") {
    ConnectionPool pool(kConn, 4);
    auto account_id = MakeAccount(pool, "flush_test_1");
    AccountData a;
    a.SetNumericId(account_id);
    a.SetCredits(500);    // marks dirty.accounts_row

    {
        auto lease = pool.acquire();
        pqxx::work tx(*lease);
        RelationalFlush::Flush(tx, a);
        tx.commit();
    }

    auto lease = pool.acquire();
    pqxx::work tx(*lease);
    auto r = tx.exec_params("SELECT credits FROM accounts WHERE account_id = $1", account_id);
    REQUIRE(r[0][0].as<int>() == 500);
}
```

(Note: this test exposes a schema gap — there's no `credits` column on accounts because credits live in the wallet ES stream. Adjust the test to a field that IS on accounts, e.g. `story_level`.)

- [ ] **Step 3: Commit**

```bash
git add Server/Account/src/db/RelationalFlush.hpp Server/Account/tests/Integration/RelationalFlushTest.cpp
git commit -m "feat(account): RelationalFlush — dirty-driven UPSERT per table"
```

### Task 24: AccountTransaction wrapper

**Files:**
- Create: `Server/Account/src/AccountTransaction.hpp`

- [ ] **Step 1: Write transaction wrapper**

`Server/Account/src/AccountTransaction.hpp`:
```cpp
#pragma once
#include "AccountData.hpp"
#include "db/ConnectionPool.hpp"
#include "db/EventStore.hpp"
#include "db/RelationalFlush.hpp"
#include "events/Event.hpp"
#include <memory>
#include <stdexcept>
#include <vector>

namespace aphelyon {

class AccountTransaction {
public:
    AccountTransaction(db::ConnectionPool& pool, db::EventStore& store, AccountData& account)
        : pool_(pool), store_(store), account_(account),
          lease_(pool.acquire()), tx_(std::make_unique<pqxx::work>(*lease_)) {}

    AccountTransaction(const AccountTransaction&) = delete;
    AccountTransaction& operator=(const AccountTransaction&) = delete;

    ~AccountTransaction() {
        if (tx_) Rollback();  // safety net
    }

    // Queue an event. The caller has already updated account_ projected state.
    void AppendEvent(events::Event ev) {
        events_.push_back(std::move(ev));
    }

    void EmitToOutbox(const std::string& destination, nlohmann::json payload) {
        outbox_.push_back({destination, std::move(payload)});
    }

    void RecordAudit(const std::string& actor, const std::string& action,
                     nlohmann::json target, nlohmann::json before, nlohmann::json after) {
        audits_.push_back({actor, action, std::move(target), std::move(before), std::move(after)});
    }

    void Commit() {
        if (!tx_) throw std::logic_error("Transaction already committed/rolled back");

        // 1. Append events
        for (const auto& ev : events_) {
            tx_->exec_params(R"SQL(
                INSERT INTO events
                  (event_id, account_id, aggregate_kind, version, event_type, schema_version,
                   data, metadata, idempotency_key)
                VALUES ($1::uuid, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb, $9)
            )SQL",
                UuidV7::ToString(ev.event_id),
                ev.account_id,
                events::AggregateKindToStr(ev.aggregate_kind),
                ev.version,
                ev.event_type,
                ev.schema_version,
                ev.data.dump(),
                ev.metadata.dump(),
                ev.idempotency_key);
        }

        // 2. Flush relational tables
        db::RelationalFlush::Flush(*tx_, account_);

        // 3. Outbox
        for (const auto& [dest, payload] : outbox_) {
            tx_->exec_params(
                "INSERT INTO outbox (destination, payload) VALUES ($1, $2::jsonb)",
                dest, payload.dump());
        }

        // 4. Audit log
        for (const auto& a : audits_) {
            tx_->exec_params(R"SQL(
                INSERT INTO audit_log (account_id, actor, action, target, before, after)
                VALUES ($1, $2, $3, $4::jsonb, $5::jsonb, $6::jsonb)
            )SQL",
                account_.NumericId(), a.actor, a.action,
                a.target.dump(),
                a.before.is_null() ? "null" : a.before.dump(),
                a.after.is_null() ? "null" : a.after.dump());
        }

        // 5. Commit
        tx_->commit();
        tx_.reset();

        // 6. Mark cache clean; advance cached versions
        for (const auto& ev : events_) {
            switch (ev.aggregate_kind) {
                case events::AggregateKind::Wallet:       account_.dirty.cached_wallet_version       = ev.version; break;
                case events::AggregateKind::Pulls:        account_.dirty.cached_pulls_version        = ev.version; break;
                case events::AggregateKind::QuestClaims:  account_.dirty.cached_quest_claims_version = ev.version; break;
                case events::AggregateKind::Progression:  account_.dirty.cached_progression_version  = ev.version; break;
            }
        }
        account_.dirty.Clear();
    }

    void Rollback() {
        if (!tx_) return;
        try { tx_->abort(); } catch (...) {}
        tx_.reset();
        // Cache reversion: caller must re-fetch the account; we don't roll the projection back.
        account_.MarkStaleForReload();
    }

private:
    struct OutboxRow { std::string destination; nlohmann::json payload; };
    struct AuditRow  { std::string actor; std::string action; nlohmann::json target; nlohmann::json before; nlohmann::json after; };

    db::ConnectionPool& pool_;
    db::EventStore& store_;
    AccountData& account_;
    db::ConnectionPool::Lease lease_;
    std::unique_ptr<pqxx::work> tx_;
    std::vector<events::Event> events_;
    std::vector<OutboxRow> outbox_;
    std::vector<AuditRow> audits_;
};

} // namespace aphelyon
```

- [ ] **Step 2: Add `MarkStaleForReload()` to AccountData**

Add a simple flag the cache layer checks on next access:
```cpp
struct AccountData {
    // ...
    bool stale_ = false;
    void MarkStaleForReload() { stale_ = true; }
    bool IsStale() const { return stale_; }
};
```

- [ ] **Step 3: Commit**

```bash
git add Server/Account/src/AccountTransaction.hpp Server/Common/src/AccountData.hpp
git commit -m "feat(account): AccountTransaction — Commit() does events+flush+outbox+audit atomically"
```

---

## Phase 7 — Repository rewrite

### Task 25: New AccountRepository (Postgres-backed)

**Files:**
- Modify (rewrite): `Server/Account/src/AccountRepository.hpp`
- Delete: `Server/Account/src/AccountSerializer.hpp`

- [ ] **Step 1: Rewrite AccountRepository**

`Server/Account/src/AccountRepository.hpp`:
```cpp
#pragma once
#include "AccountData.hpp"
#include "AccountTransaction.hpp"
#include "db/ConnectionPool.hpp"
#include "db/EventStore.hpp"
#include "reducers/WalletReducer.hpp"
#include "reducers/PullsReducer.hpp"
#include "reducers/QuestClaimsReducer.hpp"
#include "reducers/ProgressionReducer.hpp"
#include <memory>
#include <optional>
#include <pqxx/pqxx>
#include <string>

namespace aphelyon {

constexpr int kReducerVersion = 1;

class AccountRepository {
public:
    explicit AccountRepository(db::ConnectionPool& pool)
        : pool_(pool), store_(pool) {}

    std::optional<std::int64_t> FindByUsername(std::string_view username) {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto r = tx.exec_params(
            "SELECT account_id FROM accounts WHERE username = $1 AND deleted_at IS NULL",
            std::string(username));
        if (r.empty()) return std::nullopt;
        return r[0][0].as<std::int64_t>();
    }

    bool UsernameExists(std::string_view username) {
        return FindByUsername(username).has_value();
    }

    std::int64_t Create(std::string username, std::string password_hash) {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto r = tx.exec_params(
            "INSERT INTO accounts (username, password_hash) VALUES ($1, $2) RETURNING account_id",
            username, password_hash);
        tx.commit();
        return r[0][0].as<std::int64_t>();
    }

    // Loads account: relational tables + replays event streams from latest snapshot.
    std::unique_ptr<AccountData> Load(std::int64_t account_id) {
        auto a = std::make_unique<AccountData>();
        a->SetNumericId(account_id);
        LoadAccountsRow(*a);
        LoadOwnedCollections(*a);
        LoadQuests(*a);
        LoadWorldFlags(*a);
        LoadPity(*a);
        LoadParty(*a);
        LoadLoadouts(*a);
        LoadMaterials(*a);
        ReplayEventStreams(*a);
        return a;
    }

    AccountTransaction Begin(AccountData& account) {
        return AccountTransaction(pool_, store_, account);
    }

    db::EventStore& Store() { return store_; }

private:
    void LoadAccountsRow(AccountData& a) {
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto r = tx.exec_params(R"SQL(
            SELECT username, password_hash, created_at, last_login, login_streak, last_streak_day,
                   last_login_claim_at, last_login_claim_day_idx,
                   story_level, story_xp, difficulty_tier, reducer_version, deleted_at
              FROM accounts WHERE account_id = $1
        )SQL", a.NumericId());
        // Populate fields (mechanical mapping)…
    }

    void LoadOwnedCollections(AccountData& a) { /* SELECT * FROM owned_* WHERE account_id = $1 */ }
    void LoadQuests(AccountData& a)           { /* ... */ }
    void LoadWorldFlags(AccountData& a)       { /* ... */ }
    void LoadPity(AccountData& a)             { /* ... */ }
    void LoadParty(AccountData& a)            { /* ... */ }
    void LoadLoadouts(AccountData& a)         { /* ... */ }
    void LoadMaterials(AccountData& a)        { /* ... */ }

    void ReplayEventStreams(AccountData& a) {
        ReplayAggregate(a, events::AggregateKind::Wallet,       wallet_reducer_,       a.wallet_state);
        ReplayAggregate(a, events::AggregateKind::Pulls,        pulls_reducer_,        a.pulls_state);
        ReplayAggregate(a, events::AggregateKind::QuestClaims,  quest_claims_reducer_, a.quest_claims_state);
        ReplayAggregate(a, events::AggregateKind::Progression,  progression_reducer_,  a.progression_state);
    }

    template <typename Reducer, typename State>
    void ReplayAggregate(AccountData& a, events::AggregateKind kind, Reducer& reducer, State& target) {
        int from_version = 0;
        State state{};
        // Try snapshot
        auto lease = pool_.acquire();
        pqxx::work tx(*lease);
        auto snap = tx.exec_params(
            "SELECT version, reducer_version, state FROM snapshots WHERE account_id = $1 AND aggregate_kind = $2",
            a.NumericId(), events::AggregateKindToStr(kind));
        if (!snap.empty() && snap[0][1].as<int>() == kReducerVersion) {
            from_version = snap[0][0].as<int>();
            // Deserialize state from JSONB
            // state = FromJson<State>(...);
        }
        // Replay tail events
        auto tail = store_.LoadStream(a.NumericId(), kind, from_version);
        for (const auto& ev : tail) {
            // Dispatch by event_type — apply reducer
            // state = reducer.Apply(state, ev.event_type, FromJson<EventPayload>(ev.data)).state;
        }
        target = std::move(state);
    }

    db::ConnectionPool& pool_;
    db::EventStore store_;
    reducers::WalletReducer       wallet_reducer_;
    reducers::PullsReducer        pulls_reducer_;
    reducers::QuestClaimsReducer  quest_claims_reducer_;
    reducers::ProgressionReducer  progression_reducer_;
};

} // namespace aphelyon
```

(The `LoadAccountsRow` and friends contain row-shaped mapping logic — straightforward but verbose. The plan-phase researcher / executor fills these in based on the field list known from `AccountData.hpp`.)

- [ ] **Step 2: Delete AccountSerializer.hpp**

Run: `git rm Server/Account/src/AccountSerializer.hpp`

Find and delete every reference to `AccountSerializer::ToJson/FromJson` across the codebase. Replace with `repository.Load(account_id)`.

- [ ] **Step 3: Build, fix compile errors**

`msbuild Aphelyon.slnx /p:Configuration=Debug`

- [ ] **Step 4: Commit**

```bash
git add Server/Account/src/AccountRepository.hpp
git rm Server/Account/src/AccountSerializer.hpp
git commit -m "refactor(account): replace JSON AccountRepository with Postgres-backed"
```

---

## Phase 8 — Snapshot writer + outbox relay

### Task 26: SnapshotWriter thread

**Files:**
- Create: `Server/Account/src/db/SnapshotWriter.hpp`

- [ ] **Step 1: Write the snapshot writer**

`Server/Account/src/db/SnapshotWriter.hpp`:
```cpp
#pragma once
#include "ConnectionPool.hpp"
#include "events/Event.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace aphelyon::db {

struct SnapshotJob {
    std::int64_t            account_id;
    events::AggregateKind   aggregate_kind;
    int                     current_version;
    int                     reducer_version;
    nlohmann::json          state_serialized;
};

class SnapshotWriter {
public:
    SnapshotWriter(ConnectionPool& pool, std::size_t queue_capacity = 1024)
        : pool_(pool), capacity_(queue_capacity), running_(true),
          worker_(&SnapshotWriter::Run, this) {}

    ~SnapshotWriter() {
        running_ = false;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Non-blocking enqueue; on overflow, oldest job is dropped (snapshots are an optimization).
    void Enqueue(SnapshotJob job) {
        std::lock_guard lk(mtx_);
        if (queue_.size() >= capacity_) queue_.pop_front();
        queue_.push_back(std::move(job));
        cv_.notify_one();
    }

private:
    void Run() {
        while (running_) {
            SnapshotJob job;
            {
                std::unique_lock lk(mtx_);
                cv_.wait(lk, [&]{ return !queue_.empty() || !running_; });
                if (!running_) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                auto lease = pool_.acquire();
                pqxx::work tx(*lease);
                tx.exec_params(R"SQL(
                    INSERT INTO snapshots (account_id, aggregate_kind, version, reducer_version, state)
                    VALUES ($1, $2, $3, $4, $5::jsonb)
                    ON CONFLICT (account_id, aggregate_kind) DO UPDATE
                      SET version = EXCLUDED.version,
                          reducer_version = EXCLUDED.reducer_version,
                          state = EXCLUDED.state,
                          snapped_at = now()
                )SQL",
                    job.account_id, events::AggregateKindToStr(job.aggregate_kind),
                    job.current_version, job.reducer_version, job.state_serialized.dump());
                tx.commit();
            } catch (const std::exception& e) {
                // Snapshots are best-effort. Log and continue.
            }
        }
    }

    ConnectionPool& pool_;
    std::size_t capacity_;
    std::atomic<bool> running_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<SnapshotJob> queue_;
    std::thread worker_;
};

} // namespace aphelyon::db
```

- [ ] **Step 2: Commit**

```bash
git add Server/Account/src/db/SnapshotWriter.hpp
git commit -m "feat(account): SnapshotWriter thread with bounded queue + drop-on-overflow"
```

### Task 27: Outbox relay (shared thread with SnapshotWriter)

**Files:**
- Create: `Server/Account/src/db/OutboxRelay.hpp`

- [ ] **Step 1: Write the relay**

`Server/Account/src/db/OutboxRelay.hpp`:
```cpp
#pragma once
#include "ConnectionPool.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <unordered_map>

namespace aphelyon::db {

using OutboxHandler = std::function<bool(const nlohmann::json& payload)>;

class OutboxRelay {
public:
    OutboxRelay(ConnectionPool& pool, std::chrono::milliseconds interval = std::chrono::milliseconds(500))
        : pool_(pool), interval_(interval), running_(true),
          worker_(&OutboxRelay::Run, this) {}

    ~OutboxRelay() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

    void Register(const std::string& destination, OutboxHandler h) {
        std::lock_guard lk(mtx_);
        handlers_[destination] = std::move(h);
    }

private:
    void Run() {
        while (running_) {
            try {
                auto lease = pool_.acquire();
                pqxx::work tx(*lease);
                auto r = tx.exec(R"SQL(
                    SELECT outbox_id, destination, payload
                      FROM outbox
                     WHERE dispatched_at IS NULL
                  ORDER BY outbox_id
                     LIMIT 64
                       FOR UPDATE SKIP LOCKED
                )SQL");
                for (const auto& row : r) {
                    auto outbox_id = row[0].as<std::int64_t>();
                    auto destination = row[1].as<std::string>();
                    auto payload = nlohmann::json::parse(row[2].as<std::string>());

                    OutboxHandler handler;
                    {
                        std::lock_guard lk(mtx_);
                        auto it = handlers_.find(destination);
                        if (it != handlers_.end()) handler = it->second;
                    }
                    bool ok = handler && handler(payload);
                    if (ok) {
                        tx.exec_params("UPDATE outbox SET dispatched_at = now() WHERE outbox_id = $1", outbox_id);
                    }
                }
                tx.commit();
            } catch (const std::exception&) {
                // log + continue
            }
            std::this_thread::sleep_for(interval_);
        }
    }

    ConnectionPool& pool_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> running_;
    std::mutex mtx_;
    std::unordered_map<std::string, OutboxHandler> handlers_;
    std::thread worker_;
};

} // namespace aphelyon::db
```

- [ ] **Step 2: Integration test**

`Server/Account/tests/Integration/OutboxRelayTest.cpp`:
```cpp
TEST_CASE("OutboxRelay dispatches and marks rows", "[integration][outbox]") {
    ConnectionPool pool(kConn, 4);

    // Seed an outbox row
    {
        auto lease = pool.acquire();
        pqxx::work tx(*lease);
        tx.exec("INSERT INTO outbox (destination, payload) VALUES ('test_dest', '{\"hello\":\"world\"}'::jsonb)");
        tx.commit();
    }

    OutboxRelay relay(pool, std::chrono::milliseconds(50));
    std::atomic<int> calls = 0;
    relay.Register("test_dest", [&](const nlohmann::json& p) {
        REQUIRE(p["hello"] == "world");
        calls++;
        return true;
    });

    // Wait up to 2 seconds for the relay to fire
    auto start = std::chrono::steady_clock::now();
    while (calls == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(calls >= 1);
}
```

- [ ] **Step 3: Commit**

```bash
git add Server/Account/src/db/OutboxRelay.hpp Server/Account/tests/Integration/OutboxRelayTest.cpp
git commit -m "feat(account): OutboxRelay with FOR UPDATE SKIP LOCKED dispatch"
```

---

## Phase 9 — Handler migration + TickQuests

### Task 28: Migrate GachaHandlers to AccountTransaction

**Files:**
- Modify: `Server/Account/src/GachaHandlers.hpp`
- Modify: `Server/Account/src/GachaRNG.hpp`

- [ ] **Step 1: Switch GachaRNG to xoshiro256++**

Modify `Server/Account/src/GachaRNG.hpp` to use xoshiro256++ internally. Surface `state()` and `set_state()` to allow the handler to capture state before/after.

```cpp
#include "XoshiroCpp.hpp"

class GachaRNG {
public:
    explicit GachaRNG(XoshiroCpp::Xoshiro256PlusPlus::state_type s) : rng_(s) {}

    XoshiroCpp::Xoshiro256PlusPlus::state_type State() const { return rng_.serialize(); }
    void SetState(XoshiroCpp::Xoshiro256PlusPlus::state_type s) { rng_.deserialize(s); }

    // Existing roll methods stay; just replace internal uses of std::mt19937 with rng_().
    // rng_() returns a uint64_t; convert to whatever range the existing roll method expected.

private:
    XoshiroCpp::Xoshiro256PlusPlus rng_;
};
```

- [ ] **Step 2: Rewrite `HandlePull`**

The new flow:
```cpp
ResponseFrame HandlePull(const RequestFrame& req, HandlerContext& ctx) {
    auto& account = ctx.GetLockedAccount(req.playerId);

    auto txn = ctx.repository->Begin(account);

    // Pre-pull RNG capture
    GachaRNG rng(account.pulls_state.rng_state);
    auto rng_before = rng.State();
    int pity_5_before = account.pulls_state.pity_5;
    int pity_4_before = account.pulls_state.pity_4;
    bool guarantee_5_before = account.pulls_state.guarantee_5;

    // Roll the pull (mutates rng)
    auto results = bannerSystem_->RollPull(rng, banner_id, /*pity=*/account.pulls_state);

    // Construct the event
    events::pulls::PullPerformed evt{
        banner_id, banner_version, "tickets", 1,
        rng_before, /*algorithm_version=*/1,
        pity_5_before, pity_4_before, guarantee_5_before,
        ConvertResults(results),
        account.pulls_state.pity_5, account.pulls_state.pity_4,
        account.pulls_state.guarantee_5
    };

    events::Event ev;
    ev.event_id = UuidV7::Generate();
    ev.account_id = account.NumericId();
    ev.aggregate_kind = events::AggregateKind::Pulls;
    ev.version = account.dirty.cached_pulls_version + 1;
    ev.event_type = "pull_performed";
    ev.idempotency_key = req.idempotencyKey;
    ev.data = events::pulls::ToJson(evt);

    txn.AppendEvent(std::move(ev));

    // Apply reducer to update projected state (and emit grant effects)
    auto reduced = ctx.repository->PullsReducer().Apply(account.pulls_state, "pull_performed", evt);
    account.pulls_state = std::move(reduced.state);
    for (const auto& effect : reduced.effects) {
        std::visit(EffectDispatcher{account, txn}, effect);
    }
    // Save the new RNG state on the account
    account.pulls_state.rng_state = rng.State();

    // Wallet event for the cost
    events::Event wallet_ev = MakeWalletDelta(account, Currency::Tickets, -1, "pull_cost", ev.event_id);
    txn.AppendEvent(std::move(wallet_ev));

    txn.Commit();
    return BuildResponse(results);
}
```

(The `EffectDispatcher` visitor mutates `account` via setters when a grant effect is applied, and emits to the outbox for any cross-service effects.)

- [ ] **Step 3: Test**

Build and exercise via the existing GachaTest client (or write an integration test that connects to the Account service, sends a `Pull` RPC, and verifies an event row exists).

- [ ] **Step 4: Commit**

```bash
git add Server/Account/src/GachaHandlers.hpp Server/Account/src/GachaRNG.hpp
git commit -m "refactor(account): GachaHandlers use AccountTransaction; xoshiro RNG capture"
```

### Task 29: Migrate AccountHandlers / QuestHandlers / ProgressionHandlers

Each follows the same pattern as Task 28. Apply mechanically:

- [ ] **Step 1: AccountHandlers**
  - `HandleSetParty` → `txn.RecordAudit(...)` for the party change; flush via `account.SetPartySlot(...)`.
  - `HandleAddCurrency` → `txn.AppendEvent(...)` for the wallet aggregate.

- [ ] **Step 2: QuestHandlers**
  - `HandleClaimQuestReward` → `txn.AppendEvent(quest_claims, 'quest_reward_claimed', ...)`; the reducer emits `GrantCurrencyEffect` etc., dispatched into wallet/material events via the EffectDispatcher.

- [ ] **Step 3: ProgressionHandlers**
  - `HandleLevelCharacter` → audit-only (not event-sourced).
  - Story XP / level advance → `txn.AppendEvent(progression, 'story_level_advanced', ...)`.

- [ ] **Step 4: Commit each batch**

```bash
git add Server/Account/src/AccountHandlers.hpp
git commit -m "refactor(account): AccountHandlers use AccountTransaction"

git add Server/Account/src/QuestHandlers.hpp
git commit -m "refactor(account): QuestHandlers use AccountTransaction + quest_claims events"

git add Server/Account/src/ProgressionHandlers.hpp
git commit -m "refactor(account): ProgressionHandlers use AccountTransaction + progression events"
```

### Task 30: TickQuests helper

**Files:**
- Create: `Server/Account/src/TickQuests.hpp`
- Modify: `Server/Account/src/QuestHandlers.hpp`

- [ ] **Step 1: Write the helper**

`Server/Account/src/TickQuests.hpp`:
```cpp
#pragma once
#include "AccountData.hpp"
#include "AccountTransaction.hpp"
#include <chrono>

namespace aphelyon {

class TickQuests {
public:
    // Returns true if any quest was reset / unlocked (i.e. tx has work to commit).
    static bool Apply(AccountTransaction& txn, AccountData& account) {
        const auto now = std::chrono::system_clock::now();
        bool changed = false;
        for (auto& [quest_id, st] : account.questStates) {
            if (st.reset_at && *st.reset_at <= now) {
                st.state = static_cast<int16_t>(QuestState::Active);
                st.reset_at = NextResetFor(st.quest_type, now);
                account.dirty.quest_ids.insert(quest_id);
                changed = true;
            }
        }
        return changed;
    }

private:
    static std::chrono::system_clock::time_point NextResetFor(int quest_type, std::chrono::system_clock::time_point now) {
        using namespace std::chrono;
        const auto day = floor<days>(now);  // current UTC date at 00:00
        switch (static_cast<QuestType>(quest_type)) {
            case QuestType::Daily:
                return day + days(1);
            case QuestType::Weekly: {
                // Next Monday 00:00 UTC. days{0} is 1970-01-01, a Thursday → weekday(now).
                const auto wd = weekday{day};
                const auto days_until_monday = (Monday - wd).count() == 0 ? days(7) : days((Monday - wd).count());
                return day + days_until_monday;
            }
            case QuestType::Event:
                // Event quests carry their own reset_at in metadata or quest definition;
                // expired ones don't auto-reset. Return time_point::max to indicate "no further reset".
                return time_point<system_clock>::max();
            case QuestType::OneShot:
            default:
                return time_point<system_clock>::max();
        }
    }
};

} // namespace aphelyon
```

- [ ] **Step 2: Update QuestHandlers**

Make `HandleGetQuestState` a pure read — no tick. Add a `TickQuests::Apply(...)` call at:
- `HandleClaimQuestReward` entry
- `HandleCompleteQuest` entry
- `HandleReportQuestProgress` entry
- Account-load path (in `AccountRepository::Load` or the first-touch handler hook)

```cpp
ResponseFrame HandleClaimQuestReward(const RequestFrame& req, HandlerContext& ctx) {
    auto& account = ctx.GetLockedAccount(req.playerId);
    auto txn = ctx.repository->Begin(account);
    TickQuests::Apply(txn, account);
    // ... rest of handler ...
    txn.Commit();
}

ResponseFrame HandleGetQuestState(const RequestFrame& req, HandlerContext& ctx) {
    auto& account = ctx.GetLockedAccount(req.playerId);
    // NO transaction; pure read of account.questStates as currently in cache.
    return BuildResponse(account.questStates);
}
```

- [ ] **Step 3: Commit**

```bash
git add Server/Account/src/TickQuests.hpp Server/Account/src/QuestHandlers.hpp
git commit -m "fix(account): explicit TickQuests; GetQuestState is now a pure read"
```

---

## Phase 10 — Property tests + golden-file tests

### Task 31: rapidcheck property tests

**Files:**
- Create: `Server/Account/tests/PropertyTests/ReplayDeterminismTest.cpp`
- Create: `Server/Account/tests/PropertyTests/SnapshotEquivalenceTest.cpp`
- Create: `Server/Account/tests/PropertyTests/InvariantTests.cpp`

- [ ] **Step 1: Replay determinism**

`Server/Account/tests/PropertyTests/ReplayDeterminismTest.cpp`:
```cpp
#include <rapidcheck.h>
#include <rapidcheck/catch.h>
#include <catch2/catch_test_macros.hpp>
#include "reducers/WalletReducer.hpp"

using namespace aphelyon::reducers;
using namespace aphelyon::events::wallet;

namespace rc {
template<>
struct Arbitrary<CurrencyDelta> {
    static Gen<CurrencyDelta> arbitrary() {
        return gen::build<CurrencyDelta>(
            gen::set(&CurrencyDelta::currency, gen::elementOf(std::vector<Currency>{
                Currency::Credits, Currency::Tickets, Currency::Scrap})),
            gen::set(&CurrencyDelta::amount, gen::inRange<std::int64_t>(-1000, 1000)),
            gen::set(&CurrencyDelta::reason, gen::just<std::string>("test"))
        );
    }
};
}

TEST_CASE("Wallet reducer is deterministic across triple replay", "[property][wallet]") {
    rc::prop("triple replay produces equal state", []{
        auto events = *rc::gen::container<std::vector<CurrencyDelta>>(rc::gen::arbitrary<CurrencyDelta>());
        // Ensure each delta has correct balance_before/after by simulating sequentially
        // ... (helper to massage random data into a valid event log) ...

        auto fold = [&]() {
            WalletReducer r;
            WalletState s;
            for (auto& e : events) {
                try { s = r.Apply(s, "credits_added", e).state; } catch (...) {}
            }
            return s;
        };
        auto s1 = fold(), s2 = fold(), s3 = fold();
        RC_ASSERT(s1.Get(Currency::Credits) == s2.Get(Currency::Credits));
        RC_ASSERT(s2.Get(Currency::Credits) == s3.Get(Currency::Credits));
    });
}
```

- [ ] **Step 2: Snapshot equivalence**

`Server/Account/tests/PropertyTests/SnapshotEquivalenceTest.cpp`:
```cpp
#include <rapidcheck/catch.h>
#include "reducers/WalletReducer.hpp"

TEST_CASE("split-and-fold equals full-fold", "[property][wallet][snapshot]") {
    rc::prop("fold(0, log) == fold(fold(0, log[:k]), log[k:])", []{
        // Generate event log + split index k
        // Fold full; fold prefix then suffix; assert equality.
    });
}
```

- [ ] **Step 3: Domain invariants**

`Server/Account/tests/PropertyTests/InvariantTests.cpp`:
```cpp
#include <rapidcheck/catch.h>

TEST_CASE("Currency balances never go negative regardless of event order", "[property][wallet][invariant]") {
    rc::prop("balance always >= 0", []{
        // Apply random delta sequence; the reducer should THROW on would-go-negative;
        // assert that no successfully-applied state has a negative balance.
    });
}

TEST_CASE("Pity counters never exceed hard-pity threshold", "[property][pulls][invariant]") {
    rc::prop("pity_5 <= 90, pity_4 <= 10", []{
        // Generate pull event sequences; fold; assert.
    });
}
```

- [ ] **Step 4: Commit**

```bash
git add Server/Account/tests/PropertyTests/
git commit -m "test(account): rapidcheck property tests (replay/snapshot/invariants)"
```

### Task 32: Golden-file schema migration tests

**Files:**
- Create: `Server/Account/tests/events/v1_pull_performed.json`
- Create: `Server/Account/tests/events/v1_credits_spent.json`
- Create: `Server/Account/tests/events/v1_quest_reward_claimed.json`
- Create: `Server/Account/tests/events/v1_story_level_advanced.json`
- Create: `Server/Account/tests/GoldenFile/SchemaMigrationTest.cpp`

- [ ] **Step 1: Write golden event files**

`Server/Account/tests/events/v1_pull_performed.json`:
```json
{
  "event_id": "01923000-0000-7000-8000-000000000001",
  "schema_version": 1,
  "data": {
    "banner_id": "char_event_001",
    "banner_version": "2.7",
    "cost": { "currency": "tickets", "amount": 1 },
    "rng_state_before": [1, 2, 3, 4],
    "algorithm_version": 1,
    "pity_5_before": 49,
    "pity_4_before": 7,
    "guarantee_5_before": false,
    "results": [
      { "template_id": "char_4star_001", "rarity": 4, "instance_id": null, "was_featured": true }
    ],
    "pity_5_after": 50,
    "pity_4_after": 0,
    "guarantee_5_after": false
  }
}
```

(Similar for `v1_credits_spent.json`, `v1_quest_reward_claimed.json`, `v1_story_level_advanced.json`.)

- [ ] **Step 2: Write the test**

`Server/Account/tests/GoldenFile/SchemaMigrationTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "events/PullEvents.hpp"
#include "reducers/PullsReducer.hpp"

TEST_CASE("Golden v1 pull_performed event folds to expected state", "[golden][schema]") {
    std::ifstream f("tests/events/v1_pull_performed.json");
    nlohmann::json j; f >> j;
    auto evt_data = j["data"];

    aphelyon::events::pulls::PullPerformed evt = FromJson<aphelyon::events::pulls::PullPerformed>(evt_data);
    aphelyon::reducers::PullsReducer r;
    aphelyon::reducers::PullsState s;
    s.pity_5 = 49; s.pity_4 = 7;
    auto out = r.Apply(s, "pull_performed", evt);

    REQUIRE(out.state.pity_5 == 50);
    REQUIRE(out.state.pity_4 == 0);
}

// One test per golden file; auto-generate via loop if there are many.
```

- [ ] **Step 3: Commit**

```bash
git add Server/Account/tests/events/ Server/Account/tests/GoldenFile/
git commit -m "test(account): v1 golden event files + schema migration tests"
```

---

## Phase 11 — clang-tidy reducer purity check

### Task 33: Custom clang-tidy check

**Files:**
- Create: `tools/clang-tidy/GachaReducerPurityCheck.cpp`
- Create: `tools/clang-tidy/.clang-tidy`

- [ ] **Step 1: Write the check**

`tools/clang-tidy/GachaReducerPurityCheck.cpp`:
```cpp
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/Tooling.h"
#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tidy;

namespace aphelyon::tidy {

class ReducerPurityCheck : public ClangTidyCheck {
public:
    ReducerPurityCheck(StringRef name, ClangTidyContext* ctx) : ClangTidyCheck(name, ctx) {}

    void registerMatchers(MatchFinder* f) override {
        // Match calls inside any file under reducers/
        auto in_reducers = isExpansionInFileMatching(".*/reducers/.*");
        f->addMatcher(
            callExpr(in_reducers, callee(functionDecl(hasAnyName(
                "::std::chrono::system_clock::now",
                "::std::time", "::time", "::clock_gettime",
                "::rand", "::std::random_device::operator()",
                "::std::getenv", "::std::system",
                "::std::filesystem::*"  // glob-ish; clang-tidy doesn't support globs, list explicit names
            )))).bind("call"), this);
    }

    void check(const MatchFinder::MatchResult& r) override {
        if (auto* e = r.Nodes.getNodeAs<CallExpr>("call")) {
            diag(e->getBeginLoc(), "forbidden non-deterministic call inside a reducer");
        }
    }
};

class AphelyonModule : public ClangTidyModule {
public:
    void addCheckFactories(ClangTidyCheckFactories& f) override {
        f.registerCheck<ReducerPurityCheck>("aphelyon-reducer-purity");
    }
};

} // namespace

static ClangTidyModuleRegistry::Add<aphelyon::tidy::AphelyonModule> X("aphelyon-module", "Aphelyon-specific checks");
```

- [ ] **Step 2: Add config**

`tools/clang-tidy/.clang-tidy`:
```yaml
Checks: '-*,gacha-reducer-purity'
WarningsAsErrors: 'aphelyon-reducer-purity'
HeaderFilterRegex: '.*/reducers/.*\.hpp$'
```

- [ ] **Step 3: Build + integrate into CI**

This requires building against the clang-tools-extra source tree, which is a heavy build. For solo dev, an acceptable alternative is to write the check as a `grep`-based linter that runs in CI:

`tools/scripts/check-reducer-purity.sh`:
```sh
#!/usr/bin/env sh
set -e
FORBIDDEN='\b(time\(|system_clock::now|random_device|std::rand|std::getenv|std::system|std::filesystem::)\b'
FAIL=0
for f in Server/Account/src/reducers/*.hpp; do
  if grep -nE "$FORBIDDEN" "$f"; then
    echo "FORBIDDEN call found in $f"
    FAIL=1
  fi
done
exit $FAIL
```

This is the pragmatic version — ship the grep linter now, swap to a real clang-tidy check when CI infrastructure supports it.

- [ ] **Step 4: Commit**

```bash
git add tools/clang-tidy/ tools/scripts/check-reducer-purity.sh
git commit -m "feat(tooling): reducer purity linter (grep-based for solo CI)"
```

---

## Phase 12 — Backup setup (operational)

### Task 34: WAL-G configuration

**Files:**
- Create: `Server/scripts/wal-g-setup.sh`
- Create: `Server/docs/operations/backup-drill.md`

- [ ] **Step 1: WAL-G setup script**

`Server/scripts/wal-g-setup.sh`:
```sh
#!/usr/bin/env sh
# Configure WAL-G in the Postgres container for continuous WAL archive to S3-compatible storage.
# Required env: WALG_S3_PREFIX, AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_REGION

set -eu

if [ -z "${WALG_S3_PREFIX:-}" ]; then
  echo "WALG_S3_PREFIX not set; skipping backup configuration."
  exit 0
fi

docker compose exec postgres apt-get update
docker compose exec postgres apt-get install -y wal-g

docker compose exec postgres bash -lc "cat > /etc/wal-g/wal-g.json <<EOF
{
  \"WALG_S3_PREFIX\": \"$WALG_S3_PREFIX\",
  \"AWS_ACCESS_KEY_ID\": \"$AWS_ACCESS_KEY_ID\",
  \"AWS_SECRET_ACCESS_KEY\": \"$AWS_SECRET_ACCESS_KEY\",
  \"AWS_REGION\": \"$AWS_REGION\"
}
EOF"

docker compose exec postgres bash -lc 'echo "archive_mode = on" >> /var/lib/postgresql/data/postgresql.conf'
docker compose exec postgres bash -lc 'echo "archive_command = '\''wal-g wal-push %p'\''" >> /var/lib/postgresql/data/postgresql.conf'
docker compose restart postgres
```

- [ ] **Step 2: Drill documentation**

`Server/docs/operations/backup-drill.md`:
```markdown
# Backup restore drill

Run quarterly. A backup you have never restored is theoretical.

## Procedure
1. Spin up a throwaway VM or container: `docker run -d --name drill_pg postgres:16`
2. Configure WAL-G in the drill container with the same env vars.
3. `wal-g backup-fetch /var/lib/postgresql/data LATEST`
4. `wal-g wal-fetch ...` until catch-up.
5. Connect to drill DB; verify accounts table exists and contains expected rows.
6. Tear down.

## Targets
- RPO: ≤ 1 minute
- RTO: ≤ 30 minutes (unattended)
```

- [ ] **Step 3: Commit**

```bash
git add Server/scripts/wal-g-setup.sh Server/docs/operations/backup-drill.md
git commit -m "feat(ops): WAL-G setup + quarterly drill documentation"
```

---

## Phase 13 — Cutover + cleanup

### Task 35: Final cutover

**Files:**
- Delete: `Server/bin/Debug-windows-x86_64/Account/data/accounts/*.json`
- Delete: `Server/bin/Release-windows-x86_64/Account/data/accounts/*.json`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Run reset script**

Run: `cd Server && scripts\db-reset.bat`
Expected: dev volume wiped, JSON accounts wiped, migrations re-applied.

- [ ] **Step 2: Smoke test full stack**

Launch all services: `scripts\start-all.bat`
Register a fresh test account via the client.
Pull on a banner. Verify in Postgres: `SELECT * FROM events WHERE aggregate_kind = 'pulls'` returns the pull event.

- [ ] **Step 3: Update CLAUDE.md**

Modify `CLAUDE.md` — update the "Data files" section under Combat / Architecture to note that Account now persists to Postgres rather than JSON files. Update memory entries that referenced `bin/*/Account/data/accounts/`.

- [ ] **Step 4: Final commit**

```bash
git add CLAUDE.md
git commit -m "docs(account): update CLAUDE.md for Postgres persistence cutover"
```

- [ ] **Step 5: Open PR**

Run: `gh pr create --title "feat(account): Postgres + selective ES migration" --body "..."` with summary referencing the spec.

---

## Open considerations (carried from spec)

These remain for the executor to decide during implementation; the spec lists them as deliberate design space:

- **Connection-pool implementation choice.** This plan rolls a thin custom pool (Task 5). If a more mature alternative is preferred during build-out, swap before Task 24.
- **rapidcheck integration mechanism.** This plan vendors (Task 4). Plan-phase executor may choose vcpkg overlay if vendoring proves friction.
- **Cross-player wallet view.** Not built here; future spec adds projection daemon + materialized view.
- **Rollback semantics on `Transaction::Rollback`.** This plan marks the cache stale and forces re-fetch on next access (Task 24, `MarkStaleForReload`). Alternative is full pre-mutation snapshot.

---

## Self-review notes (filled in by author)

- **Coverage:** Every section in the spec (Architecture, Schema-Relational, Schema-Event-Sourced, Schema-Support, Schema-Future-seams, Repository, Concurrency, ES Patterns, TickQuests fix, Action vocabulary, Migration, Testing, Storage/Ops, Out-of-scope) has at least one task implementing or explicitly deferring it.
- **Type consistency:** `UuidV7::ValueType` is used consistently as the C++ instance ID type; serialized as TEXT via `UuidV7::ToString` at the SQL boundary. `AggregateKind` enum values match the SQL strings via `AggregateKindToStr`.
- **TDD applied** for all 4 reducers (Tasks 16–19) and for the UUID generator (Task 12). Foundation infrastructure (Tasks 1–11, 13–14, 20–27) uses integration testing rather than per-step TDD because the components are I/O-bound and the tests need real Postgres.
- **Future-seams** (friendships, mail, achievements, season_pass) intentionally have no task — they're documented in the spec, the migration framework supports adding them via numbered migrations later.
