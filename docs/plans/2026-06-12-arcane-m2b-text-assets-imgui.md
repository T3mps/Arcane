# Arcane M2b — MSDF Text, Asset Loaders v1, ImGui Backends — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The renderer core's first content systems: MSDF glyph text (msdfgen + the client's skyline packer ported as the runtime atlas), synchronous asset loaders with `services.Assets` cache semantics, and the ImGui pair (first-party `imgui_impl_nvrhi` + upstream `imgui_impl_sdl3`) inside Arcane.dll.

**Architecture:** Three semi-independent subsystems consuming M2a: **Assets** (AssetCache template porting `cache.lua`'s refcount/bytes/LRU/memoized-failure semantics + an `Assets` facade loading sRGB textures via stb, raw bytes, and JSON), **Text** (`SkylinePacker` in Core; `TextSystem` in the DLL generating per-glyph MSDFs via msdfgen into one 1024² RGBA8 atlas, drawing through the batcher's new `Glyph`/`BatchKind::Text` path with an `msdf` shader), and **ImGui** (imgui compiled into the DLL via a parameterized ThirdParty wrapper; the 1.92 `ImTextureData` renderer protocol implemented on NVRHI; SDL events reach `imgui_impl_sdl3` through a native event tap on `Window`). Everything follows the established patterns: exported pure-virtual + factory, anonymous-namespace impls, generation-keyed pipeline caches, `RenderErrorCount() == 0` gates.

**Tech Stack:** msdfgen (NEW vendored dep), FreeType (vendored; face loading for msdfgen), stb_image, nlohmann/json, Dear ImGui 1.92.9-docking (core already vendored; `imgui_impl_sdl3` to vendor), NVRHI, Catch2.

**Specs:** `docs/superpowers/specs/2026-06-12-arcane-2d-renderer-architecture.md` (M2b row; "MSDF/SDF atlas, NOT bitmap fonts"; bindless/atlas notes) + `2026-06-11-engine-architecture-design.md` (Text/Assets module responsibilities; Grimoire UI = first-party imgui_impl_nvrhi + upstream imgui_impl_sdl3) + `2026-06-10-engine-thirdparty-stack-design.md` (vendoring conventions; HarfBuzz deferred).

**User decisions (2026-06-12):** MSDF via msdfgen (not FreeType native SDF); ImGui lives inside Arcane.dll; asset v1 = sync texture/font/json loaders.

---

## Decisions made by this plan (flagged for review)

1. **msdfgen vendored minimally**: `core/` + `ext/import-font.cpp` + `ext/resolve-shape-geometry` only (no save-png/import-svg/skia — their deps stay out). Wrapper defines `MSDFGEN_PUBLIC=` (no DLL export) and points at our vendored FreeType. Pin the latest release tag at vendor time and record version + license rows in `ThirdParty/README.md` (M0 convention).
2. **One global glyph atlas, compile-time constants**: 1024×1024 RGBA8, glyph em box 48 px, MSDF pxRange 6. `kAtlasSize`/`kPxRange` are mirrored constants in `GlyphAtlas` and `msdf.hlsl` (baked into the shader — no push-constant plumbing); a comment in both names the other. Multi-atlas + plumbed ranges arrive with the real atlas-management milestone.
3. **Atlas updates = whole-texture re-upload on dirty** (CPU-side RGBA buffer, `writeTexture` once per `Flush` when new glyphs were added). nvrhi `writeTexture` is whole-subresource; region staging is a later optimization. Recorded order contract: `batcher.Begin → TextSystem::Draw... → TextSystem::Flush(commandList) → batcher.End` (uploads record before the draws).
4. **`SkylinePacker` lives in Core** (`Arcane/Core/src/Arcane/Util/SkylinePacker.hpp`, header-only, presentation-free, no export macro — so STL members are fine and other projects can lift it). NOTE: the Server workspace's ArcaneCore project globs Core sources — Task 9 MUST run the Server build + CommonTests regression.
5. **Color textures upload as `SRGBA8_UNORM`** — stb gives sRGB-encoded bytes; the sRGB format makes sampling yield linear, keeping the all-linear-canvas contract. Data textures (future) will take a flag; v1 is color-only.
6. **AssetCache is a header-only internal template** (no export; tests include it directly). The exported surface is the `Assets` facade only.
7. **Batcher gains `Glyph(...)` + `BatchKind::Text`** — text stays on the single submission path; the MSDF pixel shader is just another pipeline kind in the existing sort key.
8. **ImGui renders POST-tonemap into the backbuffer framebuffer** (display-referred editor UI, untouched by ACES). It implements the 1.92 `ImGuiBackendFlags_RendererHasTextures` protocol — the vendored `ThirdParty/imgui/backends/imgui_impl_dx11.cpp` is the in-repo reference for `ImTextureData` Status handling.
9. **`imgui_impl_sdl3` is vendored from the upstream docking branch matching `IMGUI_VERSION_NUM`** in the vendored `imgui.h` (1.92.9 WIP; record the fetched SHA in README). The ThirdParty/imgui premake wrapper is NEW and safe: Tools lists imgui sources explicitly and never `include`s the directory.
10. **SDL events reach ImGui via a native event tap on `Window`**: `SetNativeEventTap(void(*)(const void* sdlEvent, void* user), void* user)` — opaque pointer documented as `const SDL_Event*`, for engine-internal modules; plain function pointer (no std::function ABI surface). PumpEvents invokes the tap per event before its own handling.
11. **Fonts for tests/demo**: `Client/data/font/Roboto-Regular.ttf` postbuild-copied next to ArcaneTests/Playground as `data/fonts/Roboto-Regular.ttf` (single-source rule: Client stays canonical).
12. **HarfBuzz stays deferred** (stack spec): UTF-8 decode + FreeType kerning + `\n` line breaks only. Rich text/effects later.

## Foundation contracts (M1/M2a contracts all still binding; new)

- Text and ImGui draw through documented paths only: glyphs via Batcher2D (`Glyph`), ImGui via its own backend pipeline into the caller-provided framebuffer — still no `ICommandList` wrapping, no manual barriers, validation silent (`RenderErrorCount() == 0` everywhere).
- Assets facade owns texture lifetime via nvrhi handles + cache semantics; nothing else loads files for rendering.
- New shaders go through `compile-shaders.bat` (artifact-stem `_vs/_ps` suffix invariant).

## File structure

```
ThirdParty/msdfgen/                                 NEW vendor drop + premake5.lua wrapper + LICENSE
ThirdParty/imgui/premake5.lua                       NEW parameterized wrapper (core + imgui_impl_sdl3)
ThirdParty/imgui/backends/imgui_impl_sdl3.{h,cpp}   NEW vendored upstream backend
ThirdParty/README.md                                MODIFIED — msdfgen + imgui-backend rows
Arcane/premake5.lua                                 MODIFIED — msdfgen/imgui wrappers, links, IncludeDirs, font postbuild copies
Arcane/shaders/msdf.hlsl, imgui.hlsl                NEW; compile-shaders.bat MODIFIED (+4 entries)
Arcane/Core/src/Arcane/Util/SkylinePacker.hpp       NEW — header-only port of skyline.lua
Arcane/Arcane/src/Arcane/Assets/AssetCache.hpp      NEW — internal template (cache.lua semantics)
Arcane/Arcane/src/Arcane/Assets/Assets.{hpp,cpp}    NEW — exported facade (GetTexture/GetBytes/GetJson/Stats)
Arcane/Arcane/src/Arcane/Text/TextSystem.{hpp,cpp}  NEW — fonts, MSDF glyph atlas, Draw/Measure/Flush
Arcane/Arcane/src/Arcane/Render/Batcher2D.{hpp,cpp} MODIFIED — Glyph() + BatchKind::Text + msdf pipeline
Arcane/Arcane/src/Arcane/Platform/Window.{hpp,cpp}  MODIFIED — native event tap
Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.{hpp,cpp} NEW — exported facade (BeginFrame/Render)
Arcane/Arcane/src/Arcane/ImGui/ImGuiNvrhi.{hpp,cpp} NEW — first-party renderer backend (1.92 texture protocol)
Arcane/Playground/src/main.cpp                      MODIFIED — HUD text + ImGui stats overlay
Arcane/Tests/src/SkylinePackerTest.cpp              NEW — CPU characterization (assets_harness oracle)
Arcane/Tests/src/AssetCacheTest.cpp                 NEW — CPU (cache.lua semantics)
Arcane/Tests/src/AssetsTest.cpp                     NEW — [gpu] texture load + readback, memoized failure
Arcane/Tests/src/TextTest.cpp                       NEW — [gpu] glyph coverage + CPU layout tests
Arcane/Tests/src/MsdfgenSmokeTest.cpp               NEW — vendor arrival gate
Arcane/Tests/src/ImGuiTest.cpp                      NEW — [gpu] offscreen ImGui frame renders
CLAUDE.md                                           MODIFIED (Task 9)
```

## Constraints carried into every task

- UTF-8 no BOM, ASCII comments; Write/Edit tools. Never run `db-reset.bat` / `clean.bat --deep` / `docker compose down -v`.
- Build loop: `cd Arcane && GenerateProjects.bat && msbuild Arcane.slnx /p:Configuration=Debug /m && bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe`. Baseline entering: **330 assertions / 36 cases**. NDEBUG stays in Release.
- API-adaptation rule: verified-against-source claims that turn out wrong → check the vendored header, adapt minimally, record the deviation.
- Commit per task, `type(scope):` + the Co-Authored-By trailer used on this branch.

---

### Task 1: Vendor msdfgen + premake wrapper + arrival smoke test

**Files:** Create `ThirdParty/msdfgen/` (drop + `premake5.lua` + LICENSE), `Arcane/Tests/src/MsdfgenSmokeTest.cpp`; Modify `Arcane/premake5.lua`, `ThirdParty/README.md`.

- [ ] **Step 1: Vendor the drop (record the exact tag)**

```powershell
git clone --depth 1 --branch v1.12.1 https://github.com/Chlumsky/msdfgen "$env:TEMP\msdfgen"
# If v1.12.1 is not the latest release tag, use the latest and record it.
New-Item -ItemType Directory -Force ThirdParty\msdfgen
Copy-Item -Recurse "$env:TEMP\msdfgen\core" ThirdParty\msdfgen\core
Copy-Item -Recurse "$env:TEMP\msdfgen\ext"  ThirdParty\msdfgen\ext
Copy-Item "$env:TEMP\msdfgen\msdfgen.h","$env:TEMP\msdfgen\msdfgen-ext.h","$env:TEMP\msdfgen\LICENSE.txt" ThirdParty\msdfgen\
```

Then DELETE from `ThirdParty/msdfgen/ext/`: `import-svg.cpp`, `save-png.cpp`, anything referencing skia/tinyxml2/lodepng/libpng (`grep -l "skia\|lodepng\|png\|tinyxml" ThirdParty/msdfgen/ext/*.cpp` — keep `import-font.cpp` and `resolve-shape-geometry.cpp` if the latter is skia-free; if it requires skia, delete it too and note it). msdfgen may expect a generated `msdfgen-config.h`: if `#include "msdfgen-config.h"` appears, create a minimal one in `ThirdParty/msdfgen/` defining `#define MSDFGEN_PUBLIC` (empty) and version macros copied from the tag — adapt to what the headers actually require and record it.

- [ ] **Step 2: Write `ThirdParty/msdfgen/premake5.lua`** (the Catch2 wrapper pattern):

```lua
-- msdfgen premake5 build script
-- MIT -- multi-channel signed distance field generation (text glyphs).
-- Minimal vendoring: core + ext/import-font (FreeType); no svg/png/skia.
project "msdfgen"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files { "core/**.cpp", "core/**.h", "core/**.hpp",
            "ext/import-font.cpp", "ext/*.h", "ext/*.hpp",
            "msdfgen.h", "msdfgen-ext.h" }

    includedirs { ".", "../freetype/include" }

    defines { "MSDFGEN_PUBLIC=", "_CRT_SECURE_NO_WARNINGS" }

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

(If `resolve-shape-geometry.cpp` survived Step 1, add it to `files`. Adapt file globs to the tag's actual layout — `core/` has .cpp at top level in 1.12.x.)

- [ ] **Step 3: Wire the Arcane workspace** (`Arcane/premake5.lua`): add `IncludeDir["msdfgen"] = "%{wks.location}/../ThirdParty/msdfgen"`, `include "../ThirdParty/msdfgen"` in the Dependencies group, `"msdfgen"` + `"freetype"` to the **Arcane DLL** `links` (freetype moves INTO the DLL now — also REMOVE `"freetype"` from ArcaneTests links if the vendor smoke there still needs it: it does (FreeType smoke test) — keep ArcaneTests' freetype link too; two static copies in different modules is the established pattern). Add `"%{IncludeDir.msdfgen}"` and `"%{IncludeDir.freetype}"` to the DLL includedirs, and `"%{IncludeDir.msdfgen}"` to ArcaneTests includedirs.

- [ ] **Step 4: Write `Arcane/Tests/src/MsdfgenSmokeTest.cpp`** — also wire the font postbuild copy NOW (both ArcaneTests and Playground projects):

```lua
        '{MKDIR} "%{cfg.buildtarget.directory}/data/fonts"',
        '{COPYFILE} "%{wks.location}/../Client/data/font/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/fonts/Roboto-Regular.ttf"',
```

```cpp
// msdfgen arrival gate: load a real glyph from Roboto via FreeType,
// generate a 32x32 MSDF, assert the bitmap has spatial variance (a flat
// bitmap means edge coloring or the FT bridge is broken).

#include <catch2/catch_test_macros.hpp>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <cmath>

TEST_CASE("msdfgen: glyph MSDF has signal", "[vendor][msdfgen]")
{
    msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
    REQUIRE(ft != nullptr);
    msdfgen::FontHandle* font =
        msdfgen::loadFont(ft, "data/fonts/Roboto-Regular.ttf");
    REQUIRE(font != nullptr);

    msdfgen::Shape shape;
    REQUIRE(msdfgen::loadGlyph(shape, font, 'A'));
    shape.normalize();
    msdfgen::edgeColoringSimple(shape, 3.0);

    msdfgen::Bitmap<float, 3> bitmap(32, 32);
    msdfgen::SDFTransformation t(
        msdfgen::Projection(32.0 / 64.0, msdfgen::Vector2(8.0, 8.0)),
        msdfgen::Range(6.0 / (32.0 / 64.0)));
    msdfgen::generateMSDF(bitmap, shape, t);

    float minV = 1e9f, maxV = -1e9f;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            for (int c = 0; c < 3; ++c)
            {
                minV = std::min(minV, bitmap(x, y)[c]);
                maxV = std::max(maxV, bitmap(x, y)[c]);
            }
    REQUIRE(maxV - minV > 0.25f);  // real distance signal, not flat

    msdfgen::destroyFont(font);
    msdfgen::deinitializeFreetype(ft);
}
```

API note: msdfgen's generate/transform signatures changed across versions (`SDFTransformation` is 1.11+; older takes `Projection` + `Range` separately, and `loadGlyph` may take a `FontCoordinateScaling` arg in 1.12 — `msdfgen::FONT_SCALING_EM_NORMALIZED` recommended; check `msdfgen-ext.h`/`msdfgen.h` of the vendored tag and adapt, recording the exact forms used — they become the reference for Task 4).

- [ ] **Step 5: Build, run `"[msdfgen]"`, then full suite; README rows** (msdfgen row with tag + license; note ext subset). **Step 6: Commit** `feat(thirdparty): vendor msdfgen (core + import-font) with premake wrapper`.

---

### Task 2: SkylinePacker port (Core) + characterization tests

**Files:** Create `Arcane/Core/src/Arcane/Util/SkylinePacker.hpp`, `Arcane/Tests/src/SkylinePackerTest.cpp`.

The oracle is `Client/src/services/assets/skyline.lua` (77 lines — READ IT FIRST) and its harness assertions in `Client/src/tests/assets_harness/main.lua` (read the skyline/atlas section and port the concrete assertions you find there as test cases, plus the ones below).

- [ ] **Step 1: Write the failing test `Arcane/Tests/src/SkylinePackerTest.cpp`**

```cpp
// Characterization of the skyline packer against the Lua oracle
// (Client/src/services/assets/skyline.lua): lowest-fitting-y placement,
// skyline raise + collapse, merge of equal-height neighbors.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Util/SkylinePacker.hpp>

using Arcane::SkylinePacker;

TEST_CASE("skyline: first insert lands at origin", "[text][skyline]")
{
    SkylinePacker packer(256, 256);
    auto pos = packer.Insert(64, 32);
    REQUIRE(pos.has_value());
    CHECK(pos->x == 0);
    CHECK(pos->y == 0);
}

TEST_CASE("skyline: same-height inserts pack left to right", "[text][skyline]")
{
    SkylinePacker packer(256, 256);
    auto a = packer.Insert(64, 32);
    auto b = packer.Insert(64, 32);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(b->x == 64);
    CHECK(b->y == 0);
}

TEST_CASE("skyline: picks the lowest fitting valley", "[text][skyline]")
{
    SkylinePacker packer(256, 256);
    (void)packer.Insert(128, 64);   // tall block left
    (void)packer.Insert(128, 16);   // short block right
    auto c = packer.Insert(64, 16); // should stack on the SHORT side
    REQUIRE(c.has_value());
    CHECK(c->y == 16);
    CHECK(c->x == 128);
}

TEST_CASE("skyline: rejects what cannot fit", "[text][skyline]")
{
    SkylinePacker packer(64, 64);
    REQUIRE(packer.Insert(65, 10) == std::nullopt);   // too wide
    REQUIRE(packer.Insert(10, 65) == std::nullopt);   // too tall
    REQUIRE(packer.Insert(64, 64).has_value());       // exact fit
    REQUIRE(packer.Insert(1, 1) == std::nullopt);     // bin full
}

TEST_CASE("skyline: fills a bin with uniform cells completely", "[text][skyline]")
{
    SkylinePacker packer(128, 128);
    int placed = 0;
    while (packer.Insert(16, 16).has_value())
        ++placed;
    CHECK(placed == 64);  // 8x8 grid, no waste for uniform sizes
}
```

(Add any additional concrete assertions you find in the Lua harness as further cases, citing the harness line in a comment.)

- [ ] **Step 2: Confirm compile failure. Step 3: Write `Arcane/Core/src/Arcane/Util/SkylinePacker.hpp`**

```cpp
#pragma once

// Skyline rectangle packing -- C++ port of the client's tested Lua packer
// (Client/src/services/assets/skyline.lua, the porting oracle). Each insert
// picks the skyline position with the lowest fitting height, raises the
// skyline, collapses overlapped spans, and merges equal-height neighbors.
// Not optimal but well-bounded; sort inputs by descending height for best
// packing. Header-only, presentation-free (Core).

#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    class SkylinePacker
    {
    public:
        struct Pos
        {
            uint32_t x = 0;
            uint32_t y = 0;
        };

        SkylinePacker(uint32_t width, uint32_t height)
            : m_width(width), m_height(height)
        {
            m_nodes.push_back({ 0, 0, width });
        }

        // Places a w x h rectangle; returns its top-left or nullopt.
        std::optional<Pos> Insert(uint32_t w, uint32_t h)
        {
            if (w == 0 || h == 0 || w > m_width || h > m_height)
                return std::nullopt;

            int bestIndex = -1;
            uint32_t bestY = 0;
            for (size_t i = 0; i < m_nodes.size(); ++i)
            {
                const auto y = FindBestY(i, w);
                if (!y.has_value())
                    continue;
                if (*y + h > m_height)
                    continue;
                if (bestIndex < 0 || *y < bestY ||
                    (*y == bestY && m_nodes[i].x < m_nodes[(size_t)bestIndex].x))
                {
                    bestIndex = (int)i;
                    bestY = *y;
                }
            }
            if (bestIndex < 0)
                return std::nullopt;

            const uint32_t x = m_nodes[(size_t)bestIndex].x;
            AddSkyline((size_t)bestIndex, x, bestY + h, w);
            return Pos{ x, bestY };
        }

    private:
        struct Node
        {
            uint32_t x = 0;
            uint32_t y = 0;   // skyline height at this span
            uint32_t w = 0;
        };

        // Best (max) y across the spans a w-wide rect would cover starting
        // at node i; nullopt when it overruns the bin (oracle: findBestY).
        std::optional<uint32_t> FindBestY(size_t i, uint32_t w) const
        {
            if (m_nodes[i].x + w > m_width)
                return std::nullopt;
            uint32_t y = m_nodes[i].y;
            int64_t widthLeft = (int64_t)w;
            size_t j = i;
            while (widthLeft > 0)
            {
                if (j >= m_nodes.size())
                    return std::nullopt;
                y = std::max(y, m_nodes[j].y);
                widthLeft -= (int64_t)m_nodes[j].w;
                ++j;
            }
            return y;
        }

        // Insert the raised span, shrink/remove overlapped successors,
        // merge equal-height neighbors (oracle: addSkyline).
        void AddSkyline(size_t index, uint32_t x, uint32_t newY, uint32_t w)
        {
            m_nodes.insert(m_nodes.begin() + (ptrdiff_t)index,
                           Node{ x, newY, w });
            for (size_t i = index + 1; i < m_nodes.size();)
            {
                Node& prev = m_nodes[i - 1];
                Node& cur = m_nodes[i];
                if (cur.x < prev.x + prev.w)
                {
                    const uint32_t shrink = prev.x + prev.w - cur.x;
                    if (cur.w <= shrink)
                    {
                        m_nodes.erase(m_nodes.begin() + (ptrdiff_t)i);
                        continue;  // re-check the new neighbor
                    }
                    cur.x += shrink;
                    cur.w -= shrink;
                }
                break;
            }
            for (size_t i = 0; i + 1 < m_nodes.size();)
            {
                if (m_nodes[i].y == m_nodes[i + 1].y)
                {
                    m_nodes[i].w += m_nodes[i + 1].w;
                    m_nodes.erase(m_nodes.begin() + (ptrdiff_t)(i + 1));
                }
                else
                {
                    ++i;
                }
            }
        }

        uint32_t m_width;
        uint32_t m_height;
        std::vector<Node> m_nodes;
    };
}
```

(Include `<algorithm>` for std::max. Semantics, not syntax, must match the Lua: if a test ported from the harness disagrees with this implementation, the LUA behavior wins — fix the port, not the test.)

- [ ] **Step 4: Build + run `"[skyline]"`, then full suite.** **Step 5: Commit** `feat(arcane): SkylinePacker - Core port of the client skyline packer`.

---

### Task 3: Assets module — AssetCache (cache.lua semantics) + Assets facade

**Files:** Create `Arcane/Arcane/src/Arcane/Assets/AssetCache.hpp`, `Assets/Assets.hpp`, `Assets/Assets.cpp`, `Arcane/Tests/src/AssetCacheTest.cpp`, `Arcane/Tests/src/AssetsTest.cpp`.

Oracle: `Client/src/services/assets/cache.lua` (READ IT) — entries keyed by id with `{obj, refs, bytes, used}`, LRU tick on `get`, memoized failures (`false` ≠ missing), refcount acquire/release.

- [ ] **Step 1: Failing CPU test `Arcane/Tests/src/AssetCacheTest.cpp`**

```cpp
// Characterization of AssetCache against cache.lua: LRU tick on Get,
// memoized failures distinct from misses, refcount + byte bookkeeping.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <Arcane/Assets/AssetCache.hpp>

using Cache = Arcane::AssetCache<std::shared_ptr<std::string>>;

TEST_CASE("asset cache: miss vs memoized failure", "[assets][cache]")
{
    Cache cache;
    CHECK_FALSE(cache.Has("a"));
    CHECK(cache.Get("a") == nullptr);          // miss

    cache.PutFailure("a");
    CHECK(cache.Has("a"));                     // known...
    CHECK(cache.Get("a") == nullptr);          // ...but failed: no object
    CHECK(cache.IsFailure("a"));               // distinguishable from miss
}

TEST_CASE("asset cache: put/get with LRU recency and bytes", "[assets][cache]")
{
    Cache cache;
    cache.Put("x", std::make_shared<std::string>("payload"), 7);
    cache.Put("y", std::make_shared<std::string>("q"), 1);
    CHECK(cache.TotalBytes() == 8);
    CHECK(cache.Count() == 2);

    (void)cache.Get("x");                      // x most recent now
    CHECK(cache.LeastRecentKey() == "y");
    (void)cache.Get("y");
    CHECK(cache.LeastRecentKey() == "x");
}

TEST_CASE("asset cache: refcounts gate eviction", "[assets][cache]")
{
    Cache cache;
    cache.Put("x", std::make_shared<std::string>("v"), 4);
    cache.Acquire("x");
    CHECK_FALSE(cache.Evict("x"));             // pinned
    cache.Release("x");
    CHECK(cache.Evict("x"));
    CHECK_FALSE(cache.Has("x"));
    CHECK(cache.TotalBytes() == 0);
}
```

- [ ] **Step 2: Confirm compile failure. Step 3: Write `Arcane/Arcane/src/Arcane/Assets/AssetCache.hpp`** (internal header-only template — NOT exported; tests include it via the DLL include root):

```cpp
#pragma once

// Asset bookkeeping ported from the client's cache.lua: one entry per key
// with refcount + byte-size accounting and an LRU "used" tick. A cached
// FAILURE is a real entry with no object, so callers distinguish "miss"
// (never tried) from "known-failed" (do not retry). Header-only and
// engine-internal; the exported surface is the Assets facade.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Arcane
{
    template <typename T>
    class AssetCache
    {
    public:
        bool Has(const std::string& key) const
        {
            return m_entries.find(key) != m_entries.end();
        }

        bool IsFailure(const std::string& key) const
        {
            auto it = m_entries.find(key);
            return it != m_entries.end() && it->second.failed;
        }

        // Returns the object (bumping recency) or a default-constructed T
        // for both miss and failure -- pair with Has/IsFailure.
        T Get(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it == m_entries.end() || it->second.failed)
                return T{};
            it->second.used = ++m_tick;
            return it->second.obj;
        }

        void Put(const std::string& key, T obj, uint64_t bytes)
        {
            Entry& e = m_entries[key];
            m_totalBytes += bytes - e.bytes;
            e.obj = std::move(obj);
            e.bytes = bytes;
            e.failed = false;
            e.used = ++m_tick;
        }

        void PutFailure(const std::string& key)
        {
            Entry& e = m_entries[key];
            m_totalBytes -= e.bytes;
            e.obj = T{};
            e.bytes = 0;
            e.failed = true;
            e.used = ++m_tick;
        }

        void Acquire(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it != m_entries.end())
                ++it->second.refs;
        }

        void Release(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it != m_entries.end() && it->second.refs > 0)
                --it->second.refs;
        }

        // Removes the entry unless pinned by refs. Returns true on removal.
        bool Evict(const std::string& key)
        {
            auto it = m_entries.find(key);
            if (it == m_entries.end() || it->second.refs > 0)
                return false;
            m_totalBytes -= it->second.bytes;
            m_entries.erase(it);
            return true;
        }

        // Oldest unpinned entry's key, or empty when none (LRU sweep seam).
        std::string LeastRecentKey() const
        {
            std::string best;
            uint64_t bestUsed = UINT64_MAX;
            for (const auto& [key, e] : m_entries)
            {
                if (e.refs == 0 && e.used < bestUsed)
                {
                    bestUsed = e.used;
                    best = key;
                }
            }
            return best;
        }

        uint64_t TotalBytes() const { return m_totalBytes; }
        size_t Count() const { return m_entries.size(); }

    private:
        struct Entry
        {
            T obj{};
            uint32_t refs = 0;
            uint64_t bytes = 0;
            uint64_t used = 0;
            bool failed = false;
        };

        std::unordered_map<std::string, Entry> m_entries;
        uint64_t m_tick = 0;
        uint64_t m_totalBytes = 0;
    };
}
```

- [ ] **Step 4: Build + `"[cache]"` green. Step 5: Failing GPU test `Arcane/Tests/src/AssetsTest.cpp`**

```cpp
// Assets facade: PNG -> sRGB nvrhi texture with byte-true readback (the
// sRGB format must round-trip raw bytes exactly on copy), bytes/json
// loaders, and memoized failure on a missing path.

#include <catch2/catch_test_macros.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION_GUARD  // see note below
#include <stb_image_write.h>

#include <cstdio>
#include <filesystem>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Render/Device.hpp>

namespace
{
    // Writes a 2x2 PNG with known bytes into the temp dir.
    std::filesystem::path WriteTestPng()
    {
        const unsigned char pixels[2 * 2 * 4] = {
            255, 0, 0, 255,   0, 255, 0, 255,
            0, 0, 255, 255,   128, 64, 32, 255,
        };
        const auto path =
            std::filesystem::temp_directory_path() / "arcane-assets-test.png";
        REQUIRE(stbi_write_png(path.string().c_str(), 2, 2, 4, pixels, 8) != 0);
        return path;
    }
}

TEST_CASE("assets: texture loads as sRGB and reads back byte-true", "[gpu][d3d12][assets]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    auto assets = Arcane::Assets::Create(device->Nvrhi());
    REQUIRE(assets != nullptr);

    const auto png = WriteTestPng();
    nvrhi::TextureHandle tex = assets->GetTexture(png);
    REQUIRE(tex != nullptr);
    CHECK(tex->getDesc().format == nvrhi::Format::SRGBA8_UNORM);
    CHECK(tex->getDesc().width == 2);

    // Cached: identical handle.
    CHECK(assets->GetTexture(png) == tex);

    // Copy to staging and compare raw bytes (copy does not convert).
    auto stagingDesc = nvrhi::TextureDesc()
        .setWidth(2).setHeight(2)
        .setFormat(nvrhi::Format::SRGBA8_UNORM)
        .setDebugName("AssetsReadback");
    auto staging = device->Nvrhi()->createStagingTexture(
        stagingDesc, nvrhi::CpuAccessMode::Read);
    auto commandList = device->Nvrhi()->createCommandList();
    commandList->open();
    commandList->copyTexture(staging, nvrhi::TextureSlice(),
                             tex, nvrhi::TextureSlice());
    commandList->close();
    device->Nvrhi()->executeCommandList(commandList);
    device->Nvrhi()->waitForIdle();

    size_t rowPitch = 0;
    const auto* pixels = static_cast<const uint8_t*>(
        device->Nvrhi()->mapStagingTexture(staging, nvrhi::TextureSlice(),
                                           nvrhi::CpuAccessMode::Read, &rowPitch));
    REQUIRE(pixels != nullptr);
    CHECK((int)pixels[0] == 255);             // texel (0,0) R
    CHECK((int)pixels[rowPitch + 4] == 128);  // texel (0,1)... row 1, texel 1 R
    device->Nvrhi()->unmapStagingTexture(staging);
    device->Nvrhi()->runGarbageCollection();

    // Memoized failure: a missing path returns null both times (one
    // ARC_ERROR, no retry storm), and bytes/json loaders behave.
    CHECK(assets->GetTexture("does/not/exist.png") == nullptr);
    CHECK(assets->GetTexture("does/not/exist.png") == nullptr);

    auto bytes = assets->GetBytes("data/fonts/Roboto-Regular.ttf");
    REQUIRE(bytes != nullptr);
    CHECK(bytes->size() > 10000);

    CHECK(Arcane::RenderErrorCount() == 0);
    std::remove(png.string().c_str());
}
```

NOTE on stb_image_write: M2a's `VendorSmokeTest.cpp` already defines `STB_IMAGE_WRITE_IMPLEMENTATION` in the test exe — include WITHOUT the implementation define here (drop the GUARD line above; it is a reminder, not real code). Verify `nvrhi::Format::SRGBA8_UNORM` is the actual enumerator spelling in nvrhi.h (adapt to `SRGBA8_UNORM`/`RGBA8_SRGB`/etc. and record). Texel math: row 1 starts at `pixels + rowPitch`; texel (1,1) R is `pixels[rowPitch + 4]` — the comment above says (0,1)/texel 1: it is row 1, second texel, value 128 per the source array.

- [ ] **Step 6: Write `Assets.hpp` (exported facade) + `Assets.cpp`**

`Assets.hpp`:

```cpp
#pragma once

// Assets module: synchronous loose-file loaders behind cache.lua-semantics
// bookkeeping (refcount/bytes/LRU, memoized failures -- a failed path logs
// once and never retries). Color textures upload as sRGB so sampling
// yields linear (the all-linear-canvas contract). Async/streaming/BCn are
// later milestones (north star).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Arcane
{
    struct AssetStats
    {
        uint64_t totalBytes = 0;
        uint32_t count = 0;
    };

    class ARCANE_API Assets
    {
    public:
        static std::unique_ptr<Assets> Create(nvrhi::IDevice* device);
        virtual ~Assets() = default;

        // Color texture (sRGB). Null on failure (logged once, memoized).
        virtual nvrhi::TextureHandle GetTexture(
            const std::filesystem::path& path) = 0;

        // Raw file bytes (fonts, blobs). Null on failure (memoized).
        virtual std::shared_ptr<const std::vector<uint8_t>> GetBytes(
            const std::filesystem::path& path) = 0;

        // Parsed JSON document (UI/data files). Null on failure (memoized).
        virtual std::shared_ptr<const nlohmann::json> GetJson(
            const std::filesystem::path& path) = 0;

        virtual AssetStats Stats() const = 0;
    };
}
```

(`#include <Json.hpp>` — the nlohmann single header on the DLL's include path; declare via the real header, not a forward decl. GetJson impl: GetBytes-style read → `nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false)`; `is_discarded()` → PutFailure + ARC_ERROR. Cache bytes = file size. Add a test case: write a temp JSON, GetJson returns the parsed doc, second call returns the SAME shared_ptr, malformed JSON memoizes failure.)

`Assets.cpp` (anonymous-namespace impl): two `AssetCache` members (`AssetCache<nvrhi::TextureHandle>` keyed by normalized path string, bytes = w*h*4; `AssetCache<std::shared_ptr<const std::vector<uint8_t>>>`, bytes = file size). `GetTexture`: check cache (`Has`→`IsFailure`→return null / `Get`); else `stbi_load(path, &w,&h,&comp, 4)` (include `<stb_image.h>` WITHOUT the implementation define — the test exe owns it... NO: the DLL needs its own stb implementation TU since the test exe's copy is a different module. Create `Assets/StbImpl.cpp` in the DLL with `#define STB_IMAGE_IMPLEMENTATION` + include — and verify no symbol clash: stb in the DLL is internal, the test exe's copy is separate, both static, fine). On load: create `nvrhi::TextureDesc` (SRGBA8_UNORM, ShaderResource initial state, keepInitialState, debugName = filename), upload via a transient command list (`open → writeTexture(tex, 0, 0, data, (size_t)w * 4) → close → executeCommandList`) — the Batcher2D white-texel upload in `Batcher2D.cpp` Init is the exact pattern. `stbi_image_free` after. Failure paths: `PutFailure` + `ARC_ERROR` once. `GetBytes`: read via ifstream binary (ShaderLibrary's `ReadFileBytes` is the pattern — implement locally, don't export). Relative paths: resolve exe-relative like ShaderLibrary does (reuse the same `#ifdef _WIN32 GetModuleFileNameW` block; consistency note in a comment). `Stats`: sum of both caches.

`GetJson`: third cache (`AssetCache<std::shared_ptr<const nlohmann::json>>`), GetBytes-style read, non-throwing parse, `is_discarded()` → memoized failure (per the facade header's note above).

- [ ] **Step 7: Build + run; full suite green. Step 8: Commit** `feat(arcane): Assets module - cache.lua-semantics bookkeeping + sRGB texture/bytes loaders`.

---

### Task 4: GlyphAtlas + msdf shader + Batcher2D::Glyph

**Files:** Create `Arcane/Arcane/src/Arcane/Text/TextSystem.hpp`, `Text/TextSystem.cpp`, `Arcane/shaders/msdf.hlsl`, `Arcane/Tests/src/TextTest.cpp`; Modify `Arcane/shaders/compile-shaders.bat`, `Arcane/Arcane/src/Arcane/Render/Batcher2D.hpp` + `.cpp`.

- [ ] **Step 1: Write `Arcane/shaders/msdf.hlsl`**

```hlsl
// MSDF glyph shader: median-of-3 distance reconstruction with screen-space
// AA. kPxRange/kAtlasSize are compile-time constants MIRRORED in
// TextSystem.cpp (GlyphAtlas) -- change both together.

#if SPIRV
    #define VK_PUSH_CONSTANT [[vk::push_constant]]
#else
    #define VK_PUSH_CONSTANT
#endif

struct BatchConstants
{
    float2 invHalfViewport;
    float2 pad;
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<BatchConstants> g_PC;
#define g_invHalfViewport g_PC.invHalfViewport
#else
cbuffer BatchConstantsCB : register(b0) { BatchConstants g_PCData; }
#define g_invHalfViewport g_PCData.invHalfViewport
#endif

static const float kPxRange = 6.0;
static const float kAtlasSize = 1024.0;

struct VSInput
{
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos.x * g_invHalfViewport.x - 1.0,
                        1.0 - input.pos.y * g_invHalfViewport.y,
                        0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float Median3(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}

float4 ps_main(VSOutput input) : SV_Target0
{
    float3 msd = g_Texture.Sample(g_Sampler, input.uv).rgb;
    float sd = Median3(msd.r, msd.g, msd.b);

    // Chlumsky screen-px-range: how many screen pixels one SDF unit spans.
    float2 unitRange = (kPxRange / kAtlasSize).xx;
    float2 screenTexSize = 1.0 / fwidth(input.uv);
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float screenPxDistance = screenPxRange * (sd - 0.5);
    float alpha = saturate(screenPxDistance + 0.5);
    if (alpha <= 0.0)
        discard;
    return float4(input.color.rgb, input.color.a * alpha);
}
```

Add to `compile-shaders.bat` (respecting the stem/entry invariant + `|| exit /b 1`):

```bat
call :compile msdf vs_main vs_6_5 msdf_vs || exit /b 1
call :compile msdf ps_main ps_6_5 msdf_ps || exit /b 1
```

- [ ] **Step 2: Batcher2D gains the Text kind.** In `Batcher2D.hpp`, after `Quad(...)`:

```cpp
        // MSDF glyph quad: same geometry as Quad but rendered through the
        // msdf pipeline (median-of-3 distance + screen-space AA). Text
        // stays on the single submission path.
        virtual void Glyph(glm::vec2 dstPos, glm::vec2 dstSize,
                           nvrhi::ITexture* atlas,
                           glm::vec2 uvMin, glm::vec2 uvMax,
                           glm::vec4 color) = 0;
```

In `Batcher2D.cpp`: `enum class BatchKind : uint8_t { Sprite, Circle, Text };` and the override:

```cpp
            void Glyph(glm::vec2 dstPos, glm::vec2 dstSize,
                       nvrhi::ITexture* atlas, glm::vec2 uvMin,
                       glm::vec2 uvMax, glm::vec4 color) override
            {
                PushQuad(BatchKind::Text, atlas ? atlas : m_whiteTexture.Get(),
                         dstPos, dstSize, uvMin, uvMax, color);
            }
```

In `GetPipeline`, the kind→shader mapping becomes a switch (sprite_vs/ps, circle_vs/ps, msdf_vs/ps for Text) — same blend/raster/depth state; the cache key already incorporates kind (`* 2 + kind` becomes `* 4 + (size_t)kind` to keep keys disjoint with three kinds). In `Init`, validate `msdf_vs`/`msdf_ps` exist alongside the others.

- [ ] **Step 3: Failing GPU test (first half of `Arcane/Tests/src/TextTest.cpp`)**

```cpp
// Text stack: a glyph drawn huge on a small canvas must paint pixels
// inside its ink and leave far corners untouched; layout tests join below.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Text/TextSystem.hpp>

#include "Helpers/GpuTestHelpers.hpp"

namespace
{
    void CheckGlyphRenders(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "shaders");
        REQUIRE(shaders != nullptr);
        auto canvas = Arcane::CreateCanvas(device->Nvrhi(), 64, 64);
        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
        auto text = Arcane::TextSystem::Create(device->Nvrhi());
        REQUIRE(text != nullptr);

        const Arcane::FontId font = text->LoadFont("data/fonts/Roboto-Regular.ttf");
        REQUIRE(font != Arcane::kInvalidFontId);

        auto commandList = device->Nvrhi()->createCommandList();
        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                       nvrhi::Color(0, 0, 0, 1));
        batcher->Begin(commandList, canvas->Framebuffer(), 64, 64);
        // 'H' nearly fills the canvas: solid vertical strokes at the sides.
        text->Draw(*batcher, font, 56.0f, glm::vec2(6.0f, 56.0f), "H",
                   glm::vec4(1, 1, 1, 1));
        text->Flush(commandList);
        batcher->End();

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(64).setHeight(64)
            .setFormat(nvrhi::Format::RGBA16_FLOAT)
            .setDebugName("TextReadback");
        auto staging = device->Nvrhi()->createStagingTexture(
            stagingDesc, nvrhi::CpuAccessMode::Read);
        commandList->copyTexture(staging, nvrhi::TextureSlice(),
                                 canvas->Texture(), nvrhi::TextureSlice());
        commandList->close();
        device->Nvrhi()->executeCommandList(commandList);
        device->Nvrhi()->waitForIdle();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            device->Nvrhi()->mapStagingTexture(
                staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);

        // Count lit pixels (R channel fp16 > 0.5) -- 'H' ink must cover a
        // meaningful area; canvas corners stay dark.
        int lit = 0;
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
            {
                const auto* texel = reinterpret_cast<const uint16_t*>(
                    pixels + y * rowPitch + x * 8);
                if (HalfToFloat(texel[0]) > 0.5f)
                    ++lit;
            }
        CHECK(lit > 200);  // strokes of a 56px 'H'

        const auto* corner = reinterpret_cast<const uint16_t*>(pixels);
        CHECK(HalfToFloat(corner[0]) < 0.05f);

        device->Nvrhi()->unmapStagingTexture(staging);
        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: MSDF glyph renders through the batcher", "[gpu][d3d12][text]")
{
    CheckGlyphRenders(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: MSDF glyph renders through the batcher", "[gpu][vulkan][text]")
{
    CheckGlyphRenders(Arcane::GraphicsBackend::Vulkan);
}
```

(`HalfToFloat` currently lives in BatcherTest.cpp's anonymous namespace — MOVE it into `Tests/src/Helpers/GpuTestHelpers.hpp` as an inline function and delete the local copy so both tests share it.)

- [ ] **Step 4: Write `Arcane/Arcane/src/Arcane/Text/TextSystem.hpp`**

```cpp
#pragma once

// Text module: MSDF glyph text on the single submission path. Fonts load
// from disk bytes (FreeType via msdfgen's bridge); glyphs are generated
// on demand into ONE 1024x1024 RGBA8 atlas packed by SkylinePacker, and
// drawn as Batcher2D::Glyph quads. Constants kAtlasSize/kPxRange mirror
// msdf.hlsl. Latin layout only (UTF-8 decode + FreeType kerning + \n);
// HarfBuzz arrives when non-Latin does (stack-spec deferral).
//
// Recording order per frame: batcher.Begin -> Draw... -> Flush(cl) ->
// batcher.End  (Flush records the atlas upload before End records draws).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace Arcane
{
    class Batcher2D;

    using FontId = uint32_t;
    inline constexpr FontId kInvalidFontId = 0;

    class ARCANE_API TextSystem
    {
    public:
        static std::unique_ptr<TextSystem> Create(nvrhi::IDevice* device);
        virtual ~TextSystem() = default;

        // Loads a TTF/OTF (exe-relative resolution like ShaderLibrary).
        // Returns kInvalidFontId on failure (logged).
        virtual FontId LoadFont(const std::filesystem::path& path) = 0;

        // Pushes glyph quads for utf8 text at baseline pos (pixels, y
        // down). New glyphs render into the atlas CPU-side; call Flush
        // before Batcher2D::End so the upload records first.
        virtual void Draw(Batcher2D& batcher, FontId font, float sizePx,
                          glm::vec2 baselinePos, std::string_view utf8,
                          glm::vec4 color) = 0;

        // Width/height of the laid-out text (pixels) at sizePx.
        virtual glm::vec2 Measure(FontId font, float sizePx,
                                  std::string_view utf8) = 0;

        // Records the atlas upload if glyphs were added since last Flush.
        virtual void Flush(nvrhi::ICommandList* commandList) = 0;

        // The atlas texture (debug/inspection).
        virtual nvrhi::ITexture* AtlasTexture() const = 0;
    };
}
```

- [ ] **Step 5: Write `Text/TextSystem.cpp`** — the meat. Structure (complete the implementation following these exact mechanics; Task 1's recorded msdfgen call forms are the API reference):

```cpp
// Anonymous-namespace impl. Members:
//   msdfgen::FreetypeHandle* m_freetype;
//   struct Font { msdfgen::FontHandle* handle;
//                 std::shared_ptr<const std::vector<uint8_t>> bytes; // keep alive
//                 double unitsPerEm; double ascender; double lineHeight;
//                 std::unordered_map<uint64_t, GlyphEntry> glyphs; };  // key: codepoint
//   std::vector<Font> m_fonts;                 // FontId = index + 1
//   SkylinePacker m_packer{kAtlasSize, kAtlasSize};
//   std::vector<uint8_t> m_atlasPixels;        // kAtlasSize^2 * 4, RGBA8
//   nvrhi::TextureHandle m_atlas;              // RGBA8_UNORM (LINEAR -- distances are data, NOT color)
//   bool m_atlasDirty;
// Constants: kAtlasSize = 1024; kPxRange = 6.0; kGlyphEmPx = 48.0 (em box render size); kPadding = 2.
//
// Create: initializeFreetype; create the atlas texture (RGBA8_UNORM,
//   ShaderResource initial + keepInitialState, debugName "GlyphAtlas");
//   zero m_atlasPixels; m_atlasDirty = true (first Flush uploads the
//   cleared atlas).
// LoadFont: read file bytes (exe-relative resolve, ShaderLibrary pattern);
//   msdfgen::loadFontData(m_freetype, bytes.data(), (int)bytes.size());
//   query metrics via msdfgen::getFontMetrics (FontMetrics: emSize,
//   ascenderY, descenderY, lineHeight -- with FONT_SCALING_EM_NORMALIZED
//   these are em-normalized; record actual API from Task 1). Store; return id.
// GlyphEntry { glm::vec2 uvMin, uvMax;       // atlas uv
//              glm::vec2 planeMin, planeMax; // em-normalized quad bounds
//              double advance; bool hasInk; };
// EnsureGlyph(font, codepoint): cached? return. Else:
//   msdfgen::loadGlyph(shape, font.handle, codepoint, FONT_SCALING_EM_NORMALIZED, &advance);
//   if shape empty (space): store advance, hasInk=false.
//   shape.normalize(); edgeColoringSimple(shape, 3.0);
//   bounds = shape.getBounds(); pad bounds by kPxRange/kGlyphEmPx em units;
//   bitmap size = ceil((bounds extent) * kGlyphEmPx) + 2*kPadding;
//   packer.Insert(w, h) -- on nullopt: ARC_ERROR("glyph atlas full") and
//   store hasInk=false (full-atlas handling is a later milestone; the
//   1024 atlas fits hundreds of 48px glyphs);
//   generateMSDF into msdfgen::Bitmap<float,3>(w, h) with projection
//   scale = kGlyphEmPx, translate = -bounds.l + padding-in-em (the Task 1
//   smoke test's recorded transform form, adjusted);
//   blit float [0,1]-clamped *255 into m_atlasPixels at the packed pos
//   (RGB from the bitmap, A=255); m_atlasDirty = true;
//   uv = packed rect / kAtlasSize; plane bounds = padded bounds.
// Draw: decode utf8 codepoints (inline decoder: 1-4 byte sequences,
//   invalid -> U+FFFD skip); '\n' -> x = start.x, y += lineHeight*sizePx;
//   per glyph: EnsureGlyph; kerning via msdfgen::getKerning(font.handle,
//   prev, cur) (em-normalized) * sizePx; if hasInk:
//   dstPos = baseline + (planeMin.x, -planeMax.y) * sizePx  (plane Y up ->
//   screen Y down), dstSize = (planeMax-planeMin)*sizePx;
//   batcher.Glyph(dstPos, dstSize, m_atlas, uvMin, uvMax, color);
//   x += advance * sizePx.
// Measure: same walk, no draws; returns (maxLineWidth, lines*lineHeight*sizePx).
// Flush: if m_atlasDirty: commandList->writeTexture(m_atlas, 0, 0,
//   m_atlasPixels.data(), kAtlasSize * 4); m_atlasDirty = false.
// Destructor: destroyFont per font; deinitializeFreetype.
```

Write this as REAL code (the comment block above is the design contract, not the deliverable). The msdfgen calls MUST match the forms recorded by Task 1's smoke test. The atlas texture format is `RGBA8_UNORM` (NOT sRGB — distance data). UV V-axis: msdfgen bitmaps are bottom-up by default (`Bitmap(x, y)` with y=0 at bottom) — blit with a vertical flip into the top-down atlas, and verify the glyph isn't upside-down via the test (if 'H' looks fine either way, add a 'L' coverage check: lit pixels concentrated in the LEFT column and BOTTOM row — bottom row y near baseline → high y values in screen space).

- [ ] **Step 6: Build, run `"[text]"` + full suite (both backends green, validation silent). Step 7: Commit** `feat(arcane): MSDF glyph atlas + TextSystem core + Batcher2D Glyph path`.

---

### Task 5: Text layout polish — kerning/newline/Measure CPU tests

**Files:** Modify `Arcane/Tests/src/TextTest.cpp` (append).

- [ ] **Step 1: Append CPU layout tests** (no GPU needed beyond device-free calls? `TextSystem::Create` takes a device for the atlas — use one D3D12 device; Measure paths don't render):

```cpp
TEST_CASE("text: measure and layout invariants", "[gpu][d3d12][text]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    auto text = Arcane::TextSystem::Create(device->Nvrhi());
    const Arcane::FontId font = text->LoadFont("data/fonts/Roboto-Regular.ttf");
    REQUIRE(font != Arcane::kInvalidFontId);

    const float size = 32.0f;
    const auto wide = text->Measure(font, size, "MMMM");
    const auto narrow = text->Measure(font, size, "iiii");
    CHECK(wide.x > narrow.x);                    // proportional advances
    CHECK(wide.x > 4 * 0.5f * size);             // 'M' is a wide glyph

    const auto one = text->Measure(font, size, "Hello");
    const auto two = text->Measure(font, size, "Hello\nHello");
    CHECK(two.y > one.y * 1.7f);                 // second line adds height
    CHECK(std::abs(two.x - one.x) < 0.5f);       // same widest line

    const auto scaled = text->Measure(font, size * 2.0f, "Hello");
    CHECK(std::abs(scaled.x - one.x * 2.0f) < 1.0f);  // linear in size

    CHECK(text->Measure(font, size, "").x == 0.0f);

    // Kerning: "AV" should be no wider than advance-sum "A"+"V".
    const float av = text->Measure(font, size, "AV").x;
    const float sum = text->Measure(font, size, "A").x +
                      text->Measure(font, size, "V").x;
    CHECK(av <= sum + 0.01f);

    CHECK(Arcane::RenderErrorCount() == 0);
}
```

(Note: Roboto may carry kerning in GPOS, which FreeType's `FT_Get_Kerning`/msdfgen `getKerning` does NOT read (kern table only) — if the AV check shows zero kerning, the `<=` assertion still passes; that is deliberate. Document in the test comment if observed.)

- [ ] **Step 2: Run; fix layout bugs the tests surface (Measure/Draw share one walk — factor a private `LayoutWalk` if they diverged). Step 3: Commit** `test(arcane): text layout invariants - advances, newlines, scaling, kerning`.

---

### Task 6: ImGui into the workspace — wrapper + vendored SDL3 backend

**Files:** Create `ThirdParty/imgui/premake5.lua`, vendor `ThirdParty/imgui/backends/imgui_impl_sdl3.{h,cpp}`; Modify `Arcane/premake5.lua`, `ThirdParty/README.md`; Create `Arcane/Tests/src/ImGuiTest.cpp` (context smoke only this task).

- [ ] **Step 1: Vendor the SDL3 platform backend.** Vendored imgui is `1.92.9 WIP` docking (imgui.h:32). Fetch `backends/imgui_impl_sdl3.h/.cpp` from upstream `ocornut/imgui` docking branch at a commit whose `imgui.h` says the same `IMGUI_VERSION_NUM` (clone `--branch docking --depth 50` and check; if HEAD moved past 1.92.9, find the matching commit via `git log -S "1.92.9" -- imgui.h`). Copy the two files into `ThirdParty/imgui/backends/`. Record the source SHA in the README imgui row. Compile sanity comes in Step 3.

- [ ] **Step 2: Write `ThirdParty/imgui/premake5.lua`** (NEW — safe: Tools lists imgui sources explicitly in its own premake and never `include`s this dir):

```lua
-- Dear ImGui premake5 build script (Arcane workspace consumer).
-- MIT -- immediate-mode UI. Compiles imgui core + the SDL3 platform
-- backend; the NVRHI renderer backend is first-party engine code
-- (Arcane/Arcane/src/Arcane/ImGui). Tools/ compiles imgui sources
-- directly into its vcxproj and does not use this wrapper.
project "imgui"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp",
        "imgui_widgets.cpp", "imgui_demo.cpp",
        "imgui.h", "imgui_internal.h",
        "backends/imgui_impl_sdl3.cpp", "backends/imgui_impl_sdl3.h",
    }

    includedirs { ".", THIRDPARTY_SDL3_INCLUDE or "." }

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

- [ ] **Step 3: Wire the Arcane workspace**: in `Arcane/premake5.lua` set `THIRDPARTY_SDL3_INCLUDE = VCPKG_INSTALLED_MD .. "/include"` next to the other THIRDPARTY_ globals; `include "../ThirdParty/imgui"` in Dependencies; `IncludeDir["imgui"] = "%{wks.location}/../ThirdParty/imgui"`; add `"%{IncludeDir.imgui}"` to the Arcane DLL + ArcaneTests includedirs and `"imgui"` to the DLL links.

- [ ] **Step 4: Context smoke `Arcane/Tests/src/ImGuiTest.cpp`** (headless — no window/GPU):

```cpp
// ImGui arrival gate: core compiles into the DLL's workspace flavor and a
// context round-trips headlessly. The renderer/platform integration test
// joins this file with the ImGuiLayer task.

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

TEST_CASE("imgui: context creates headlessly", "[imgui]")
{
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    REQUIRE(ctx != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640, 360);
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);  // builds default font
    ImGui::NewFrame();
    ImGui::Begin("smoke");
    ImGui::Text("hello");
    ImGui::End();
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData() != nullptr);
    ImGui::DestroyContext(ctx);
}
```

(1.92 note: `GetTexDataAsRGBA32` may be legacy-but-present; if compile fails, the modern headless path is just `NewFrame` without it — fonts build lazily under `RendererHasTextures`. Adapt + record.)

- [ ] **Step 5: Build + run; README rows (imgui row gains backend SHA; note the wrapper). Step 6: Commit** `feat(thirdparty): imgui wrapper for the Arcane workspace + vendored imgui_impl_sdl3`.

---

### Task 7: imgui_impl_nvrhi + ImGuiLayer + Window event tap

**Files:** Create `Arcane/Arcane/src/Arcane/ImGui/ImGuiNvrhi.hpp`, `ImGui/ImGuiNvrhi.cpp`, `ImGui/ImGuiLayer.hpp`, `ImGui/ImGuiLayer.cpp`, `Arcane/shaders/imgui.hlsl`; Modify `Arcane/Arcane/src/Arcane/Platform/Window.{hpp,cpp}`, `Arcane/shaders/compile-shaders.bat`, `Arcane/Tests/src/ImGuiTest.cpp`.

**THE reference implementation is the vendored `ThirdParty/imgui/backends/imgui_impl_dx11.cpp`** — it shows the exact 1.92 texture protocol (`draw_data->Textures` iteration, `ImTextureStatus_WantCreate/WantUpdates/WantDestroy`, `tex->SetTexID`/`SetStatus`) and the RenderDrawData loop shape (vertex/index upload, per-cmd scissor + texture, `idx_offset`/`vtx_offset`). Port that structure onto NVRHI.

- [ ] **Step 1: `Arcane/shaders/imgui.hlsl`** (ImDrawVert layout: float2 pos, float2 uv, RGBA8 col; push constants carry the ortho scale/translate like the dx11 backend's projection):

```hlsl
// Dear ImGui render shader. Vertices arrive in display pixels; push
// constants carry scale/translate to clip space (the standard ImGui
// orthographic transform). Vertex color is sRGB-ish UI color -- ImGui
// draws POST-tonemap into the display-referred backbuffer.

#if SPIRV
    #define VK_PUSH_CONSTANT [[vk::push_constant]]
#endif

struct ImGuiConstants
{
    float2 scale;
    float2 translate;
};

#if SPIRV
[[vk::push_constant]] ConstantBuffer<ImGuiConstants> g_PC;
#define g_scale g_PC.scale
#define g_translate g_PC.translate
#else
cbuffer ImGuiConstantsCB : register(b0) { ImGuiConstants g_PCData; }
#define g_scale g_PCData.scale
#define g_translate g_PCData.translate
#endif

struct VSInput
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

VSOutput vs_main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos * g_scale + g_translate, 0.0, 1.0);
    output.uv = input.uv;
    output.col = input.col;
    return output;
}

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

float4 ps_main(VSOutput input) : SV_Target0
{
    return input.col * g_Texture.Sample(g_Sampler, input.uv);
}
```

`compile-shaders.bat`: `call :compile imgui vs_main vs_6_5 imgui_vs || exit /b 1` + ps line.

- [ ] **Step 2: Window event tap.** `Window.hpp` additions (public, after `SetSize`):

```cpp
        // Native event tap for engine-internal modules (ImGui): invoked
        // once per SDL event during PumpEvents, BEFORE Window's own
        // handling. `event` is a const SDL_Event* passed opaquely so SDL
        // types stay out of public headers. One tap; null uninstalls.
        using NativeEventTap = void (*)(const void* event, void* user);
        void SetNativeEventTap(NativeEventTap tap, void* user);
```

`Window.cpp`: two members (`NativeEventTap m_tap = nullptr; void* m_tapUser = nullptr;` — function pointer + void*, exported-class-safe), setter, and in `PumpEvents`'s poll loop first line: `if (m_tap) m_tap(&e, m_tapUser);`.

- [ ] **Step 3: `ImGui/ImGuiNvrhi.hpp/.cpp`** — internal (not exported) renderer backend, namespace `Arcane`:

```cpp
// ImGuiNvrhi.hpp -- first-party Dear ImGui renderer backend on NVRHI
// (1.92 ImGuiBackendFlags_RendererHasTextures protocol; reference:
// ThirdParty/imgui/backends/imgui_impl_dx11.cpp). Engine-internal --
// ImGuiLayer is the exported facade.
class ImGuiNvrhiRenderer
{
public:
    bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders);
    void Shutdown();   // destroys ImTextureData-owned textures
    void RenderDrawData(ImDrawData* drawData, nvrhi::ICommandList* commandList,
                        nvrhi::IFramebuffer* target);
private:
    // members: device, shaders ref, sampler (linear clamp), binding layout
    // {PushConstants(0, 16), Texture_SRV(0), Sampler(0)} visibility All,
    // input layout (POSITION RG32_FLOAT off 0, TEXCOORD RG32_FLOAT off 8,
    // COLOR RGBA8_UNORM off 16, stride sizeof(ImDrawVert) -- static_assert
    // == 20), grow-only vertex/index BufferHandles (ImDrawIdx is 16-bit:
    // index buffer format R16_UINT), generation-keyed pipeline cache
    // (hash(FramebufferInfo)), per-ITexture* binding-set cache, and a
    // std::vector<nvrhi::TextureHandle> m_ownedTextures keyed by
    // ImTextureData (store the handle pointer in tex->BackendUserData or
    // an unordered_map<ImTextureData*, TextureHandle>).
};
```

`Init`: set `io.BackendRendererName = "imgui_impl_nvrhi"`, `io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures` (and `ImGuiBackendFlags_RendererHasVtxOffset`); create sampler/layouts/input layout against `imgui_vs`; validate shaders.

`RenderDrawData` (port the dx11 loop):
1. For each `ImTextureData* tex : *drawData->Textures` with `tex->Status != ImTextureStatus_OK`: `WantCreate` → create RGBA8_UNORM (NOT sRGB; ImGui colors are display-referred and the target is the display-referred backbuffer) texture w/h from tex, `writeTexture` full upload from `tex->GetPixels()` (pitch `tex->GetPitch()`), `tex->SetTexID((ImTextureID)(intptr_t)handle.Get())`, map-store the handle, `SetStatus(OK)`. `WantUpdates` → full re-upload from GetPixels (region staging later), `SetStatus(OK)`. `WantDestroy` (and `UnusedFrames > 0` per dx11 reference) → drop the handle from the map, `SetTexID(ImTextureID_Invalid)`, `SetStatus(Destroyed)`.
2. Grow VB/IB to `TotalVtxCount/TotalIdxCount`; concatenate all cmd lists' `VtxBuffer/IdxBuffer` into CPU scratch; one `writeBuffer` each.
3. Push constants: `scale = 2/DisplaySize`, `translate = -1 - DisplayPos*scale` with Y NEGATED to match the VS above outputting y directly (dx11 ortho flips Y; our VS uses `pos*scale+translate` straight — so `scale.y` must be NEGATIVE: `scale = (2/w, -2/h)`, `translate = (-1 - pos.x*sx, 1 - pos.y*sy)`; verify visually, the ImGui demo window upside-down means flip it).
4. Per ImDrawList, per ImDrawCmd: skip `UserCallback` (assert none for v1); compute clip rect minus `DisplayPos`, skip degenerate; `GraphicsState` (pipeline for target fbinfo, binding set for `(nvrhi::ITexture*)cmd->GetTexID()`, VB+IB R16_UINT, viewport = full target + scissor = clip via `state.viewport.addViewportAndScissorRect`... NVRHI viewport state couples viewport+scissor arrays — set ONE viewport and per-cmd scissor: `state.viewport.addViewport(full); state.viewport.addScissorRect(nvrhi::Rect(clipMinX, clipMaxX, clipMinY, clipMaxY))` — verify `nvrhi::Rect` field order `minX,maxX,minY,maxY` in nvrhi.h); `setGraphicsState`, `setPushConstants`, `drawIndexed({.vertexCount=cmd->ElemCount, .startIndexLocation=idxOffset+cmd->IdxOffset, .startVertexLocation=vtxOffset+cmd->VtxOffset})`.
5. Pipeline: alpha blend SrcAlpha/InvSrcAlpha (+alpha One/InvSrcAlpha), cull none, no depth, TriangleList, scissor implied by NVRHI dynamic scissor.

- [ ] **Step 4: `ImGui/ImGuiLayer.hpp/.cpp`** — exported facade:

```cpp
#pragma once

// ImGui module facade: owns the context + both backends (upstream
// imgui_impl_sdl3 for platform/input via Window's native event tap;
// first-party NVRHI renderer). Renders POST-tonemap into the
// display-referred backbuffer framebuffer the caller provides.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace Arcane
{
    class Window;
    class RenderDevice;
    class ShaderLibrary;

    class ARCANE_API ImGuiLayer
    {
    public:
        // Window must outlive the layer (the layer taps its events).
        static std::unique_ptr<ImGuiLayer> Create(Window& window,
                                                  RenderDevice& device,
                                                  ShaderLibrary& shaders);
        virtual ~ImGuiLayer() = default;

        virtual void BeginFrame() = 0;  // sdl3 NewFrame + ImGui::NewFrame
        // ImGui::Render + draw into target (an OPEN command list).
        virtual void Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* target) = 0;
    };
}
```

Impl: `Create` → `IMGUI_CHECKVERSION`, `CreateContext`, `ImGui_ImplSDL3_InitForOther(window.SdlWindow())`, renderer `Init`, install the tap (`window.SetNativeEventTap(&Tap, this)` where `Tap` casts and calls `ImGui_ImplSDL3_ProcessEvent((const SDL_Event*)event)`). Destructor: uninstall tap (null), renderer `Shutdown`, `ImGui_ImplSDL3_Shutdown`, `DestroyContext`. `BeginFrame`: `ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();`. `Render`: `ImGui::Render(); m_renderer.RenderDrawData(ImGui::GetDrawData(), commandList, target);`.

- [ ] **Step 5: Append the GPU test to `ImGuiTest.cpp`**: hidden window + device + ImGuiLayer; one frame: `BeginFrame` → `ImGui::ShowDemoWindow()` → render into an offscreen BGRA8 target (8x8 is too small for the demo — use 256x144 target); readback: assert ANY pixel differs from the clear color (the demo window paints) + `RenderErrorCount() == 0`; run on BOTH backends. Follow AssetsTest's staging pattern.

- [ ] **Step 6: Build + full suite both configs. Step 7: Commit** `feat(arcane): first-party imgui_impl_nvrhi + ImGuiLayer + Window event tap`.

---

### Task 8: Playground — HUD text + ImGui stats overlay

**Files:** Modify `Arcane/Playground/src/main.cpp`.

- [ ] **Step 1:** Setup additions after `tonemap`: `auto text = Arcane::TextSystem::Create(device->Nvrhi());` then `const Arcane::FontId hudFont = text->LoadFont("data/fonts/Roboto-Regular.ttf");` (exit 1 when null/invalid), `auto imgui = Arcane::ImGuiLayer::Create(window, *device, *shaders);` (exit 1 on null). Frame changes: after `events` handling, `imgui->BeginFrame();` then an ImGui window:

```cpp
        ImGui::Begin("Arcane Stats");
        ImGui::Text("Backend: %s", Arcane::ToString(device->Backend()));
        ImGui::Text("Adapter: %s", device->AdapterName().c_str());
        const Arcane::Batch2DStats lastStats = batcher->Stats();
        ImGui::Text("Quads: %u  Draws: %u", lastStats.quads, lastStats.drawCalls);
        ImGui::End();
```

In the batch section (before `End`), HUD line in the scene (linear space, layer 10 like the swatch):

```cpp
        batcher->SetLayer(10, 1);
        char hud[96];
        std::snprintf(hud, sizeof(hud), "Arcane M2b -- %s",
                      Arcane::ToString(device->Backend()));
        text->Draw(*batcher, hudFont, 22.0f, glm::vec2(20.0f, h - 24.0f),
                   hud, glm::vec4(0.9f, 0.9f, 1.0f, 1.0f));
        text->Flush(commandList);
        batcher->End();
```

After `tonemap->Run(...)`: `imgui->Render(commandList, backbufferFb);` (UI post-tonemap). Includes: `<Arcane/Text/TextSystem.hpp>`, `<Arcane/ImGui/ImGuiLayer.hpp>`, `<imgui.h>` (Playground premake includedirs gains `"%{IncludeDir.imgui}"`).

- [ ] **Step 2: Verify** — scripted `--frames 240 --no-vsync` both backends exit 0, zero validation; manual run: HUD text crisp bottom-left, ImGui stats window draggable/interactive (mouse reaches it — proves the event tap), demo still animates behind it. Note in the report: ESC still quits (tap runs BEFORE Window's handling — if ImGui captures keyboard, Window still sees ESC; acceptable for the demo, note `io.WantCaptureKeyboard` as the M3 refinement).
- [ ] **Step 3: Full suite + Release/Dist builds. Step 4: Commit** `feat(playground): M2b demo - MSDF HUD text + ImGui stats overlay`.

---

### Task 9: Docs + sweep (Server regression REQUIRED this time)

**Files:** Modify `CLAUDE.md`.

- [ ] **Step 1: CLAUDE.md Arcane section**: "M2a state:" → "M2b state:" — extend the sentence with "MSDF TextSystem (msdfgen + skyline atlas) and ImGui (first-party `imgui_impl_nvrhi` + upstream `imgui_impl_sdl3`) live in the DLL; Assets facade loads sRGB textures/bytes with cache.lua semantics." Append to the shaders-are-data rule: "msdf.hlsl/imgui.hlsl follow the same artifact conventions; kPxRange/kAtlasSize are mirrored constants between msdf.hlsl and TextSystem.cpp."
- [ ] **Step 2: Full verification** — three configs + ArcaneTests Debug (JUnit, failures=0) + Release.
- [ ] **Step 3: Server regression** — Task 2 added a file under `Arcane/Core/` which the Server workspace's ArcaneCore project globs: `cd Server && GenerateProjects.bat && msbuild Aphelyon.slnx /p:Configuration=Debug /m && bin\Debug-windows-x86_64\CommonTests\CommonTests.exe` — expect clean build + 378 assertions / 60 cases (M1 baseline). Do NOT run AccountTests.
- [ ] **Step 4: Commit** `docs: CLAUDE.md Arcane section - M2b state`.

---

## M2b exit criteria

- [ ] msdfgen vendored (minimal subset, pinned, README'd) and generating real glyph MSDFs.
- [ ] SkylinePacker in Core matches the Lua oracle's semantics (characterization tests); Server workspace still builds + CommonTests green.
- [ ] Assets facade: sRGB textures byte-true on readback, cached handles, memoized failures, bytes loader; cache.lua semantics CPU-tested.
- [ ] TextSystem: glyphs on demand into one skyline-packed atlas, drawn via `Batcher2D::Glyph` (single submission path); GPU coverage + layout invariants green on both backends.
- [ ] ImGui in the DLL: upstream sdl3 platform backend (version-matched, SHA recorded) + first-party NVRHI renderer implementing the 1.92 texture protocol; offscreen demo-frame test green on both backends.
- [ ] Playground: crisp MSDF HUD text + interactive ImGui stats overlay, both backends, validation silent.
- [ ] Full suite green Debug + Release; `RenderErrorCount() == 0` throughout.

Out of scope (per north star): HarfBuzz/rich text/effects, atlas eviction + multi-atlas, async/streaming assets, BCn, ImGui multi-viewport/docking polish (docking branch features may work but are not gated), the M3 backend-swap UI (consumes ImGuiLayer next milestone).

