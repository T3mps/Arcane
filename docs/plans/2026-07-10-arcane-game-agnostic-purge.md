# Arcane Game-Agnostic Purge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove all game-specific (Aphelyon) code from the Arcane engine workspace: relocate the gacha domain types and the game/service logging surface back to `Server/Common` (namespace `Aphelyon`), and give Core's generic mechanisms a game-neutral logging seam.

**Architecture:** Three-sided move built on two survey docs (`.superpowers/sdd/agnostic-survey-1-arcane-inventory.md`, `agnostic-survey-2-consumers.md`). (1) Core `Logger` grows a generic string-keyed surface + `LOG_CORE_*` macros; the 9 contaminated call sites in Crypto/RateLimiter/Protocol repoint to it. (2) The server-side shims stop being pure re-export shims and become the real Aphelyon facade: category enum + `LOG_<CATEGORY>_*` macros + the four analytics event functions + `gacha_server.log` + `RateLimits` presets. (3) `Types.hpp` content replaces its own server shim; the Arcane original is deleted. Every task keeps BOTH workspaces (Arcane /MD + Server static-CRT, which compiles the same Core sources as `ArcaneCore`) building and green.

**Tech Stack:** C++23, MSVC (VS 2026), premake5 (regen per workspace after file add/delete), Catch2 v3 (ArcaneTests, CommonTests), spdlog (header-only, per-module registry).

## Global Constraints

- Branch: `feature/arcane-foundations`, worked in the existing worktree at `C:\Users\ETHANT~1\AppData\Local\Temp\claude\D--dev-starworks-Gacha\27eb7fcd-1e4e-4ada-ae92-e422fa28965b\scratchpad\foundations-rebase` (referred to below as `<WT>`). Do not push. Do not touch the main repo working tree at `D:\dev\starworks\Gacha` except `.superpowers/sdd/` report files.
- ASCII-only source, UTF-8 without BOM. Conventional commits, NO AI/Claude trailers. `git commit -F <tempfile>` (PowerShell pipes add BOMs). Stage exact paths — never `git add -A`.
- Line numbers cited from the surveys DRIFT (W2 just landed) — always locate by content anchor, never by number.
- Verbatim-move discipline: when a code block MOVES between files, cut-and-paste the exact text (then apply only the listed transformations). Do not retype or "improve" moved code.
- Enum integer values in the relocated Types content MUST stay identical (the Love2D client mirrors them via `protocol.json` name->int maps).
- MSBuild (PowerShell only — Git Bash mangles switches): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" <sln> /p:Configuration=Debug /m /nologo /v:minimal`.
- Premake regen (needed in a workspace whenever a globbed file is added/deleted there): from `<WT>\Arcane\`: `& "<WT>\ThirdParty\premake5\premake5.exe" vs2026`; from `<WT>\Server\`: same exe, same arg. Never GenerateProjects.bat (it pauses).
- Arcane test gate: run from `<WT>\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`, FOREGROUND: `.\ArcaneTests.exe "~[gpu]"` must be ALL PASS. Baseline entering this plan: **177089 assertions / 577 cases**. Test-count changes from rewritten `[logger]` tests are expected — record the new counts per task; assertion failures are not acceptable. DO NOT run `[gpu]`, Loom, or SandboxSmoke (known NVIDIA-driver instability with windowed d3d12 under the current remote-display session — see `.superpowers/sdd/progress.md` bottom).
- Server test gate: build `<WT>\Server\Aphelyon.slnx`, then run CommonTests, AuthTests, CombatTests executables from their own output dirs under `<WT>\Server\bin\Debug-windows-x86_64\`. Do NOT run AccountTests locally (~17 min, needs Postgres; CI covers it). Server services must NOT be running during tests.
- Server vcxproj/slnx are NOT git-tracked (verified) — regen produces zero git churn.

## File Structure

| File | Role after this plan |
|---|---|
| `Arcane/Core/src/Arcane/Util/Logger.hpp` | Generic engine logger only: `Level`, `Init(console, file, filePath)`, string-keyed `Get(name)`, `Set*Level`, public `JsonEscape`, `LOG_CORE_*` macros. No categories enum, no analytics, no game filename, no picosha2. |
| `Arcane/Core/src/Arcane/Types/Types.hpp` | DELETED (with its `Types/` dir). |
| `Arcane/Core/src/Arcane/Crypto/Crypto.hpp` | Unchanged mechanism; log calls via `LOG_CORE_*`; comments de-flavored. |
| `Arcane/Core/src/Arcane/Net/RateLimiter.hpp` | Unchanged mechanism; log calls via `LOG_CORE_*`; `RateLimits` presets removed. |
| `Arcane/Core/src/Arcane/Net/Protocol.hpp` | Unchanged mechanism; log calls via `LOG_CORE_*`; stray Types include removed. |
| `Arcane/Core/src/ArcaneCore.cpp` | Types include removed from the include list. |
| `Server/Common/src/Util/Logger.hpp` | Real Aphelyon logging facade: `class Logger : public Arcane::Logger` with `LogCategory` enum, `Get(LogCategory)`, `Init(console,file)` -> `gacha_server.log`, the 4 analytics statics, + the `LOG_SERVER/NET/AUTH/GACHA/DATA/PROTOCOL_*` macros. |
| `Server/Common/src/Types/Types.hpp` | Real gacha domain types in `namespace Aphelyon` (shim replaced by content). |
| `Server/Common/src/Net/RateLimiter.hpp` | Shim + real `Aphelyon::RateLimits` presets (alias replaced by content). |
| `Arcane/Tests/src/LoggerTest.cpp` | Rewritten against the generic surface (JsonEscape direct, Get-by-name, Init file path). |
| `Server/Common/tests/AnalyticsEventShapeTest.cpp` | NEW: the analytics-event escaping/shape coverage (adopted from the old ArcaneTests analytics cases). |

Sequencing rationale: Task 1 is additive (both workspaces keep compiling). Task 2 is the atomic Logger-surface move (Core sheds + facade gains in ONE commit so no intermediate state has duplicate or missing macros). Task 3 is the Types move (independent of logging). Task 4 is proof-sweep + docs.

---

### Task 1: Core generic logging seam (additive) + repoint the 9 contaminated call sites

**Files:**
- Modify: `Arcane/Core/src/Arcane/Util/Logger.hpp`
- Modify: `Arcane/Core/src/Arcane/Crypto/Crypto.hpp` (3 sites)
- Modify: `Arcane/Core/src/Arcane/Net/RateLimiter.hpp` (3 sites)
- Modify: `Arcane/Core/src/Arcane/Net/Protocol.hpp` (6 sites)
- Test: `Arcane/Tests/src/LoggerTest.cpp` (extend)

**Interfaces:**
- Consumes: existing `Logger` internals (sink members, mutex/registry pattern — read the class first).
- Produces (later tasks rely on these EXACT names): `static spdlog::logger* Arcane::Logger::Get(std::string_view name)` (lazily creates a logger named `name` wired to the already-configured console+file sinks; safe pre-Init: lazily calls `Init` defaults like the existing enum `Get` does); `static void Arcane::Logger::Init(Level consoleLevel, Level fileLevel, const std::string& logFilePath)` (empty `logFilePath` => console sink only); PUBLIC `static std::string Arcane::Logger::JsonEscape(const std::string& s)`; macros `LOG_CORE_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL(...)` => `::Arcane::Logger::Get("Core")->...`.

- [ ] **Step 1: Read the current class** — `Logger.hpp` top-to-bottom in the worktree (W1's `JsonEscape` is a private static near the bottom; `Init` currently hardcodes the category logger set and `logs/gacha_server.log`). Also grep Core for ALL `LOG_` macro uses to confirm the contamination set is exactly Crypto(3)/RateLimiter(3)/Protocol(6): `rg -n "LOG_[A-Z]+_" <WT>/Arcane/Core/src`.
- [ ] **Step 2: Write failing tests** — append to `Arcane/Tests/src/LoggerTest.cpp`:

```cpp
TEST_CASE("Logger: JsonEscape escapes quotes, backslashes, and control chars", "[logger]")
{
    using Arcane::Logger;
    CHECK(Logger::JsonEscape("plain") == "plain");
    CHECK(Logger::JsonEscape("a\"b") == "a\\\"b");
    CHECK(Logger::JsonEscape("a\\b") == "a\\\\b");
    CHECK(Logger::JsonEscape("a\nb") == "a\\nb");
}

TEST_CASE("Logger: Get by name returns a usable named logger", "[logger]")
{
    auto* lg = Arcane::Logger::Get("Core");
    REQUIRE(lg != nullptr);
    CHECK(lg->name() == "Core");
    // Same name twice = same logger (registry, not a fresh sink stack per call).
    CHECK(Arcane::Logger::Get("Core") == lg);
    // A second, arbitrary name also works (lazy creation).
    auto* other = Arcane::Logger::Get("PurgeSeamProbe");
    REQUIRE(other != nullptr);
    CHECK(other != lg);
}
```

(If `JsonEscape` taking `std::string` vs `std::string_view` differs in the worktree, match the existing signature — do not change it.)
- [ ] **Step 3: Run to verify red** — from the ArcaneTests exe dir: `.\ArcaneTests.exe "[logger]"`. Expected: compile failure (`JsonEscape` private / `Get(const char*)` no overload).
- [ ] **Step 4: Implement the seam in `Logger.hpp`** — (a) move `JsonEscape` to the public section unchanged; (b) add the string-keyed `Get`:

```cpp
        // Generic named-logger access. Creates the logger on first use with the
        // sinks configured at Init (console always; file sink when Init was given
        // a log file path). Category names are the CONSUMER's vocabulary -- the
        // engine core itself logs only under "Core" (LOG_CORE_* below).
        static spdlog::logger* Get(std::string_view name)
        {
            if (!IsInitialized())
                Init();
            std::string key(name);
            if (auto existing = spdlog::get(key))
                return existing.get();
            return CreateLogger(key).get();
        }
```

Adapt the body to the class's real internals: reuse the existing `CreateLogger` sink plumbing (strip its enum name-map so it takes the name string); keep whatever locking the class already uses around registry mutation. (c) add the `logFilePath` parameter to `Init` — signature `static void Init(Level consoleLevel = Level::Info, Level fileLevel = Level::Trace, const std::string& logFilePath = "logs/gacha_server.log")` FOR NOW (the game default moves out in Task 2; keeping it this task means zero behavior change for the Server build) — empty string skips the rotating-file sink. (d) append the macros right after the existing macro families:

```cpp
    // Engine-core neutral logging (generic mechanisms: crypto, rate limiting,
    // protocol framing). Game/service category macros live with the consumer.
    #define LOG_CORE_TRACE(...)    ::Arcane::Logger::Get("Core")->trace(__VA_ARGS__)
    #define LOG_CORE_DEBUG(...)    ::Arcane::Logger::Get("Core")->debug(__VA_ARGS__)
    #define LOG_CORE_INFO(...)     ::Arcane::Logger::Get("Core")->info(__VA_ARGS__)
    #define LOG_CORE_WARN(...)     ::Arcane::Logger::Get("Core")->warn(__VA_ARGS__)
    #define LOG_CORE_ERROR(...)    ::Arcane::Logger::Get("Core")->error(__VA_ARGS__)
    #define LOG_CORE_CRITICAL(...) ::Arcane::Logger::Get("Core")->critical(__VA_ARGS__)
```

- [ ] **Step 5: Repoint the 9 call sites** (message text unchanged, only the macro name swaps; locate by the quoted message text):
  - `Crypto.hpp`: `LOG_AUTH_WARN("Password verification failed: malformed stored hash"...)` -> `LOG_CORE_WARN`; `LOG_AUTH_DEBUG("Password verification failed: hash mismatch"...)` -> `LOG_CORE_DEBUG`; the BCryptGenRandom-fallback `LOG_AUTH_WARN`/whatever W2 left at that site -> `LOG_CORE_WARN` (W2's E03-3 edited this region — read it first; repoint whatever `LOG_AUTH_*` calls remain, including any W2 added).
  - `RateLimiter.hpp`: `LOG_AUTH_TRACE("Rate limit: {} still in cooldown"...)` -> `LOG_CORE_TRACE`; `LOG_AUTH_DEBUG("Rate limit: {} cooldown expired, resetting"...)` -> `LOG_CORE_DEBUG`; `LOG_AUTH_WARN("Rate limit triggered for {}"...)` -> `LOG_CORE_WARN`.
  - `Protocol.hpp`: all six `LOG_PROTOCOL_*` sites -> same level `LOG_CORE_*`.
  - Verify afterward: `rg -n "LOG_(AUTH|PROTOCOL|SERVER|NET|DATA|GACHA)_" <WT>/Arcane/Core/src` returns ONLY the macro definitions in Logger.hpp.
- [ ] **Step 6: Build Arcane + run green** — build `Arcane.slnx`; then `.\ArcaneTests.exe "[logger],[crypto],[ratelimiter],[protocol],[wire]"` ALL PASS; then full `.\ArcaneTests.exe "~[gpu]"` ALL PASS (record counts; expect 177089/577 plus the new logger assertions).
- [ ] **Step 7: Build Server untouched-proof** — regen Server (`premake5.exe vs2026` from `<WT>\Server\`), build `<WT>\Server\Aphelyon.slnx` Debug. Expected: clean (old enum surface still present, so nothing server-side changed).
- [ ] **Step 8: Commit**

```bash
git add Arcane/Core/src/Arcane/Util/Logger.hpp Arcane/Core/src/Arcane/Crypto/Crypto.hpp Arcane/Core/src/Arcane/Net/RateLimiter.hpp Arcane/Core/src/Arcane/Net/Protocol.hpp Arcane/Tests/src/LoggerTest.cpp
git commit -F <msgfile>   # refactor(arcane/core): generic logging seam (string-keyed Get + LOG_CORE_*) + repoint 9 service-flavored call sites
```

---

### Task 2: Atomic Logger-surface move — Core sheds the game/service surface, the Server facade gains it

**Files:**
- Modify: `Arcane/Core/src/Arcane/Util/Logger.hpp` (delete game surface)
- Modify: `Server/Common/src/Util/Logger.hpp` (shim -> real facade)
- Modify: `Server/Common/src/Net/RateLimiter.hpp` (alias -> real presets)
- Modify: `Arcane/Core/src/Arcane/Net/RateLimiter.hpp` (delete presets)
- Modify: `Arcane/Tests/src/LoggerTest.cpp` (drop analytics-event cases)
- Create: `Server/Common/tests/AnalyticsEventShapeTest.cpp`

**Interfaces:**
- Consumes: Task 1's `Get(std::string_view)`, `Init(console, file, filePath)`, public `JsonEscape`, `LOG_CORE_*`.
- Produces: `Aphelyon::Logger` facade with EXACTLY the API today's 33 server includers use: `Logger::Init(Level, Level)`, `Logger::Get(LogCategory)`, `Logger::LogPullEvent(...)`, `LogAuthEvent(...)`, `LogSessionEvent(...)`, `LogConnectionEvent(...)` (same parameter lists as the current Core versions — copy the signatures verbatim), `enum class LogCategory { Server, Net, Auth, Gacha, Data, Protocol }`, and the `LOG_SERVER_*`/`LOG_NET_*`/`LOG_AUTH_*`/`LOG_GACHA_*`/`LOG_DATA_*`/`LOG_PROTOCOL_*` macro families (now expanding through `::Aphelyon::Logger`). `Aphelyon::RateLimits::{Login,Registration,Pulls,AddCurrency}` with today's exact `Config` values.

- [ ] **Step 1: Inventory what moves.** In Core `Logger.hpp` mark: `enum class LogCategory` block; the six named-logger registrations inside `Init` + default-logger selection; the `CreateLogger` enum-name mapping remnants; the four analytics statics (`LogPullEvent`, `LogAuthEvent`, `LogSessionEvent`, `LogConnectionEvent`) INCLUDING their W1 JsonEscape-hardened bodies; the `#include "picosha2.hpp"`; the six macro families `LOG_SERVER_/NET_/AUTH_/GACHA_/DATA_/PROTOCOL_*`. In Core `RateLimiter.hpp` mark the `namespace RateLimits { ... }` preset block. Check whether ArcaneTests reference `RateLimits::` presets (`rg -n "RateLimits::" <WT>/Arcane/Tests/src`) — if W2's RateLimiterTest uses a preset, rewrite that usage to construct the equivalent `RateLimiter::Config` inline with the same values.
- [ ] **Step 2: Build the facade** — replace `Server/Common/src/Util/Logger.hpp` shim body with (keep the file's existing header-comment style; this is the shape, fill the analytics bodies by VERBATIM MOVE from Core):

```cpp
#pragma once

#include <Arcane/Util/Logger.hpp>

#include "picosha2.hpp"

namespace Aphelyon
{
    using Arcane::Level;

    // Aphelyon service log categories. The engine core is category-agnostic
    // (string-keyed); these names are this game's operational vocabulary.
    enum class LogCategory : uint8_t { Server, Net, Auth, Gacha, Data, Protocol };

    inline const char* LogCategoryName(LogCategory c)
    {
        switch (c)
        {
            case LogCategory::Server:   return "Server";
            case LogCategory::Net:      return "Net";
            case LogCategory::Auth:     return "Auth";
            case LogCategory::Gacha:    return "Gacha";
            case LogCategory::Data:     return "Data";
            case LogCategory::Protocol: return "Protocol";
        }
        return "Server";
    }

    class Logger : public Arcane::Logger
    {
    public:
        static void Init(Level consoleLevel = Level::Info, Level fileLevel = Level::Trace)
        {
            Arcane::Logger::Init(consoleLevel, fileLevel, "logs/gacha_server.log");
        }

        static spdlog::logger* Get(LogCategory c)
        {
            return Arcane::Logger::Get(LogCategoryName(c));
        }

        // --- Structured analytics events (moved verbatim from Arcane Core,
        // --- 2026-07-10 game-agnostic purge; bodies unchanged incl. E01-4
        // --- JsonEscape hardening; Get(...) now resolves via LogCategoryName).
        //  [PASTE LogPullEvent / LogAuthEvent / LogSessionEvent / LogConnectionEvent
        //   here verbatim; transformations allowed: Get(LogCategory::X) stays as-is
        //   (resolves to the facade Get above); JsonEscape stays (inherited public).]
    };
}

// Category convenience macros (moved from Arcane Core; now Aphelyon-owned).
#define LOG_SERVER_TRACE(...)    ::Aphelyon::Logger::Get(::Aphelyon::LogCategory::Server)->trace(__VA_ARGS__)
// ... [full six families x six levels, exactly as they exist in Core today,
//      with ::Arcane:: -> ::Aphelyon:: -- move the block verbatim and sed the namespace]
```

Notes: session-token hashing inside `LogSessionEvent` keeps its picosha2 use — confirm the `Common` premake project can see the picosha2 include dir (`Server/premake5.lua`, project "Common" includedirs); if not, add the same `IncludeDir["picosha2"]` entry ArcaneCore uses and regen.
- [ ] **Step 3: Move the RateLimits presets** — in `Server/Common/src/Net/RateLimiter.hpp`, replace `namespace RateLimits = Arcane::RateLimits;` with the preset block moved verbatim from Core (inside `namespace Aphelyon`, `using Arcane::RateLimiter;` already present):

```cpp
    // Service rate-limit policy presets (moved from Arcane Core, 2026-07-10 purge).
    namespace RateLimits
    {
        // [PASTE the Login()/Registration()/Pulls()/AddCurrency() inline functions
        //  verbatim from Arcane/Core/src/Arcane/Net/RateLimiter.hpp]
    }
```

- [ ] **Step 4: Strip Core** — delete from `Arcane/Core/src/Arcane/Util/Logger.hpp`: the `LogCategory` enum, the six named-logger registrations in `Init` (Init keeps: console sink, optional file sink from `logFilePath`, level plumbing; creates NO named loggers eagerly — `Get` is lazy), the enum `Get(LogCategory)` overload, the enum `SetCategoryLevel` overload if present (check server callers first: `rg -n "SetCategoryLevel" <WT>/Server` — if server calls it with the enum, add a matching forwarder on the facade), the four analytics statics, the picosha2 include, the six macro families. Delete the `namespace RateLimits` block from Core `RateLimiter.hpp`. Grep-proof: `rg -n "LogCategory|LogPullEvent|LogAuthEvent|LogSessionEvent|LogConnectionEvent|picosha2|gacha" <WT>/Arcane/Core/src` -> only the Task-1 `Init` default filename remains — remove that default too now: `Init(Level consoleLevel = Level::Info, Level fileLevel = Level::Trace, const std::string& logFilePath = "")`.
- [ ] **Step 5: Adapt ArcaneTests** — in `LoggerTest.cpp`, delete/rewrite any case that calls the moved analytics functions or names `LogCategory` (W1 wrote its escaping proof through them; the escaping proof is now the direct `JsonEscape` case from Task 1). Keep every case that exercises the generic surface. `rg -n "LogPullEvent|LogAuthEvent|LogSessionEvent|LogConnectionEvent|LogCategory" <WT>/Arcane/Tests/src` must end up empty.
- [ ] **Step 6: Server-side analytics test** — create `Server/Common/tests/AnalyticsEventShapeTest.cpp` following the include/style pattern of `Server/Common/tests/RateLimiterSmokeTest.cpp` (read it first). Port the W1 analytics escaping cases (adversarial quotes/backslashes/newlines through `LogPullEvent`/`LogAuthEvent`/`LogSessionEvent`) — assert via a captured spdlog sink or, if the existing server tests have no sink-capture helper, assert on `Aphelyon::Logger::JsonEscape`-composed expected substrings using the same technique the W1 ArcaneTests version used (read the old test before deleting it in Step 5 and port its mechanism).
- [ ] **Step 7: Build + test BOTH workspaces** — Arcane: regen NOT needed (no file add/delete in Arcane), build, `.\ArcaneTests.exe "[logger],[crypto],[ratelimiter],[protocol]"` then full `~[gpu]` ALL PASS (record counts — [logger] case delta expected). Server: regen (new test file globbed), build `Aphelyon.slnx`, run CommonTests, AuthTests, CombatTests from their bin dirs — ALL PASS. Grep-proof zero remaining server references to the Arcane-deleted names outside the facade: `rg -n "Arcane::LogCategory|Arcane::Logger::LogPull" <WT>/Server` -> empty.
- [ ] **Step 8: Commit**

```bash
git add Arcane/Core/src/Arcane/Util/Logger.hpp Arcane/Core/src/Arcane/Net/RateLimiter.hpp Server/Common/src/Util/Logger.hpp Server/Common/src/Net/RateLimiter.hpp Arcane/Tests/src/LoggerTest.cpp Server/Common/tests/AnalyticsEventShapeTest.cpp
git commit -F <msgfile>   # refactor(agnostic): move game/service logging surface + rate-limit presets from Arcane Core to the Aphelyon server facade
```

(If Step 2 required a premake includedirs edit: also stage `Server/premake5.lua`.)

---

### Task 3: Types.hpp relocation — Server shim becomes the real header, Arcane copy deleted

**Files:**
- Modify: `Server/Common/src/Types/Types.hpp` (shim -> full content)
- Delete: `Arcane/Core/src/Arcane/Types/Types.hpp` (and the empty `Types/` dir)
- Modify: `Arcane/Core/src/Arcane/Net/Protocol.hpp` (drop stray include)
- Modify: `Arcane/Core/src/ArcaneCore.cpp` (drop include-list entry)

**Interfaces:**
- Consumes: nothing from earlier tasks (independent).
- Produces: `namespace Aphelyon` now DIRECTLY defines `ItemRarity, ItemType, CharacterArchetype, Element, BannerType, Item, PullResult, PlayerStats, WalletState, PityState, BannerInfo, Color, GetRarityStars, ParseArchetype, ParseElement, GetElementName, GetItemTypeName, GetBannerTypeName, GetRarityColor` — same names/signatures/ENUM VALUES the shim's using-decls exported, so all 7 direct includers and ~29 transitive consumers compile unchanged.

- [ ] **Step 1: Replace the shim content.** Copy the FULL body of `Arcane/Core/src/Arcane/Types/Types.hpp` into `Server/Common/src/Types/Types.hpp`, with exactly these transformations: `namespace Arcane` -> `namespace Aphelyon`; delete the old shim's `#include <Arcane/Types/Types.hpp>` and its entire `using Arcane::...` block; keep/update the file's top comment to say it is the canonical Aphelyon domain-types header (moved back from Arcane Core, 2026-07-10 game-agnostic purge — the M0 extraction took it engine-side by mistake). Enum VALUES stay byte-identical (client `protocol.json` mirrors them).
- [ ] **Step 2: Delete the Arcane original + fix the two stray includes.** `git rm Arcane/Core/src/Arcane/Types/Types.hpp`. In `Arcane/Core/src/Arcane/Net/Protocol.hpp` delete the `#include <Arcane/Types/Types.hpp>` line (verified: zero Types symbols used there). In `Arcane/Core/src/ArcaneCore.cpp` delete its `#include <Arcane/Types/Types.hpp>` line.
- [ ] **Step 3: Grep-proof** — `rg -in "ItemRarity|PullResult|BannerInfo|WalletState|PityState|CharacterArchetype|BannerType|GetRarityStars|ParseArchetype" <WT>/Arcane` -> zero hits. `rg -n "Types/Types.hpp" <WT>/Arcane` -> zero hits.
- [ ] **Step 4: Build + test BOTH workspaces** — Arcane: regen (`premake5.exe vs2026` from `<WT>\Arcane\` — a globbed header was deleted), build, full `.\ArcaneTests.exe "~[gpu]"` ALL PASS (counts unchanged from Task 2 — no test touches these types). Server: regen, build, CommonTests + AuthTests + CombatTests ALL PASS.
- [ ] **Step 5: Commit**

```bash
git add Server/Common/src/Types/Types.hpp Arcane/Core/src/Arcane/Net/Protocol.hpp Arcane/Core/src/ArcaneCore.cpp
git rm Arcane/Core/src/Arcane/Types/Types.hpp   # (already staged by git rm in step 2; listed for clarity)
git commit -F <msgfile>   # refactor(agnostic): relocate gacha domain types from Arcane Core to Server/Common (Aphelyon)
```

---

### Task 4: Exit sweep — comment de-flavoring, grep proof, docs

**Files:**
- Modify: `Arcane/Core/src/Arcane/Crypto/Crypto.hpp` (comments only)
- Modify: `Arcane/Arcane/src/Arcane/Base/Log.hpp` (comment only)
- Modify: `CLAUDE.md` (one line)
- Possibly: any straggler comment sites the sweep finds

**Interfaces:** none — comments/docs only. Must be behavior-neutral (comment-only diff except CLAUDE.md).

- [ ] **Step 1: Sweep.** `rg -in "gacha|banner|pity|wallet|credits|tickets|scrap|aphelyon|quest|login|auth" <WT>/Arcane --glob '!**/bin/**' --glob '!**/generated/**'` and triage every hit against survey-1's (C) list: physics/ECS/console-banner senses stay; genuinely game/service-flavored COMMENTS get de-flavored:
  - `Crypto.hpp`: the "login" flow comment and the two "quest challenge token" / `QuestHandlers::VerifyQuestToken` comment references -> rewrite as generic examples (e.g. "e.g. a short-lived signed challenge token issued by a service"). Code untouched.
  - `Base/Log.hpp` header comment: still contrasts against "Core's server-flavored Logger (which writes logs/gacha_server.log...)" — now stale. Rewrite: "Deliberately separate from Core's Logger (Util/Logger.hpp), which serves engine-agnostic named-category logging for library code; this is the engine runtime's own console logger."
  - `Arcane/premake5.lua` comment about the reserved `Game/` slot for the future Aphelyon client: KEEP (workspace wiring note, not game code).
  - `Tests/data/physics_feel_reference/README.md` mention: KEEP (historical provenance doc).
- [ ] **Step 2: CLAUDE.md** — in the Core-extraction bullet (the strangler paragraph saying "Core contains zero Aphelyon references — keep it that way"), append one sentence: "The 2026-07-10 game-agnostic purge moved the last transplants out: gacha domain types (`Types.hpp`) and the game/service logging surface (categories, analytics events, `gacha_server.log`, `RateLimits` presets) now live in `Server/Common` (namespace `Aphelyon`); Core logs only via the neutral `LOG_CORE_*`/string-keyed `Logger::Get(name)` seam."
- [ ] **Step 3: Final gates** — Arcane: build, full `~[gpu]` ALL PASS (record final counts). Server: build, CommonTests/AuthTests/CombatTests ALL PASS. Final grep proof, expect zero domain hits: `rg -in "ItemRarity|PullResult|BannerInfo|WalletState|PityState|LogPullEvent|LOG_GACHA|gacha" <WT>/Arcane/Core/src <WT>/Arcane/Arcane/src <WT>/Arcane/Loom <WT>/Arcane/Sandbox`.
- [ ] **Step 4: Commit**

```bash
git add Arcane/Core/src/Arcane/Crypto/Crypto.hpp Arcane/Arcane/src/Arcane/Base/Log.hpp CLAUDE.md
git commit -F <msgfile>   # docs(agnostic): de-flavor residual game comments + record the purge in CLAUDE.md
```

---

## Self-Review Notes

- Spec coverage: survey-1 (A) items -> Task 3 (Types), Task 2 (LogPullEvent/LogAuthEvent/LogSessionEvent/LogConnectionEvent, Gacha category+macros, gacha_server.log); survey-1 (B) items -> Task 1 (9 call sites; Protocol stray include lands in Task 3 with the Types deletion), Task 2 (LogCategory enum/macros/Init hardcoding, RateLimits presets). Comments-only residue -> Task 4. Survey-2 risks: R1 (Protocol include ordering) satisfied because Task 3 deletes the include in the same commit as the header; R2 (macros used by generic modules) satisfied by Task 1 running FIRST; R3 filename/category -> Tasks 1+2; R4 no renames performed; R5 enum values pinned by Global Constraint; R6/R7 regen steps included per task.
- Type consistency: facade `Get(LogCategory)` forwards to Task 1's `Get(std::string_view)`; `Init(Level, Level)` facade wraps Task 1's 3-arg `Init`; `JsonEscape` made public in Task 1 and consumed by facade in Task 2.
- Deliberate deviations from the plain skill template: verbatim-move blocks are specified as move-directives with anchors instead of retyped code (house rule: moved text is never retyped — the decomp arc proved this prevents rewrite-class errors). The implementer pastes from the source file, applies only the listed transformations.
