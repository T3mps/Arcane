# Arcane M0 — Workspace, Core Extraction, ThirdParty Vendoring — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `Arcane/` premake workspace (/MD, C++23, Debug/Release/Dist), extract the wire/protocol/crypto layer from `Server/Common` into `Arcane.Core` strangler-style with zero server call-site churn, and vendor the full engine third-party stack with premake wrappers, pinned versions, and smoke tests.

**Architecture:** One new premake workspace at `Arcane/` (beside `Server/`, `Client/`, `Tools/`) containing `Core` (static lib) and `ArcaneTests` (Catch2 + rapidcheck). Core sources move from `Server/Common/src/` to a properly-namespaced include root (`Arcane/Core/src/Arcane/Net/...`, consumed as `#include <Arcane/Net/Protocol.hpp>`) in `namespace Arcane` — **Core contains zero Aphelyon knowledge and is drop-in reusable by other projects**. The server keeps compiling with zero call-site churn via thin shim headers at the old paths (`Server/Common/src/Net/Protocol.hpp` becomes a re-export shim); the Server workspace compiles Core as a second flavor (static CRT). Vendored deps follow the existing `ThirdParty/<name>/premake5.lua` wrapper convention, parameterized so the same wrapper builds /MT for Server and /MD for Arcane without project-file collisions.

**Tech Stack:** premake5 (vendored binary), MSVC VS2026, Catch2 + rapidcheck, NVRHI (+Vulkan-Headers, DirectX-Headers), SDL3 (vcpkg, new `-md` overlay triplet), Astra (already dropped in `ThirdParty/Astra/`, untracked), enkiTS, miniaudio, FreeType, glm, stb, Tracy, DXC + ShaderMake tool binaries.

**Spec:** `docs/superpowers/specs/2026-06-11-engine-architecture-design.md` (M0 bullet of the bring-up order) + `docs/superpowers/specs/2026-06-10-engine-thirdparty-stack-design.md` (the stack table and integration mechanics).

---

## Decisions made by this plan (deviations and interpretations — flagged for review)

1. **M0 projects are `Core` + `ArcaneTests` only.** `Arcane.dll`, `Loom`, `Grimoire`, `Playground`, `Game` arrive in M1+ per the bring-up order. The workspace layout leaves room for them.
2. **`Util/Logger.hpp` and `Util/LruCache.hpp` move to Core** even though the spec's extraction list doesn't name them: `Protocol.hpp`/`Crypto.hpp`/`RateLimiter.hpp` include them, and Core cannot depend on Common (Core is the lower layer). Both are presentation-free.
3. **Server flavor of Core builds C++23, not C++20.** The spec says "Core's server flavor builds C++20 to match Server," but `Server/premake5.lua` builds everything `cppdialect "C++23"` today — the spec's premise is stale. Matching the actual Server dialect is the faithful reading. Astra C++20-compatibility is unaffected (Core doesn't include Astra).
4. **Core is pure; the server adapts via shims.** Moved code lives under the namespaced include root `Arcane/Core/src/Arcane/...` (consumed as `#include <Arcane/Net/Protocol.hpp>`, the Astra convention) in `namespace Arcane`, with **no Aphelyon names, defines, or compat code anywhere in Core** — the engine is reusable by other projects as-is. The old paths in `Server/Common/src/` become thin server-owned shim headers that include the Core header and re-export the names server code uses via explicit using-declarations inside `namespace Aphelyon`. The ~50 server include sites compile unchanged; each shim is deletable independently once its includers migrate to `Arcane::`. The two include roots have no overlapping relative paths, so include resolution is unambiguous. Platform guards in moved files change from `APHELYON_PLATFORM_WINDOWS` to `_WIN32` so no workspace define is needed.
5. **Server workspace gets a separate `ArcaneCore` static-lib project** (not files folded into Common) — visible boundary in the solution, honest dependency direction. Services and test projects add one `links` entry each.
6. **`ThirdParty/README.md` is the license/version record** (the stack spec's `THIRDPARTY-LICENSES.md` gate). The repo already maintains the inventory table there with Version + License columns; a second index would drift. Every new dep gets a row with a pinned version and a `LICENSE` file in its subdir.
7. **Astra vendoring = commit the existing drop minus build artifacts.** Its own `.gitignore` already excludes `bin/`, `bin-int/`, `ide/`, `*.sln`; we add `*.slnx`, delete the `error_list.txt` scratch file, and `git add` the rest (include/, tests/, vendor/, scripts/, .github/, release_notes/, docs). This matches "vendored via subtree (no bin/ide artifacts)". Astra's test suite still runs in its own repo; the Arcane workspace consumes headers only.
8. **Version pins:** where this plan names a tag (enkiTS v1.11, FreeType VER-2-13-3, glm 1.0.1) use it; where upstream is rolling (NVRHI, stb, ShaderMake, DXC releases, Vulkan-/DirectX-Headers, miniaudio, Tracy) take the latest tag/release at vendor time and **record the exact version/SHA in the README row** — that recording step is part of each task.

## File structure

```
Arcane/
├── premake5.lua                      NEW — workspace (Core, ArcaneTests, dep wrappers)
├── GenerateProjects.bat              NEW — thin wrapper → scripts/generate.bat
├── scripts/
│   ├── generate.bat                  NEW — premake5 vs2026 (mirrors Server's)
│   └── setup-vcpkg-deps.bat          NEW — installs sdl3:x64-windows-static-md
├── Core/
│   └── src/
│       ├── ArcaneCore.cpp            NEW — lib anchor TU (GachaCommon.cpp pattern)
│       └── Arcane/                   namespaced include root: #include <Arcane/...>
│           ├── Version.hpp           NEW — Arcane::VersionString()
│           ├── Net/TcpSocket.hpp     MOVED from Server/Common/src/Net/
│           ├── Net/Protocol.hpp      MOVED from Server/Common/src/Net/
│           ├── Net/RateLimiter.hpp   MOVED from Server/Common/src/Net/
│           ├── Crypto/Crypto.hpp     MOVED from Server/Common/src/Crypto/
│           ├── Types/Types.hpp       MOVED from Server/Common/src/Types/
│           ├── Util/Logger.hpp       MOVED from Server/Common/src/Util/
│           └── Util/LruCache.hpp     MOVED from Server/Common/src/Util/
└── Tests/
    └── src/
        ├── test_main.cpp             NEW — Catch::Session main (Server convention)
        ├── VersionTest.cpp           NEW
        ├── FramingTest.cpp           NEW — wire oracle (threading_harness port)
        ├── WireRoundTripTest.cpp     NEW — rapidcheck property
        ├── CryptoSmokeTest.cpp       NEW
        └── VendorSmokeTest.cpp       NEW — Astra/enkiTS/glm/stb/miniaudio/FreeType/NVRHI/SDL3

Server/Common/src/{Net/TcpSocket,Net/Protocol,Net/RateLimiter,Crypto/Crypto,Types/Types,Util/Logger,Util/LruCache}.hpp
                                      REPLACED — thin re-export shims (old paths, server-owned)
Server/premake5.lua                   MODIFIED — ArcaneCore project, includedirs/links
ThirdParty/Catch2/premake5.lua        MODIFIED — location/runtime parameterization
ThirdParty/rapidcheck/premake5.lua    MODIFIED — location/runtime parameterization
ThirdParty/Astra/                     COMMITTED (currently untracked)
ThirdParty/{nvrhi,Vulkan-Headers,DirectX-Headers,enkiTS,miniaudio,freetype,glm,stb,tracy}/  NEW vendor drops
ThirdParty/tools/{dxc,ShaderMake}/    NEW tool binaries
ThirdParty/README.md                  MODIFIED — inventory rows
vcpkg-triplets/x64-windows-static-md.cmake  NEW
CLAUDE.md                             MODIFIED — Arcane section
.gitignore (root)                     MODIFIED — Arcane build outputs, wrapper ide-md/ dirs
```

## Constraints carried into every task

- All new/edited files: **UTF-8 without BOM, ASCII-only comments** (Astra hardening lesson; the spec carries it forward). On PowerShell, never use `Out-File`/`Set-Content` without `-Encoding utf8`; prefer the Write/Edit tools.
- No `/fp:fast` anywhere in the Arcane workspace (MSVC default under our flags is `/fp:precise` — just don't override it).
- **Never run `db-reset.bat`, `clean.bat --deep`, or `docker compose down -v`** as part of any verification step (standing user rule).
- If a build produces "impossible" errors after header moves, do a full rebuild (`msbuild ... /t:Rebuild`) before debugging — stale-object ODR lesson.
- Commit after every task; messages follow the repo's `type(scope):` convention.

---

### Task 1: Parameterize shared ThirdParty wrappers (Catch2, rapidcheck)

Both workspaces will include these wrapper scripts. Today they hardcode `staticruntime "on"` and write their vcxproj into the dep's root dir — the second workspace to generate would overwrite the first's project file with the wrong CRT. Parameterize via globals that default to current behavior so Server/Tools are unaffected.

**Files:**
- Modify: `ThirdParty/Catch2/premake5.lua`
- Modify: `ThirdParty/rapidcheck/premake5.lua`

- [ ] **Step 1: Edit `ThirdParty/Catch2/premake5.lua`**

Replace the project header lines and add a Dist filter. The full resulting file:

```lua
-- Catch2 v3 premake5 build script
-- Boost Software License 1.0 -- modern C++ unit-testing framework
--
-- Included by BOTH the Server (Aphelyon) and Arcane workspaces. The two
-- globals below default to the historical Server behavior; the Arcane
-- workspace overrides them (dynamic CRT, project files in ide-md/) so
-- the workspaces never overwrite each other's generated vcxproj and
-- never collide on lib outputs (outputdir differs per workspace).

project "Catch2"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "src/catch2/**.cpp",
        "src/catch2/**.hpp",
    }

    -- catch_main.cpp provides a default main() that conflicts with
    -- test executables that define their own; exclude it here.
    -- Tests that want the default runner can link Catch2WithDefs separately
    -- (a future task); for now all test executables provide their own main.
    removefiles { "src/catch2/internal/catch_main.cpp" }

    includedirs { "src" }

    defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
```

- [ ] **Step 2: Apply the same three changes to `ThirdParty/rapidcheck/premake5.lua`**

Same pattern: add `location(THIRDPARTY_PROJECT_LOCATION or ".")`, change `staticruntime "on"` to `staticruntime(THIRDPARTY_STATICRUNTIME or "on")`, append an identical `filter "configurations:Dist"` block (runtime Release, optimize on, NDEBUG). Add the same explanatory comment block at the top.

- [ ] **Step 3: Regenerate and rebuild the Server workspace to prove no regression**

```bat
cd Server
GenerateProjects.bat
msbuild Aphelyon.slnx /p:Configuration=Debug /m
```
Expected: build succeeds; `git diff` shows only the two wrapper files changed.

- [ ] **Step 4: Run the existing server test suites (regression baseline for the whole plan)**

```bat
Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe
Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
Server\bin\Debug-windows-x86_64\AuthTests\AuthTests.exe
Server\bin\Debug-windows-x86_64\CombatTests\CombatTests.exe
```
Expected: all pass. Record the test counts — they are the baseline every later task must match. (AccountTests integration tests need the Postgres container up; if `docker compose -f Server/docker-compose.yml ps` shows it down, start it with `scripts\db-setup.bat` — never reset.)

- [ ] **Step 5: Commit**

```bash
git add ThirdParty/Catch2/premake5.lua ThirdParty/rapidcheck/premake5.lua
git commit -m "build(thirdparty): parameterize Catch2/rapidcheck wrappers for multi-workspace use"
```

---

### Task 2: Arcane workspace skeleton

Create the workspace with a trivially-buildable Core and a one-test ArcaneTests. This proves: premake generation, /MD, three configurations, the parameterized wrappers, and the test runner — before any real code moves.

**Files:**
- Create: `Arcane/premake5.lua`
- Create: `Arcane/GenerateProjects.bat`
- Create: `Arcane/scripts/generate.bat`
- Create: `Arcane/Core/src/Arcane/Version.hpp`
- Create: `Arcane/Core/src/ArcaneCore.cpp`
- Create: `Arcane/Tests/src/test_main.cpp`
- Create: `Arcane/Tests/src/VersionTest.cpp`
- Modify: `.gitignore` (root)

- [ ] **Step 1: Write `Arcane/premake5.lua`**

```lua
-- Arcane Engine Workspace
-- premake5 for Visual Studio 2026 (generates Arcane.slnx)
--
-- CRT rule (architecture spec 2026-06-11, decision 3): the entire Arcane
-- workspace builds /MD (dynamic CRT) -- memory crosses the Arcane.dll /
-- Game.dll boundary, so all modules must share one heap. Server/ and
-- Tools/ keep their static-runtime conventions, unaffected.

workspace "Arcane"
    architecture "x64"
    startproject "ArcaneTests"
    configurations { "Debug", "Release", "Dist" }
    multiprocessorcompile "On"

    -- MSVC: /utf-8 ensures source + execution charsets are UTF-8.
    -- Required by fmt 11+ (bundled in spdlog 1.17).
    filter "system:windows"
        buildoptions { "/utf-8" }
    filter {}

    -- "-md" suffix keeps ThirdParty wrapper outputs (each dep builds into
    -- bin/ under its own dir) separate from the static-CRT flavors the
    -- Server workspace builds from the same wrapper scripts.
    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"

    -- Shared ThirdParty wrappers read these; defaults preserve the
    -- Server/Tools behavior (static CRT, vcxproj in the dep root).
    THIRDPARTY_STATICRUNTIME    = "off"
    THIRDPARTY_PROJECT_LOCATION = "ide-md"

    IncludeDir = {}
    IncludeDir["Core"]             = "%{wks.location}/Core/src"
    IncludeDir["nlohmann"]         = "%{wks.location}/../ThirdParty/nlohmann"
    IncludeDir["picosha2"]         = "%{wks.location}/../ThirdParty/picosha2"
    IncludeDir["spdlog"]           = "%{wks.location}/../ThirdParty/spdlog/include"
    IncludeDir["Catch2"]           = "%{wks.location}/../ThirdParty/Catch2/src"
    IncludeDir["rapidcheck"]       = "%{wks.location}/../ThirdParty/rapidcheck/include"
    IncludeDir["rapidcheck_catch"] = "%{wks.location}/../ThirdParty/rapidcheck/extras/catch/include"

group "Dependencies"
    include "../ThirdParty/Catch2"
    include "../ThirdParty/rapidcheck"
group ""

-- ============================================================================
-- Core: Arcane.Core static lib (presentation-free; also compiled by the
-- Server workspace as project "ArcaneCore" with static CRT)
-- ============================================================================
project "Core"
    location "Core"
    kind "StaticLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"

-- ============================================================================
-- ArcaneTests: Catch2 + rapidcheck (Server conventions). Links Core
-- directly -- Core links into exactly ONE module per process.
-- ============================================================================
project "ArcaneTests"
    location "Tests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
    }

    includedirs {
        "%{IncludeDir.Core}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Catch2}",
        "%{IncludeDir.rapidcheck}",
        "%{IncludeDir.rapidcheck_catch}",
    }

    links { "Core", "Catch2", "rapidcheck" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        links { "ws2_32" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"
```

- [ ] **Step 2: Write `Arcane/scripts/generate.bat`**

Copy `Server/scripts/generate.bat` and adapt: same premake resolution (`%PROJECT_ROOT%\..\ThirdParty\premake5\premake5.exe`), same `premake5 vs2026` invocation from `%PROJECT_ROOT%`. Drop the vcpkg check for now (vcpkg becomes required in Task 12 — that task re-adds the check). Change banner text to "Arcane Engine Project Generation" and the success message to "Open Arcane.slnx".

- [ ] **Step 3: Write `Arcane/GenerateProjects.bat`**

```bat
@echo off
:: Thin wrapper -- delegates to scripts\generate.bat
call "%~dp0scripts\generate.bat" %*
```

- [ ] **Step 4: Write `Arcane/Core/src/Arcane/Version.hpp`**

(`Core/src` is the include-path root; everything Core exports lives under the `Arcane/` subdirectory so consumers write `#include <Arcane/Version.hpp>` — the Astra `include/Astra/...` convention, and the reason a future project can lift Core wholesale.)

```cpp
#pragma once

// Arcane engine version. Bumped manually at milestone boundaries for now;
// the plugin ABI version (M4) is a separate constant by design.

namespace Arcane
{
    inline constexpr int kVersionMajor = 0;
    inline constexpr int kVersionMinor = 1;

    inline const char* VersionString() { return "Arcane 0.1 (M0)"; }
}
```

- [ ] **Step 5: Write `Arcane/Core/src/ArcaneCore.cpp`**

```cpp
// ArcaneCore.cpp
// Ensures the static library produces a .lib file.
// All Core code is in headers; this file provides a compiled translation unit.
#include <Arcane/Version.hpp>
```

(Task 3 extends the include list with the moved headers.)

- [ ] **Step 6: Write `Arcane/Tests/src/test_main.cpp`**

```cpp
// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/Tests/src; premake picks them up
// via the wildcard glob.

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
```

- [ ] **Step 7: Write `Arcane/Tests/src/VersionTest.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <Arcane/Version.hpp>

TEST_CASE("Core links and reports a version", "[core]")
{
    REQUIRE(Arcane::kVersionMajor == 0);
    REQUIRE(std::strlen(Arcane::VersionString()) > 0);
}
```

- [ ] **Step 8: Update the root `.gitignore`**

First check what the existing patterns already cover:

```bat
git check-ignore -v Server/bin/probe ThirdParty/Catch2/bin/probe Server/Aphelyon.slnx
```

Mirror whatever mechanism covers Server outputs for the Arcane equivalents. If the existing patterns are unanchored (`bin/`, `bin-int/`, `*.slnx`, `*.vcxproj*`) nothing may be needed; otherwise append:

```
Arcane/bin/
Arcane/bin-int/
Arcane/Arcane.slnx
Arcane/**/*.vcxproj*
ThirdParty/*/ide-md/
```

Verify with `git check-ignore -v Arcane/bin/probe ThirdParty/Catch2/ide-md/probe` after editing.

- [ ] **Step 9: Generate and build (all three configurations)**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
```
Expected: all three succeed. Spot-check the CRT: `dumpbin /directives Arcane\bin\Debug-windows-x86_64-md\Core\Core.lib | findstr DEFAULTLIB` should show `MSVCRTD` (not `LIBCMTD`).

- [ ] **Step 10: Run the test runner**

```bat
Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Expected: `All tests passed (2 assertions in 1 test case)`.

- [ ] **Step 11: Regenerate Server and confirm no cross-contamination**

```bat
cd Server
GenerateProjects.bat
msbuild Aphelyon.slnx /p:Configuration=Debug /m
```
Expected: succeeds. This proves the wrapper parameterization keeps the two workspaces' generated files disjoint (Server writes `ThirdParty/Catch2/Catch2.vcxproj`; Arcane writes `ThirdParty/Catch2/ide-md/Catch2.vcxproj`).

- [ ] **Step 12: Commit**

```bash
git add Arcane/ .gitignore
git commit -m "feat(arcane): M0 workspace skeleton - Core + ArcaneTests, /MD, Debug/Release/Dist"
```

---

### Task 3: Core extraction from Server/Common (the heart of M0)

Move seven headers into Core's namespaced include root, rename to `namespace Arcane` with **zero Aphelyon residue in Core**, and keep the server compiling via thin re-export shims at the old paths. One atomic commit — the repo must build on both sides at every commit.

**Files:**
- Move: `Server/Common/src/Net/TcpSocket.hpp` → `Arcane/Core/src/Arcane/Net/TcpSocket.hpp`
- Move: `Server/Common/src/Net/Protocol.hpp` → `Arcane/Core/src/Arcane/Net/Protocol.hpp`
- Move: `Server/Common/src/Net/RateLimiter.hpp` → `Arcane/Core/src/Arcane/Net/RateLimiter.hpp`
- Move: `Server/Common/src/Crypto/Crypto.hpp` → `Arcane/Core/src/Arcane/Crypto/Crypto.hpp`
- Move: `Server/Common/src/Types/Types.hpp` → `Arcane/Core/src/Arcane/Types/Types.hpp`
- Move: `Server/Common/src/Util/Logger.hpp` → `Arcane/Core/src/Arcane/Util/Logger.hpp`
- Move: `Server/Common/src/Util/LruCache.hpp` → `Arcane/Core/src/Arcane/Util/LruCache.hpp`
- Create: seven shim headers at the old `Server/Common/src/...` paths
- Modify: `Arcane/Core/src/ArcaneCore.cpp`
- Modify: `Server/premake5.lua`

NOT moving (stay in Common, per spec): `SessionCache`, `ServiceEndpoint`, `ServiceClient`, `InternalRpcAuth`, `TcpServerBase`, `RpcFraming`, `MessageDispatcher`, `StripedMutex`, `UuidV7`, `IdempotencyKey`, all of `State/`, `Db/`, `Persistence/`, and the remaining `Types/` headers (`GearTypes`, `CombatStats`, `QuestTypes`, `QuestDefinition`).

- [ ] **Step 1: Move the files with git mv (preserves history)**

```bash
mkdir -p Arcane/Core/src/Arcane/Net Arcane/Core/src/Arcane/Crypto Arcane/Core/src/Arcane/Types Arcane/Core/src/Arcane/Util
git mv Server/Common/src/Net/TcpSocket.hpp    Arcane/Core/src/Arcane/Net/TcpSocket.hpp
git mv Server/Common/src/Net/Protocol.hpp     Arcane/Core/src/Arcane/Net/Protocol.hpp
git mv Server/Common/src/Net/RateLimiter.hpp  Arcane/Core/src/Arcane/Net/RateLimiter.hpp
git mv Server/Common/src/Crypto/Crypto.hpp    Arcane/Core/src/Arcane/Crypto/Crypto.hpp
git mv Server/Common/src/Types/Types.hpp      Arcane/Core/src/Arcane/Types/Types.hpp
git mv Server/Common/src/Util/Logger.hpp      Arcane/Core/src/Arcane/Util/Logger.hpp
git mv Server/Common/src/Util/LruCache.hpp    Arcane/Core/src/Arcane/Util/LruCache.hpp
```

- [ ] **Step 2: Rename the namespace and fix internal includes in each moved file**

In each of the seven files:
1. `namespace Aphelyon` → `namespace Arcane` (each file declares it exactly once; also fix the closing-brace comment `// namespace Aphelyon`).
2. Every `Aphelyon::` qualification inside the file → `Arcane::`. The only file with these is `Util/Logger.hpp` — its `LOG_*` macro bodies say `::Aphelyon::Logger::Get(::Aphelyon::LogCategory::...)`; they become `::Arcane::Logger::Get(::Arcane::LogCategory::...)`. (Macro call sites in server code are unaffected — the expansion is fully qualified.)
3. Cross-includes between moved headers switch to the namespaced form:
   - `Protocol.hpp`: `#include "Util/Logger.hpp"` → `#include <Arcane/Util/Logger.hpp>`, `"Types/Types.hpp"` → `<Arcane/Types/Types.hpp>`, `"Net/TcpSocket.hpp"` → `<Arcane/Net/TcpSocket.hpp>` (`"Json.hpp"` stays — that's ThirdParty/nlohmann).
   - `Crypto.hpp`: `"Util/Logger.hpp"` → `<Arcane/Util/Logger.hpp>` (`"picosha2.hpp"` stays).
   - `RateLimiter.hpp`: `"Util/Logger.hpp"` → `<Arcane/Util/Logger.hpp>`, `"Util/LruCache.hpp"` → `<Arcane/Util/LruCache.hpp>`.
4. `#ifdef APHELYON_PLATFORM_WINDOWS` (and `#ifndef`/`#if defined` variants) → `_WIN32` (occurs in `TcpSocket.hpp`; `Crypto.hpp` already uses `_WIN32`; check the others). The define came from the Server premake; `_WIN32` is compiler-provided so both workspaces work with no build-system support.

- [ ] **Step 3: Verify Core purity — zero Aphelyon residue**

```bash
grep -rni "aphelyon" Arcane/Core/
```
Expected: **no matches at all.** Core must contain no Aphelyon names, defines, or comments referencing the server — this is what makes it liftable into another project. (Comments mentioning audit IDs etc. are fine; the literal string "aphelyon" is not.)

- [ ] **Step 4: Extend `Arcane/Core/src/ArcaneCore.cpp`**

```cpp
// ArcaneCore.cpp
// Ensures the static library produces a .lib file.
// All Core code is in headers; this file provides a compiled translation unit.
#include <Arcane/Version.hpp>
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Net/Protocol.hpp>
#include <Arcane/Net/RateLimiter.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Types/Types.hpp>
#include <Arcane/Util/Logger.hpp>
#include <Arcane/Util/LruCache.hpp>
```

- [ ] **Step 5: Build the Arcane side**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Expected: builds clean, VersionTest still passes. (Compile errors here mean a missed rename or include fix in Step 2 — the Arcane workspace has no shims, so any surviving `Aphelyon::` or old relative include fails loudly. That is the point.)

- [ ] **Step 6: Create the seven server-side shim headers at the old paths**

Each shim includes the Core header and re-exports, via explicit using-declarations inside `namespace Aphelyon`, every namespace-scope name the original header declared. The shims are server-owned adaptation code — Core knows nothing about them. A missed name fails the server build with that exact identifier, so the lists are self-correcting; enumerate names mechanically with:

```bash
grep -nE "^    (enum class|class|struct|inline|using)" Arcane/Core/src/Arcane/<file>
```

`Server/Common/src/Net/TcpSocket.hpp`:

```cpp
#pragma once

// Strangler shim (M0, 2026-06-11): the real header moved to
// <Arcane/Net/TcpSocket.hpp> (Arcane.Core). This shim re-exports the
// names existing server code uses into namespace Aphelyon. New server
// code should include the Arcane header and use Arcane:: directly.
// Delete this file once all includers migrate.
#include <Arcane/Net/TcpSocket.hpp>

// SocketType, INVALID_SOCK, CloseSocket are global-scope in the Arcane
// header and need no re-export.
namespace Aphelyon
{
    namespace ServerConfig = Arcane::ServerConfig;

    using Arcane::SetSocketTimeout;
    using Arcane::EnableTcpKeepAlive;
    using Arcane::IsSocketTimeoutError;
    using Arcane::IsDisconnectError;
    using Arcane::SendAll;
    using Arcane::InitSockets;
    using Arcane::CleanupSockets;
    using Arcane::CreateListenSocket;
    using Arcane::ConnectSocket;
    using Arcane::ConnectSocketWithTimeout;
    using Arcane::LengthFrameResult;
    using Arcane::ExtractLengthFramed;
}
```

`Server/Common/src/Net/Protocol.hpp` (same shim comment header on this and all below):

```cpp
#pragma once
#include <Arcane/Net/Protocol.hpp>

namespace Aphelyon
{
    using Arcane::MsgId;
    using Arcane::kInvalidMsgId;
    using Arcane::Json;
    using Arcane::ParseJsonSafe;
    using Arcane::ParseJsonStrict;
    using Arcane::ProtocolLoader;
    using Arcane::Message;
    using Arcane::IsRequestMessage;
    using Arcane::IsResponseMessage;
    using Arcane::RequiresAuthentication;
}
```

`Server/Common/src/Net/RateLimiter.hpp`:

```cpp
#pragma once
#include <Arcane/Net/RateLimiter.hpp>

namespace Aphelyon
{
    using Arcane::g_rateLimitingEnabled;
    using Arcane::RateLimiter;
}
```

`Server/Common/src/Crypto/Crypto.hpp`:

```cpp
#pragma once
#include <Arcane/Crypto/Crypto.hpp>

namespace Aphelyon
{
    using Arcane::Crypto;
}
```

`Server/Common/src/Util/LruCache.hpp`:

```cpp
#pragma once
#include <Arcane/Util/LruCache.hpp>

namespace Aphelyon
{
    using Arcane::LruCache;
}
```

`Server/Common/src/Util/Logger.hpp`:

```cpp
#pragma once
#include <Arcane/Util/Logger.hpp>

// LOG_* macros come through unchanged (they expand to ::Arcane::Logger).
namespace Aphelyon
{
    using Arcane::Level;
    using Arcane::LogCategory;
    using Arcane::Logger;
    // Re-export any free helper functions the header declares at
    // namespace scope (e.g. session-event logging) -- enumerate with
    // the grep above and add a using-declaration per name.
}
```

`Server/Common/src/Types/Types.hpp`:

```cpp
#pragma once
#include <Arcane/Types/Types.hpp>

namespace Aphelyon
{
    using Arcane::ItemRarity;
    using Arcane::ItemType;
    using Arcane::CharacterArchetype;
    using Arcane::Element;
    using Arcane::BannerType;
    using Arcane::Item;
    // Complete from the grep: one using-declaration per enum/struct/
    // inline function declared at namespace scope in the Arcane header.
}
```

- [ ] **Step 7: Check for ADL helpers on moved types**

```bash
grep -rn "to_json\|from_json" Server/ --include=*.hpp --include=*.cpp | grep -vi test
```
If any `to_json`/`from_json` (or other operator/free-function overloads) in server code take a moved type (`Item`, `Message`, ...) as a parameter, ADL will no longer find them from `namespace Aphelyon` — move that overload into `namespace Arcane` is wrong (server code in engine namespace); instead keep it in `namespace Aphelyon` and qualify call sites, or relocate the helper next to the type in Core if it's genuinely type-intrinsic. Expected: none exist (server serialization is hand-rolled per handler) — investigate any hit before proceeding.

- [ ] **Step 8: Rewire `Server/premake5.lua`**

Three edits (no workspace-level define — Core needs no activation):

**(a)** IncludeDir table — add after the `IncludeDir["Common"]` line:

```lua
    IncludeDir["ArcaneCore"] = "%{wks.location}/../Arcane/Core/src"
```

**(b)** New project, placed immediately before `project "Common"`:

```lua
-- ============================================================================
-- ArcaneCore: server-flavor build of Arcane/Core/src (static CRT, C++23).
-- Same sources the Arcane workspace builds /MD; the two flavors never meet
-- in one process. Contains: wire framing (TcpSocket), Protocol/ProtocolLoader,
-- RateLimiter, Crypto, Types, Logger, LruCache.
-- ============================================================================
project "ArcaneCore"
    location "ArcaneCore"
    kind "StaticLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "on"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{wks.location}/../Arcane/Core/src/**.hpp",
        "%{wks.location}/../Arcane/Core/src/**.cpp",
    }

    includedirs {
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING"
    }

    filter "system:windows"
        systemversion "latest"
        defines { "APHELYON_PLATFORM_WINDOWS" }
        buildoptions { "/Zc:__cplusplus", "/bigobj" }

    filter "configurations:Debug"
        defines { "APHELYON_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "APHELYON_RELEASE" }
        runtime "Release"
        optimize "on"
```

**(c)** Wire consumers. In **each** of: `Common`, `Auth`, `Account`, `Combat`, `AccountTests`, and the `aphelyon_test_project` function body:
- add `"%{IncludeDir.ArcaneCore}",` to `includedirs` (right after `"%{IncludeDir.Common}",` where present, else first).
- For every project that has a `links` line (`Auth`, `Account`, `Combat`, `AccountTests`, `aphelyon_test_project`): change `links { "Common", ... }` to `links { "Common", "ArcaneCore", ... }`. `Common` itself is a static lib — no links change, includedirs only.

- [ ] **Step 9: Build the Server side and run all four suites**

```bat
cd Server
GenerateProjects.bat
msbuild Aphelyon.slnx /p:Configuration=Debug /m
Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe
Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
Server\bin\Debug-windows-x86_64\AuthTests\AuthTests.exe
Server\bin\Debug-windows-x86_64\CombatTests\CombatTests.exe
```
Expected: builds clean; test counts match the Task 1 baseline exactly. Also build Release: `msbuild Aphelyon.slnx /p:Configuration=Release /m`.

Likely failure modes and fixes:
- `'X': undeclared identifier` or `'Aphelyon::X' not found` where X is a moved name → the relevant shim is missing a using-declaration for X; add it to the shim (never touch Core).
- Ambiguous-name errors → a server header independently declares a name a shim re-exports; resolve by qualifying the server use site (report it — none are expected).
- `cannot open include file 'Arcane/...'` → that project is missing the `IncludeDir.ArcaneCore` entry from edit (c).

- [ ] **Step 10: Boot-smoke the services**

```bat
cd Server
scripts\start-all.bat
```
Wait ~5 s, confirm all three consoles show startup banners with no protocol-load errors, then `scripts\stop-all.bat`.

- [ ] **Step 11: Commit**

```bash
git add -A Arcane/Core Server/Common Server/premake5.lua
git commit -m "feat(arcane): extract wire framing, Protocol, Types, Crypto, RateLimiter to Arcane.Core

Strangler extraction per the 2026-06-11 architecture spec. Sources live
under the namespaced include root Arcane/Core/src/Arcane (consumed as
<Arcane/...>) in namespace Arcane with zero Aphelyon residue -- Core is
liftable into other projects as-is. Server compiles unchanged via thin
re-export shims at the old Server/Common paths; each shim is deletable
once its includers migrate. Logger + LruCache move too (hard deps of
Protocol/Crypto/RateLimiter). Server workspace compiles the same sources
as static-CRT project ArcaneCore."
```

---

### Task 4: Wire oracle + Crypto smoke in ArcaneTests

Port the `threading_harness` stream-buffer framing assertions (`Client/src/tests/threading_harness/main.lua:40-72`) as Catch2 characterization tests against `ExtractLengthFramed` + `Message`, add the audit-derived edge cases, and a rapidcheck round-trip property. Note: the Lua `extractMessage` returns the frame *including* the length prefix; the C++ `ExtractLengthFramed` returns body only — the oracle ports semantically (need_more / error / drain / glue / partial-completion behavior), not byte-for-byte.

**Files:**
- Create: `Arcane/Tests/src/FramingTest.cpp`
- Create: `Arcane/Tests/src/WireRoundTripTest.cpp`
- Create: `Arcane/Tests/src/CryptoSmokeTest.cpp`

- [ ] **Step 1: Write `Arcane/Tests/src/FramingTest.cpp`**

```cpp
// Core wire oracle: characterization tests for the LENGTH:BODY\n framing,
// ported from the Lua threading_harness stream_buffer assertions
// (Client/src/tests/threading_harness/main.lua) plus the audit-derived
// edge cases baked into ExtractLengthFramed (M-V4-5 partial-parse prefix).

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Net/Protocol.hpp>

using Arcane::ExtractLengthFramed;
using Arcane::Message;

TEST_CASE("framing: empty buffer needs more data", "[wire]")
{
    auto r = ExtractLengthFramed("");
    REQUIRE(r.needMoreData);
    REQUIRE_FALSE(r.error);
}

TEST_CASE("framing: short prefix without colon needs more data", "[wire]")
{
    auto r = ExtractLengthFramed("12");
    REQUIRE(r.needMoreData);
    REQUIRE_FALSE(r.error);
}

TEST_CASE("framing: long garbage without colon is an error", "[wire]")
{
    auto r = ExtractLengthFramed("12345678901"); // > 10 bytes, no colon
    REQUIRE(r.error);
}

TEST_CASE("framing: one complete frame parses, body excludes newline", "[wire]")
{
    auto r = ExtractLengthFramed("8:1|tok|{}\n");
    REQUIRE_FALSE(r.error);
    REQUIRE_FALSE(r.needMoreData);
    REQUIRE(r.body == "1|tok|{}");
    REQUIRE(r.consumed == 11);
}

TEST_CASE("framing: two glued frames extract sequentially and drain", "[wire]")
{
    std::string buf = "8:1|tok|{}\n8:2|tok|{}\n";
    auto r1 = ExtractLengthFramed(buf);
    REQUIRE(r1.body == "1|tok|{}");
    buf.erase(0, r1.consumed);

    auto r2 = ExtractLengthFramed(buf);
    REQUIRE(r2.body == "2|tok|{}");
    buf.erase(0, r2.consumed);

    auto r3 = ExtractLengthFramed(buf);
    REQUIRE(r3.needMoreData); // drained
}

TEST_CASE("framing: non-numeric length prefix is an error", "[wire]")
{
    auto r = ExtractLengthFramed("notanum:bogus\n");
    REQUIRE(r.error);
}

TEST_CASE("framing: partially-numeric prefix is rejected (audit M-V4-5)", "[wire]")
{
    auto r = ExtractLengthFramed("12abc:somebodybytes\n");
    REQUIRE(r.error);
}

TEST_CASE("framing: partial frame stays buffered, completes on append", "[wire]")
{
    std::string buf = "8:1|tok|"; // 8 bytes promised, 5 of body present
    auto r1 = ExtractLengthFramed(buf);
    REQUIRE(r1.needMoreData);

    buf += "{}\n";
    auto r2 = ExtractLengthFramed(buf);
    REQUIRE(r2.body == "1|tok|{}");
}

TEST_CASE("framing: length above maxBodySize is rejected", "[wire]")
{
    auto r = ExtractLengthFramed("99:abc\n", /*maxBodySize=*/16);
    REQUIRE(r.error);
}

TEST_CASE("message: serialize -> extract -> parse round-trips", "[wire]")
{
    Message m;
    m.type    = 42;
    m.token   = std::string(64, 'a');
    m.payload = R"({"key":"va|ue with pipe"})"; // '|' in payload is legal

    auto framed = ExtractLengthFramed(m.Serialize());
    REQUIRE_FALSE(framed.error);

    Message back = Message::ParseBody(framed.body);
    REQUIRE(back.type == m.type);
    REQUIRE(back.token == m.token);
    REQUIRE(back.payload == m.payload);
}
```

- [ ] **Step 2: Write `Arcane/Tests/src/WireRoundTripTest.cpp`**

```cpp
// Property: any (type, token, payload) survives Serialize -> frame-extract
// -> ParseBody. Token is pipe-free by protocol construction (64 hex chars
// in production); payload may contain pipes and newlines -- the length
// prefix disambiguates.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck/catch.h>
#include <string>
#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Net/Protocol.hpp>

using Arcane::ExtractLengthFramed;
using Arcane::Message;

TEST_CASE("wire round-trip property", "[wire][property]")
{
    rc::prop("serialize/extract/parse preserves all fields",
             [](uint16_t type, const std::string& payload) {
        RC_PRE(type > 0);
        RC_PRE(payload.size() < 4096);

        Message m;
        m.type    = type;
        m.token   = std::string(64, 'f');
        m.payload = payload;

        auto framed = ExtractLengthFramed(m.Serialize());
        RC_ASSERT(!framed.error);
        RC_ASSERT(!framed.needMoreData);

        Message back = Message::ParseBody(framed.body);
        RC_ASSERT(back.type == m.type);
        RC_ASSERT(back.token == m.token);
        RC_ASSERT(back.payload == m.payload);
    });
}
```

- [ ] **Step 3: Write `Arcane/Tests/src/CryptoSmokeTest.cpp`**

```cpp
// Crypto smoke: proves the moved Crypto compiles, links bcrypt via its
// pragma, and round-trips a password at the current iteration setting.

#include <catch2/catch_test_macros.hpp>
#include <Arcane/Crypto/Crypto.hpp>

TEST_CASE("crypto: password hash round-trips", "[crypto]")
{
    const std::string hash = Arcane::Crypto::HashPassword("correct horse battery staple");
    REQUIRE(Arcane::Crypto::VerifyPassword("correct horse battery staple", hash));
    REQUIRE_FALSE(Arcane::Crypto::VerifyPassword("wrong password", hash));
}
```

(If `VerifyPassword` has a different exact name, crib it from `Server/Auth/src/SessionManager.hpp` usage — but the Crypto.hpp comments name `VerifyPassword`, `NeedsRehash`, `IterationsOfStoredHash`.)

- [ ] **Step 4: Build and run**

```bat
cd Arcane
msbuild Arcane.slnx /p:Configuration=Debug /m
Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```
Expected: all test cases pass (the implementations already exist — these are characterization tests pinning behavior; a failure means the port found a real semantic difference and must be investigated, not adjusted away).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Tests
git commit -m "test(arcane): wire-framing oracle (threading_harness port) + crypto smoke"
```

---

### Task 5: Vendor header-only deps — glm, stb, miniaudio

**Files:**
- Create: `ThirdParty/glm/` (vendor drop: `glm/` header tree + `copying.txt` as `LICENSE`)
- Create: `ThirdParty/stb/stb_image.h`, `ThirdParty/stb/stb_image_write.h`, `ThirdParty/stb/LICENSE`
- Create: `ThirdParty/miniaudio/miniaudio.h`, `ThirdParty/miniaudio/LICENSE`
- Modify: `Arcane/premake5.lua` (IncludeDirs + test includedirs)
- Create: `Arcane/Tests/src/VendorSmokeTest.cpp`
- Modify: `ThirdParty/README.md`

- [ ] **Step 1: Download and vendor (record exact versions)**

```powershell
# glm 1.0.1
git clone --depth 1 --branch 1.0.1 https://github.com/g-truc/glm "$env:TEMP\glm"
New-Item -ItemType Directory -Force ThirdParty\glm
Copy-Item -Recurse "$env:TEMP\glm\glm" ThirdParty\glm\glm
Copy-Item "$env:TEMP\glm\copying.txt" ThirdParty\glm\LICENSE

# stb (rolling; record the SHA)
git clone --depth 1 https://github.com/nothings/stb "$env:TEMP\stb"
git -C "$env:TEMP\stb" rev-parse --short HEAD   # record this SHA for the README row
New-Item -ItemType Directory -Force ThirdParty\stb
Copy-Item "$env:TEMP\stb\stb_image.h","$env:TEMP\stb\stb_image_write.h" ThirdParty\stb\
Copy-Item "$env:TEMP\stb\LICENSE" ThirdParty\stb\LICENSE

# miniaudio (latest 0.11.x tag; record it)
git clone --depth 1 https://github.com/mackron/miniaudio "$env:TEMP\miniaudio"
git -C "$env:TEMP\miniaudio" describe --tags    # record for the README row
New-Item -ItemType Directory -Force ThirdParty\miniaudio
Copy-Item "$env:TEMP\miniaudio\miniaudio.h" ThirdParty\miniaudio\
Copy-Item "$env:TEMP\miniaudio\LICENSE" ThirdParty\miniaudio\LICENSE
```

- [ ] **Step 2: Add IncludeDirs to `Arcane/premake5.lua`**

```lua
    IncludeDir["glm"]       = "%{wks.location}/../ThirdParty/glm"
    IncludeDir["stb"]       = "%{wks.location}/../ThirdParty/stb"
    IncludeDir["miniaudio"] = "%{wks.location}/../ThirdParty/miniaudio"
```
and add `"%{IncludeDir.glm}", "%{IncludeDir.stb}", "%{IncludeDir.miniaudio}",` to the ArcaneTests `includedirs`.

- [ ] **Step 3: Create `Arcane/Tests/src/VendorSmokeTest.cpp`**

This file accumulates one section per vendored dep across Tasks 5-11. Initial content:

```cpp
// Vendored-dependency smoke tests. One test case per ThirdParty dep proving
// it compiles, links, and does one real operation headlessly (no window,
// no GPU, no audio device). Grown task-by-task during M0; the Playground
// (M3) is the *integration* test -- these are arrival gates.

#include <catch2/catch_test_macros.hpp>

// ---------------------------------------------------------------- glm
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("glm: vector and matrix math", "[vendor][glm]")
{
    glm::vec3 a(1.0f, 0.0f, 0.0f);
    glm::vec3 b(0.0f, 1.0f, 0.0f);
    REQUIRE(glm::dot(a, b) == 0.0f);
    REQUIRE(glm::cross(a, b) == glm::vec3(0.0f, 0.0f, 1.0f));
}

// ---------------------------------------------------------------- stb
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <cstring>
#include <vector>

namespace {
    void CollectPng(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        out->insert(out->end(), static_cast<unsigned char*>(data),
                    static_cast<unsigned char*>(data) + size);
    }
}

TEST_CASE("stb: PNG write -> read round-trip in memory", "[vendor][stb]")
{
    const unsigned char pixels[2 * 2 * 4] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    std::vector<unsigned char> png;
    REQUIRE(stbi_write_png_to_func(&CollectPng, &png, 2, 2, 4, pixels, 2 * 4) != 0);

    int w = 0, h = 0, comp = 0;
    unsigned char* decoded = stbi_load_from_memory(png.data(), (int)png.size(),
                                                   &w, &h, &comp, 4);
    REQUIRE(decoded != nullptr);
    REQUIRE(w == 2);
    REQUIRE(h == 2);
    REQUIRE(std::memcmp(decoded, pixels, sizeof(pixels)) == 0);
    stbi_image_free(decoded);
}

// ---------------------------------------------------------------- miniaudio
// Implementation TU lives in MiniaudioImpl.cpp to keep this file readable.
extern "C" const char* ma_version_string(void);

TEST_CASE("miniaudio: implementation links and reports a version", "[vendor][miniaudio]")
{
    const char* v = ma_version_string();
    REQUIRE(v != nullptr);
    REQUIRE(v[0] == '0'); // 0.11.x series
}
```

- [ ] **Step 4: Create `Arcane/Tests/src/MiniaudioImpl.cpp`**

```cpp
// miniaudio implementation TU. MA_NO_DEVICE_IO is deliberately NOT set --
// the engine needs device IO in M2+; this TU proves the full implementation
// compiles. No device is opened by the smoke test.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
```

- [ ] **Step 5: Build and run**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[vendor]"
```
Expected: glm, stb, miniaudio cases pass.

- [ ] **Step 6: Add README inventory rows**

In `ThirdParty/README.md`, add alphabetically (with the actual recorded versions):

```
| **glm** | 1.0.1 | MIT | Arcane | Math (vectors/matrices) | https://github.com/g-truc/glm |
| **miniaudio** | <recorded> | MIT-0 | Arcane | Audio engine (single header) | https://github.com/mackron/miniaudio |
| **stb** | <recorded SHA> | MIT/Public Domain | Arcane | stb_image + stb_image_write | https://github.com/nothings/stb |
```

- [ ] **Step 7: Commit**

```bash
git add ThirdParty/glm ThirdParty/stb ThirdParty/miniaudio ThirdParty/README.md Arcane/
git commit -m "build(thirdparty): vendor glm, stb, miniaudio with smoke tests"
```

---

### Task 6: Commit the Astra vendor drop + registry smoke test

`ThirdParty/Astra/` is already on disk but untracked (full repo dump from the hardening workstream). Its own `.gitignore` excludes `bin/`, `bin-int/`, `ide/`, `*.sln`. Commit it as the vendored copy.

**Files:**
- Modify: `ThirdParty/Astra/.gitignore` (add `*.slnx`)
- Delete: `ThirdParty/Astra/error_list.txt` (build-error scratch — verify it's scratch before deleting)
- Commit: everything else under `ThirdParty/Astra/`
- Modify: `Arcane/premake5.lua`, `ThirdParty/README.md`
- Modify: `Arcane/Tests/src/VendorSmokeTest.cpp`

- [ ] **Step 1: Clean the drop**

```powershell
Get-Content ThirdParty\Astra\error_list.txt -TotalCount 5   # confirm it's compiler-error scratch
Remove-Item ThirdParty\Astra\error_list.txt -Confirm:$false
Add-Content -Encoding ascii ThirdParty\Astra\.gitignore "`n# Generated solution (vendored copy)`n*.slnx"
```

- [ ] **Step 2: Stage and verify nothing ignored leaks in**

```bash
git add ThirdParty/Astra
git status --short ThirdParty/Astra | head -50
git diff --cached --stat ThirdParty/Astra | tail -5
```
Expected: staged paths cover `include/`, `tests/`, `benchmark/`, `vendor/`, `scripts/`, `.github/`, `release_notes/`, `LICENSE`, `README.md`, `CLAUDE.md`, `premake5.lua`, `.gitignore` — and **zero** paths under `bin/`, `bin-int/`, `ide/`, no `.sln`/`.slnx`/`.vcxproj`, no `.claude/`. If `.claude/settings.local.json` got staged, `git restore --staged ThirdParty/Astra/.claude` and add `.claude/` to the Astra `.gitignore`.

- [ ] **Step 3: Wire into the Arcane workspace**

In `Arcane/premake5.lua`:

```lua
    IncludeDir["Astra"] = "%{wks.location}/../ThirdParty/Astra/include"
```
and add `"%{IncludeDir.Astra}",` to ArcaneTests `includedirs`. (Header-only — no wrapper project; Astra's own premake5.lua is its repo's workspace and is NOT included here.)

- [ ] **Step 4: Append the Astra smoke to `VendorSmokeTest.cpp`**

```cpp
// ---------------------------------------------------------------- Astra
#include <Astra/Astra.hpp>

namespace {
    struct SmokePosition { float x, y, z; };
}

TEST_CASE("Astra: registry create/get round-trip", "[vendor][astra]")
{
    Astra::Registry registry;
    Astra::Entity e = registry.CreateEntityWith(SmokePosition{1.0f, 2.0f, 3.0f});

    SmokePosition* p = registry.GetComponent<SmokePosition>(e);
    REQUIRE(p != nullptr);
    REQUIRE(p->x == 1.0f);
    REQUIRE(p->z == 3.0f);
}
```
(API confirmed against `ThirdParty/Astra/tests/Registry/ViewTest.cpp` and `RegistryTest.cpp`: `CreateEntityWith(T{...})`, `GetComponent<T>(entity)` returning `T*`. If signatures shifted in v3.1, crib from those test files.)

- [ ] **Step 5: Build, run, add README row, commit**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[astra]"
```

README row: `| **Astra** | v3.1.0 | MIT | Arcane | Archetype ECS (in-house; tests run in its own repo, gate vendor pulls) | (own repo) |`

```bash
git add ThirdParty/Astra ThirdParty/README.md Arcane/
git commit -m "build(thirdparty): vendor Astra v3.1 (hardened) + registry smoke test"
```

---### Task 7: Vendor enkiTS + wrapper + parallel smoke

**Files:**
- Create: `ThirdParty/enkiTS/` (vendor: `src/`, `LICENSE`)
- Create: `ThirdParty/enkiTS/premake5.lua`
- Modify: `Arcane/premake5.lua`, `Arcane/Tests/src/VendorSmokeTest.cpp`, `ThirdParty/README.md`

- [ ] **Step 1: Vendor**

```powershell
git clone --depth 1 --branch v1.11 https://github.com/dougbinks/enkiTS "$env:TEMP\enkiTS"
New-Item -ItemType Directory -Force ThirdParty\enkiTS
Copy-Item -Recurse "$env:TEMP\enkiTS\src" ThirdParty\enkiTS\src
Copy-Item "$env:TEMP\enkiTS\License.txt" ThirdParty\enkiTS\LICENSE
```

- [ ] **Step 2: Write `ThirdParty/enkiTS/premake5.lua`**

```lua
-- enkiTS premake5 build script
-- zlib license -- permissively licensed C++11 task scheduler
-- Consumed by the Arcane workspace (and Astra's IWorkScheduler adapter, M1+).

project "enkiTS"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "src/TaskScheduler.cpp",
        "src/TaskScheduler.h",
        "src/LockLessMultiReadPipe.h",
    }

    includedirs { "src" }

    defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
```

- [ ] **Step 3: Wire into `Arcane/premake5.lua`**

Add `IncludeDir["enkiTS"] = "%{wks.location}/../ThirdParty/enkiTS/src"`, add `include "../ThirdParty/enkiTS"` to the Dependencies group, add the include dir and `"enkiTS"` to ArcaneTests `includedirs`/`links`.

- [ ] **Step 4: Append the smoke test**

```cpp
// ---------------------------------------------------------------- enkiTS
#include <TaskScheduler.h>
#include <atomic>

TEST_CASE("enkiTS: parallel task set sums a range", "[vendor][enkits]")
{
    enki::TaskScheduler scheduler;
    scheduler.Initialize();

    std::atomic<uint32_t> count{0};
    enki::TaskSet task(10000, [&](enki::TaskSetPartition range, uint32_t) {
        count.fetch_add(range.end - range.start, std::memory_order_relaxed);
    });
    scheduler.AddTaskSetToPipe(&task);
    scheduler.WaitforTask(&task);

    REQUIRE(count.load() == 10000);
}
```

- [ ] **Step 5: Build, run `"[enkits]"`, add README row (`v1.11 | zlib | Arcane | Job system`), commit**

```bash
git add ThirdParty/enkiTS ThirdParty/README.md Arcane/
git commit -m "build(thirdparty): vendor enkiTS v1.11 with premake wrapper + smoke"
```

---

### Task 8: Vendor Tracy client + wrapper (build gate only)

**Files:**
- Create: `ThirdParty/tracy/` (vendor: `public/`, `LICENSE`)
- Create: `ThirdParty/tracy/premake5.lua`
- Modify: `Arcane/premake5.lua`, `ThirdParty/README.md`

- [ ] **Step 1: Vendor (latest tagged release; record it)**

```powershell
git clone --depth 1 --branch v0.11.1 https://github.com/wolfpld/tracy "$env:TEMP\tracy"
# If a newer tag exists, prefer it: git ls-remote --tags https://github.com/wolfpld/tracy | tail
New-Item -ItemType Directory -Force ThirdParty\tracy
Copy-Item -Recurse "$env:TEMP\tracy\public" ThirdParty\tracy\public
Copy-Item "$env:TEMP\tracy\LICENSE" ThirdParty\tracy\LICENSE
```
(The Tracy *viewer* binary goes to `ThirdParty/tools/` in Task 11's tooling pass only if desired later — the stack spec lists it as a prebuilt; defer until first profiling session needs it. Record the decision in the README row.)

- [ ] **Step 2: Write `ThirdParty/tracy/premake5.lua`**

```lua
-- Tracy client premake5 build script
-- BSD-3-Clause -- frame profiler client. Compiled in only where
-- TRACY_ENABLE is defined: Debug + Release here; Dist builds the TU
-- with the define absent, which compiles to nothing (zero cost).

project "TracyClient"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files { "public/TracyClient.cpp" }

    includedirs { "public" }

    defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
        defines { "TRACY_ENABLE" }

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG", "TRACY_ENABLE" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
```

- [ ] **Step 3: Wire and build**

Add `IncludeDir["tracy"] = "%{wks.location}/../ThirdParty/tracy/public"` and `include "../ThirdParty/tracy"` to `Arcane/premake5.lua` (do NOT link it into ArcaneTests — consumers arrive in M1; note Tracy consumers on Windows also need `ws2_32` + `dbghelp`, which ArcaneTests/Loom add when they first link it). Regenerate; build Debug and Dist:

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m /t:TracyClient
msbuild Arcane.slnx /p:Configuration=Dist /m /t:TracyClient
```
Expected: both build (Dist compiles the TU to an empty lib — that is the design).

- [ ] **Step 4: README row + commit**

Row: `| **tracy** | <recorded tag> | BSD-3-Clause | Arcane | Frame profiler client (TRACY_ENABLE in Debug/Release only) | https://github.com/wolfpld/tracy |`

```bash
git add ThirdParty/tracy ThirdParty/README.md Arcane/
git commit -m "build(thirdparty): vendor Tracy client with TRACY_ENABLE config gating"
```

---

### Task 9: Vendor FreeType + wrapper + init smoke

**Files:**
- Create: `ThirdParty/freetype/` (vendor: `include/`, `src/`, `LICENSE.TXT` + `docs/FTL.TXT` as `LICENSE`)
- Create: `ThirdParty/freetype/premake5.lua`
- Modify: `Arcane/premake5.lua`, `Arcane/Tests/src/VendorSmokeTest.cpp`, `ThirdParty/README.md`

- [ ] **Step 1: Vendor**

```powershell
git clone --depth 1 --branch VER-2-13-3 https://github.com/freetype/freetype "$env:TEMP\freetype"
New-Item -ItemType Directory -Force ThirdParty\freetype
Copy-Item -Recurse "$env:TEMP\freetype\include" ThirdParty\freetype\include
Copy-Item -Recurse "$env:TEMP\freetype\src" ThirdParty\freetype\src
Copy-Item "$env:TEMP\freetype\docs\FTL.TXT" ThirdParty\freetype\LICENSE
```

- [ ] **Step 2: Write `ThirdParty/freetype/premake5.lua`**

The file list is the canonical "one TU per module" amalgamation build — it must cover every module declared in `include/freetype/config/ftmodule.h`, or `ftinit.c` fails to link.

```lua
-- FreeType premake5 build script
-- FTL license -- font rasterizer. Amalgamated-module build: one TU per
-- module, matching the default ftmodule.h module list. FT2_BUILD_LIBRARY
-- selects the internal-build include paths.

project "freetype"
    kind "StaticLib"
    language "C"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "src/autofit/autofit.c",
        "src/base/ftbase.c",
        "src/base/ftbbox.c",
        "src/base/ftbdf.c",
        "src/base/ftbitmap.c",
        "src/base/ftcid.c",
        "src/base/ftdebug.c",
        "src/base/ftfstype.c",
        "src/base/ftgasp.c",
        "src/base/ftglyph.c",
        "src/base/ftgxval.c",
        "src/base/ftinit.c",
        "src/base/ftmm.c",
        "src/base/ftotval.c",
        "src/base/ftpatent.c",
        "src/base/ftpfr.c",
        "src/base/ftstroke.c",
        "src/base/ftsynth.c",
        "src/base/ftsystem.c",
        "src/base/fttype1.c",
        "src/base/ftwinfnt.c",
        "src/bdf/bdf.c",
        "src/cache/ftcache.c",
        "src/cff/cff.c",
        "src/cid/type1cid.c",
        "src/gzip/ftgzip.c",
        "src/lzw/ftlzw.c",
        "src/pcf/pcf.c",
        "src/pfr/pfr.c",
        "src/psaux/psaux.c",
        "src/pshinter/pshinter.c",
        "src/psnames/psnames.c",
        "src/raster/raster.c",
        "src/sdf/sdf.c",
        "src/sfnt/sfnt.c",
        "src/smooth/smooth.c",
        "src/svg/svg.c",
        "src/truetype/truetype.c",
        "src/type1/type1.c",
        "src/type42/type42.c",
        "src/winfonts/winfnt.c",
    }

    includedirs { "include" }

    defines { "FT2_BUILD_LIBRARY", "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
```

If the 2.13.3 tree's module list differs (link errors mentioning `FT_USE_MODULE` symbols), reconcile against `include/freetype/config/ftmodule.h` — add the missing module TU rather than editing ftmodule.h.

- [ ] **Step 3: Wire into `Arcane/premake5.lua`**

`IncludeDir["freetype"] = "%{wks.location}/../ThirdParty/freetype/include"`, `include "../ThirdParty/freetype"` in Dependencies, add include dir + `"freetype"` link to ArcaneTests.

- [ ] **Step 4: Append the smoke test**

```cpp
// ---------------------------------------------------------------- FreeType
#include <ft2build.h>
#include FT_FREETYPE_H

TEST_CASE("FreeType: library init/done and version", "[vendor][freetype]")
{
    FT_Library lib = nullptr;
    REQUIRE(FT_Init_FreeType(&lib) == 0);

    FT_Int maj = 0, min = 0, patch = 0;
    FT_Library_Version(lib, &maj, &min, &patch);
    REQUIRE(maj == 2);
    REQUIRE(min >= 13);

    REQUIRE(FT_Done_FreeType(lib) == 0);
}
```

- [ ] **Step 5: Build, run `"[freetype]"`, README row (`VER-2-13-3 | FTL | Arcane | Font rasterizer`), commit**

```bash
git add ThirdParty/freetype ThirdParty/README.md Arcane/
git commit -m "build(thirdparty): vendor FreeType 2.13.3 with premake wrapper + smoke"
```

---

### Task 10: Vendor NVRHI + Vulkan-Headers + DirectX-Headers + wrapper + link smoke

**Files:**
- Create: `ThirdParty/Vulkan-Headers/include/` + `LICENSE`
- Create: `ThirdParty/DirectX-Headers/include/` + `LICENSE`
- Create: `ThirdParty/nvrhi/` (vendor: `include/`, `src/`, `LICENSE.txt` as `LICENSE`, `CMakeLists.txt` kept for reference)
- Create: `ThirdParty/nvrhi/premake5.lua`
- Modify: `Arcane/premake5.lua`, `Arcane/Tests/src/VendorSmokeTest.cpp`, `ThirdParty/README.md`

- [ ] **Step 1: Vendor all three (record tags/SHAs)**

```powershell
git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers "$env:TEMP\vkh"
git -C "$env:TEMP\vkh" describe --tags          # record
New-Item -ItemType Directory -Force ThirdParty\Vulkan-Headers
Copy-Item -Recurse "$env:TEMP\vkh\include" ThirdParty\Vulkan-Headers\include
Copy-Item "$env:TEMP\vkh\LICENSE.md" ThirdParty\Vulkan-Headers\LICENSE

git clone --depth 1 https://github.com/microsoft/DirectX-Headers "$env:TEMP\dxh"
git -C "$env:TEMP\dxh" describe --tags          # record
New-Item -ItemType Directory -Force ThirdParty\DirectX-Headers
Copy-Item -Recurse "$env:TEMP\dxh\include" ThirdParty\DirectX-Headers\include
Copy-Item "$env:TEMP\dxh\LICENSE" ThirdParty\DirectX-Headers\LICENSE

git clone --depth 1 https://github.com/NVIDIA-RTX/NVRHI "$env:TEMP\nvrhi"
git -C "$env:TEMP\nvrhi" rev-parse --short HEAD # record
New-Item -ItemType Directory -Force ThirdParty\nvrhi
Copy-Item -Recurse "$env:TEMP\nvrhi\include" ThirdParty\nvrhi\include
Copy-Item -Recurse "$env:TEMP\nvrhi\src" ThirdParty\nvrhi\src
Copy-Item "$env:TEMP\nvrhi\LICENSE.txt" ThirdParty\nvrhi\LICENSE
Copy-Item "$env:TEMP\nvrhi\CMakeLists.txt" ThirdParty\nvrhi\CMakeLists.txt.reference
```
DirectX-Headers' generated DirectXMath dependency is NOT needed — only `include/`. If NVRHI's clone has thirdparty submodule dirs, do not copy them (we vendor the two header packages ourselves).

- [ ] **Step 2: Write `ThirdParty/nvrhi/premake5.lua`**

```lua
-- NVRHI premake5 build script
-- MIT -- GPU abstraction (D3D12 + Vulkan backends; D3D11 off per the
-- 2026-06-10 stack spec). Mirrors upstream CMakeLists options; the copy
-- kept at CMakeLists.txt.reference is the reconciliation source on pulls
-- (risk R1: wrapper drift).

project "nvrhi"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "include/nvrhi/**.h",
        "src/common/**.cpp",
        "src/common/**.h",
        "src/validation/**.cpp",
        "src/validation/**.h",
        "src/d3d12/**.cpp",
        "src/d3d12/**.h",
        "src/vulkan/**.cpp",
        "src/vulkan/**.h",
    }

    includedirs {
        "include",
        "src",
        "../Vulkan-Headers/include",
        "../DirectX-Headers/include",
        "../DirectX-Headers/include/directx",
    }

    defines {
        "NVRHI_WITH_DX11=0",
        "NVRHI_WITH_DX12=1",
        "NVRHI_WITH_VULKAN=1",
        "NVRHI_WITH_AFTERMATH=0",
        "VK_USE_PLATFORM_WIN32_KHR",
        "VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
        "_CRT_SECURE_NO_WARNINGS",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/bigobj" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
```

Reconcile against the actual clone before first build: check `src/` subdir names (`common`, `validation`, `d3d12`, `vulkan` expected; skip any `d3d11`/`rtxmu` content), and check `CMakeLists.txt.reference` for required defines this wrapper misses (e.g. a Vulkan-Hpp storage/assert define). Adjust the wrapper, not the sources.

- [ ] **Step 3: Wire into `Arcane/premake5.lua`**

```lua
    IncludeDir["nvrhi"]           = "%{wks.location}/../ThirdParty/nvrhi/include"
    IncludeDir["VulkanHeaders"]   = "%{wks.location}/../ThirdParty/Vulkan-Headers/include"
    IncludeDir["DirectXHeaders"]  = "%{wks.location}/../ThirdParty/DirectX-Headers/include"
```
`include "../ThirdParty/nvrhi"` in Dependencies. ArcaneTests: add the three include dirs, link `"nvrhi"`, and in the windows filter add `links { "d3d12", "dxgi", "dxguid" }`.

- [ ] **Step 4: Append the link smoke (no GPU required)**

```cpp
// ---------------------------------------------------------------- NVRHI
// Link smoke only: taking the factory addresses forces both backend
// objects to link (pulling in d3d12/dxgi and the Vulkan-Hpp dynamic
// dispatch path). Real device creation is M1's Playground gate.
#include <nvrhi/nvrhi.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/vulkan.h>

TEST_CASE("NVRHI: both backend factories link", "[vendor][nvrhi]")
{
    auto* d3d12Factory  = &nvrhi::d3d12::createDevice;
    auto* vulkanFactory = &nvrhi::vulkan::createDevice;
    REQUIRE(d3d12Factory != nullptr);
    REQUIRE(vulkanFactory != nullptr);

    // Header-level sanity: descriptor defaults exist and compile.
    nvrhi::TextureDesc desc;
    desc.width = 1920;
    REQUIRE(desc.width == 1920);
}
```
(If the factory function names differ in the vendored revision — check `include/nvrhi/d3d12.h` / `include/nvrhi/vulkan.h` for the `createDevice` declarations and use the exact names found.)

- [ ] **Step 5: Build, run `"[nvrhi]"`, README rows, commit**

Rows: `nvrhi | <SHA> | MIT | Arcane | GPU abstraction (DX12+Vulkan, DX11 off)`, `Vulkan-Headers | <tag> | Apache-2.0/MIT | Arcane | Vulkan API headers (include-only)`, `DirectX-Headers | <tag> | MIT | Arcane | D3D12 Agility headers (include-only)`.

```bash
git add ThirdParty/nvrhi ThirdParty/Vulkan-Headers ThirdParty/DirectX-Headers ThirdParty/README.md Arcane/
git commit -m "build(thirdparty): vendor NVRHI + Vulkan-Headers + DirectX-Headers, DX12+VK wrapper"
```

---

### Task 11: DXC + ShaderMake tool binaries

**Files:**
- Create: `ThirdParty/tools/dxc/` (binaries + `LICENSE`)
- Create: `ThirdParty/tools/ShaderMake/` (built binary + `LICENSE`)
- Modify: `ThirdParty/README.md`

- [ ] **Step 1: Vendor DXC (latest GitHub release; record version)**

```powershell
New-Item -ItemType Directory -Force ThirdParty\tools\dxc
gh release download -R microsoft/DirectXShaderCompiler --pattern "dxc_*.zip" -D "$env:TEMP\dxc"
Expand-Archive "$env:TEMP\dxc\dxc_*.zip" -DestinationPath "$env:TEMP\dxc\unpacked"
Copy-Item "$env:TEMP\dxc\unpacked\bin\x64\dxc.exe","$env:TEMP\dxc\unpacked\bin\x64\dxcompiler.dll","$env:TEMP\dxc\unpacked\bin\x64\dxil.dll" ThirdParty\tools\dxc\
# License: LICENSE-MIT or LICENSE.TXT in the zip root
Copy-Item "$env:TEMP\dxc\unpacked\LICENSE*" ThirdParty\tools\dxc\
ThirdParty\tools\dxc\dxc.exe --version          # record for README
```

- [ ] **Step 2: Build and vendor ShaderMake (record SHA)**

```powershell
git clone --recursive https://github.com/NVIDIA-RTX/ShaderMake "$env:TEMP\ShaderMake"
git -C "$env:TEMP\ShaderMake" rev-parse --short HEAD   # record
cmake -S "$env:TEMP\ShaderMake" -B "$env:TEMP\ShaderMake\build"
cmake --build "$env:TEMP\ShaderMake\build" --config Release
New-Item -ItemType Directory -Force ThirdParty\tools\ShaderMake
Copy-Item "$env:TEMP\ShaderMake\build\Release\ShaderMake.exe" ThirdParty\tools\ShaderMake\   # adjust path to actual cmake output
Copy-Item "$env:TEMP\ShaderMake\LICENSE*" ThirdParty\tools\ShaderMake\
ThirdParty\tools\ShaderMake\ShaderMake.exe --help      # verify it runs
```
(If `cmake` is not on PATH, use the VS-bundled one: `& "C:\Program Files\Microsoft Visual Studio\...\CMake\bin\cmake.exe"` — locate with `Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter cmake.exe -Depth 6`.)

- [ ] **Step 3: README rows + commit**

Add a `## Tools` note or rows: `dxc | <recorded> | LLVM/NCSA | Arcane (M2 shader pipeline) | HLSL -> DXIL/SPIR-V compiler binaries`, `ShaderMake | <SHA> | MIT | Arcane (M2) | Shader build orchestrator (built from source, binary vendored)`. The premake pre-build step that invokes them lands in M2 with the first HLSL file — M0's gate is "binaries present, run, licensed, pinned".

```bash
git add ThirdParty/tools ThirdParty/README.md
git commit -m "build(thirdparty): vendor DXC + ShaderMake tool binaries (shader pipeline lands M2)"
```

---

### Task 12: SDL3 via vcpkg with a /MD overlay triplet

The existing `x64-windows-static` overlay pins static CRT + v143 — wrong CRT for Arcane. Add the `-md` variant (static lib, dynamic CRT, same v143 ABI pin) and wire SDL3 into ArcaneTests to retire stack-spec risk R2 now.

**Files:**
- Create: `vcpkg-triplets/x64-windows-static-md.cmake`
- Create: `Arcane/scripts/setup-vcpkg-deps.bat`
- Modify: `Arcane/scripts/generate.bat` (re-add the vcpkg check, mirroring Server's)
- Modify: `Arcane/premake5.lua`, `Arcane/Tests/src/VendorSmokeTest.cpp`, `ThirdParty/README.md`

- [ ] **Step 1: Write `vcpkg-triplets/x64-windows-static-md.cmake`**

```cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET v143)
```

- [ ] **Step 2: Write `Arcane/scripts/setup-vcpkg-deps.bat`**

Mirror `Server/scripts/setup-vcpkg-deps.bat`'s structure (vcpkg resolution, error messages) but install:

```bat
"%VCPKG_PATH%\vcpkg.exe" install sdl3:x64-windows-static-md --overlay-triplets="%~dp0..\..\vcpkg-triplets"
```

- [ ] **Step 3: Run it and record the installed SDL3 version**

```bat
Arcane\scripts\setup-vcpkg-deps.bat
```
Expected: `sdl3:x64-windows-static-md` installed under `%VCPKG_ROOT%\installed\x64-windows-static-md\`. Record the version vcpkg prints.

- [ ] **Step 4: Wire into `Arcane/premake5.lua`**

At the top (before the workspace), mirror Server's vcpkg guard:

```lua
-- vcpkg required for SDL3 (platform layer; deep build system -> vcpkg per
-- the repo rule). Overlay triplet x64-windows-static-md pins v143 + /MD.
VCPKG_ROOT = os.getenv("VCPKG_ROOT")
if not VCPKG_ROOT then
    error("VCPKG_ROOT environment variable is not set.\nSet it to your vcpkg installation directory, e.g.:\n  setx VCPKG_ROOT C:\\vcpkg\nThen restart your terminal and re-run GenerateProjects.bat.")
end
VCPKG_INSTALLED_MD = VCPKG_ROOT .. "/installed/x64-windows-static-md"
```

IncludeDir: `IncludeDir["SDL3"] = VCPKG_INSTALLED_MD .. "/include"`. ArcaneTests additions:

```lua
    -- inside includedirs:    "%{IncludeDir.SDL3}",
    filter { "system:windows", "configurations:Debug" }
        libdirs { VCPKG_INSTALLED_MD .. "/debug/lib" }
    filter { "system:windows", "configurations:Release or configurations:Dist" }
        libdirs { VCPKG_INSTALLED_MD .. "/lib" }
    filter "system:windows"
        links {
            "SDL3-static",
            "winmm", "version", "imm32", "setupapi",
            "ole32", "oleaut32", "gdi32", "user32", "advapi32", "shell32",
        }
```
(If link errors name other system libs, check `%VCPKG_ROOT%\installed\x64-windows-static-md\lib\pkgconfig\sdl3.pc` for the authoritative `Libs.private` list and match it.)

Also re-add the vcpkg presence check to `Arcane/scripts/generate.bat` (copy the `[1/2] Checking vcpkg...` block from `Server/scripts/generate.bat` verbatim).

- [ ] **Step 5: Append the smoke test**

```cpp
// ---------------------------------------------------------------- SDL3
// Version query only -- no SDL_Init, no window. Proves the vcpkg
// static-md triplet links against /MD (retires stack-spec risk R2).
#include <SDL3/SDL_version.h>

TEST_CASE("SDL3: links and reports its version", "[vendor][sdl3]")
{
    const int v = SDL_GetVersion();
    REQUIRE(SDL_VERSIONNUM_MAJOR(v) == 3);
}
```

- [ ] **Step 6: Build, run `"[sdl3]"`, README note, commit**

In `ThirdParty/README.md`'s vcpkg paragraph, update: vcpkg now also manages `sdl3` (Arcane, `x64-windows-static-md` overlay triplet).

```bash
git add vcpkg-triplets/x64-windows-static-md.cmake Arcane/ ThirdParty/README.md
git commit -m "build(arcane): SDL3 via vcpkg x64-windows-static-md overlay triplet + link smoke"
```

---

### Task 13: Docs + final verification sweep

**Files:**
- Modify: `CLAUDE.md` (root)
- Modify: `ThirdParty/README.md` (final consistency pass)

- [ ] **Step 1: Update root `CLAUDE.md`**

(a) Repository Layout block — add after the `Tools/` line:

```
├── Arcane/         # C++23 engine workspace (Arcane.slnx): Core + ArcaneTests (M0)
```

(b) New section after "## Build System (Server)":

```markdown
## Build System (Arcane engine)

The engine workspace lives at `Arcane/` (spec: `docs/superpowers/specs/2026-06-11-engine-architecture-design.md`). M0 contains `Core` (static lib) + `ArcaneTests`; Arcane.dll/Loom/Grimoire/Playground arrive in later milestones.

```bat
cd Arcane
scripts\setup-vcpkg-deps.bat   # once: SDL3 via the x64-windows-static-md overlay triplet
GenerateProjects.bat            # generates Arcane.slnx
msbuild Arcane.slnx /p:Configuration=Debug /m
bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
```

Rules baked into the workspace:
- **/MD everywhere** (dynamic CRT) — memory will cross the Arcane.dll/Game.dll boundary. Server/Tools stay /MT; the shared ThirdParty wrappers are parameterized via `THIRDPARTY_STATICRUNTIME` / `THIRDPARTY_PROJECT_LOCATION` globals.
- **Core extraction (strangler):** TcpSocket, Protocol, RateLimiter, Crypto, Types, Logger, LruCache live under the namespaced include root `Arcane/Core/src/Arcane/` (`#include <Arcane/Net/Protocol.hpp>`) in `namespace Arcane`. **Core contains zero Aphelyon references** — keep it that way; it must stay liftable into other projects. The Server workspace compiles the same sources as project `ArcaneCore` (static CRT); the old `Server/Common/src/...` paths are thin server-owned shims re-exporting names into `namespace Aphelyon`. New server code should include `<Arcane/...>` directly; delete a shim when its last includer migrates. Never re-add these names to `Server/Common`.
- No `/fp:fast` in engine builds. UTF-8 without BOM, ASCII comments.
```

(c) In the existing Architecture section's Common description, note that Protocol/Types/Crypto/RateLimiter/Logger/LruCache moved to `Arcane/Core/src/Arcane/` and the old Common paths are re-export shims.

- [ ] **Step 2: ThirdParty/README.md consistency pass**

Verify every dep added in Tasks 5-12 has a row with a real recorded version (no `<recorded>` placeholders left), alphabetical order holds, and each subdir has a `LICENSE` file:

```powershell
Get-ChildItem ThirdParty -Directory | Where-Object { -not (Test-Path "$($_.FullName)\LICENSE*") } | Select-Object Name
```
Expected: only dirs that predate this convention with embedded licenses (and `tools/`, `premake5/`, `love2d/` style binary dirs that carry licenses inside) — investigate anything new.

- [ ] **Step 3: Full verification matrix**

```bat
cd Arcane
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
msbuild Arcane.slnx /p:Configuration=Release /m
msbuild Arcane.slnx /p:Configuration=Dist /m
Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe
Arcane\bin\Release-windows-x86_64-md\ArcaneTests\ArcaneTests.exe

cd ..\Server
GenerateProjects.bat
msbuild Aphelyon.slnx /p:Configuration=Debug /m
msbuild Aphelyon.slnx /p:Configuration=Release /m
Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe
Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
Server\bin\Debug-windows-x86_64\AuthTests\AuthTests.exe
Server\bin\Debug-windows-x86_64\CombatTests\CombatTests.exe
```
Expected: every build green; ArcaneTests all pass in Debug AND Release; server suites match the Task 1 baseline counts.

Also confirm the Tools editor still generates/builds (it shares ThirdParty): `Tools\GenerateProjects.bat` then build `Tools/AphelyonTools.slnx` Debug.

- [ ] **Step 4: Working-tree hygiene check**

```bash
git status --short
```
Expected: empty (or only known-untracked local files). Generated `.slnx`/`.vcxproj`/`ide-md/` paths must be ignored, not pending.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md ThirdParty/README.md
git commit -m "docs: Arcane M0 - workspace build docs, Core extraction rules, ThirdParty inventory"
```

---

## M0 exit criteria (verify-work checklist)

1. `Arcane/Arcane.slnx` generates and builds Debug/Release/Dist, all /MD.
2. `ArcaneTests.exe` passes: wire oracle (threading_harness port + audit edges + round-trip property), Crypto smoke, and one smoke per vendored dep (glm, stb, miniaudio, Astra, enkiTS, FreeType, NVRHI link, SDL3 link).
3. `Aphelyon.slnx` still builds Debug + Release; all four server test suites match the pre-extraction baseline; services boot via `start-all.bat`.
4. The seven old `Server/Common/src/...` paths are thin re-export shims; the real headers live under `Arcane/Core/src/Arcane/` with **zero Aphelyon residue** (`grep -rni aphelyon Arcane/Core/` is empty). No server file changed except `Server/premake5.lua` and the seven shims.
5. Every ThirdParty addition has a LICENSE file and a README row with a pinned version/SHA; DXC + ShaderMake binaries run.
6. `ThirdParty/Astra/` is tracked (no bin/ide artifacts).
7. Tools workspace unaffected (generates + builds).
