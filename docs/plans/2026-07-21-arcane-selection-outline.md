# Selection + Hover Outline (GPU edge-detect) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Grimoire viewport an amber outline on the selected entity and a cyan outline on the hovered entity, drawn the AAA way — a per-frame GPU screen-space edge-detect on the existing hit-proxy id buffer.

**Architecture:** Picking (click → entity via `PickBuffer::Pick` readback) is unchanged. A new per-frame system, `Arcane::SelectionOutline`, edge-detects the `R32_UINT` id buffer (`PickBuffer::IdTarget()`) in a fullscreen shader and composites two-color exterior outlines into the post-tonemap viewport framebuffer (`OffscreenCanvas::OutputFramebuffer()`). The hovered id is derived in-shader by sampling the id buffer at the cursor pixel — zero CPU readback, zero stall.

**Tech Stack:** C++23, NVRHI (D3D12 + Vulkan), HLSL (DXC → DXIL+SPIR-V), Astra ECS, glm, Catch2. Spec: `docs/superpowers/specs/2026-07-21-arcane-selection-outline-design.md`.

## Global Constraints

- **`ARCANE_API`, editor-free, game-agnostic:** `Arcane::SelectionOutline` lives in Arcane, knows nothing about Grimoire or any game — it takes an id buffer, two ids/colors, a cursor, and a target framebuffer. Consumed by Grimoire.
- **Two systems stay separate:** picking (`PickBuffer::Pick`) is unchanged; outlining is the new pass. They share only the id buffer.
- **No CPU readback for hover:** the hovered id is `idBuffer.Load(cursorPx)` **in the shader**. Never read the id buffer on the CPU per-frame (that is the stall this design exists to avoid).
- **Colors are display-referred:** written directly into the gamma-encoded `BGRA8` `OutputFramebuffer` (like the game-imgui overlay / `TonemapPass` output), NOT linear-HDR. No linear→sRGB conversion in the outline shader.
- **/MD everywhere; no `/fp:fast`; UTF-8 without BOM; ASCII-only comments.**
- **Shaders are data:** HLSL in `Arcane/shaders/`, compiled by the Arcane prebuild (DXC, DXIL+SPIR-V). SPIR-V register shifts match `nvrhi::VulkanBindingOffsets` (t=0 s=128 b=256 u=384). `msdf.hlsl`/`imgui.hlsl`/`tonemap` are the reference artifacts.
- **Build (PowerShell, VS18 MSBuild — NOT the Bash tool; Git Bash mangles `/p:` `/m`):**
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /t:<Target> /p:Configuration=Debug /m /nologo /v:minimal`
  New source/shader files → run `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` once first. Targets: `ArcaneTests`, `Grimoire`.
- **Run headless tests** from the exe dir: `cd "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"` then `.\ArcaneTests.exe "<filter>"`.
- **`[gpu]` tests are DESK/CI-ONLY** (Parsec GPU-driver crash hazard headless). Run only `~[gpu]` filters headless; author `[gpu]` tests + build-verify them, but their runtime pass is desk/CI. `[gpu]` tests assert `Arcane::RenderErrorCount() == 0`.
- **Clang/clangd diagnostics in this workspace are NOISE** (wrong toolchain — "expected Clang 20", "Api.hpp not found"). MSVC build is the sole source of truth.
- **Commits:** single-line `type(scope): summary`, NO body, NO AI trailers.
- **Baselines (must not drop):** `~[gpu]` 27849/344, `[grimoire]` 84/12. Task 1 adds headless `[pick]` assertions (grows `~[gpu]`); Task 2 adds a `[gpu]` case (NOT in `~[gpu]`).

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Render/PickBuffer.hpp` | Modify | Add `PassIdOf(Astra::Entity) -> uint32_t`. |
| `Arcane/Arcane/src/Arcane/Render/PickBuffer.cpp` | Modify | Retain last-pass entity order; back `PassIdOf`. |
| `Arcane/Arcane/src/Arcane/Render/PickEmit.hpp` | Modify | Declare the pure `PickPassId(ordered, entity)` helper. |
| `Arcane/Arcane/src/Arcane/Render/PickEmit.cpp` | Modify | Implement `PickPassId`. |
| `Arcane/Arcane/src/Arcane/Render/SelectionOutline.hpp` | Create | `ARCANE_API` facade: `Create` + `Render(cmd, idBuffer, target, Params)`. |
| `Arcane/Arcane/src/Arcane/Render/SelectionOutline.cpp` | Create | Impl: pipeline (mirror `TonemapPass`) + per-draw CB fill + fullscreen draw. |
| `Arcane/shaders/selection_outline.hlsl` | Create | Fullscreen VS + exterior two-color edge-detect PS over `R32_UINT`. |
| `Arcane/Tests/src/SelectionOutlineTest.cpp` | Create | `[gpu]` render test + `[pick]` `PickPassId` test. |
| `Arcane/Grimoire/src/GrimoireApp.hpp` | Modify | Own `std::unique_ptr<Arcane::SelectionOutline> m_outline`. |
| `Arcane/Grimoire/src/GrimoireApp.cpp` | Modify | Create it; per-frame Edit-only outline pass into `OutputFramebuffer()`. |

---

## Task 1: `PickBuffer::PassIdOf` + pure `PickPassId` (headless `[pick]` TDD)

The CPU seam the outline needs: the pass id assigned to the selected entity. The k+1 assignment is deterministic from the collected order, so the lookup is a pure function — unit-tested headless. GPU-free.

**Files:** Modify `PickBuffer.{hpp,cpp}`, `PickEmit.{hpp,cpp}`; test `Arcane/Tests/src/SelectionOutlineTest.cpp`.

**Interfaces:**
- Produces: free `uint32_t Arcane::PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e)` (0 = absent/invalid; else index+1); method `uint32_t PickBuffer::PassIdOf(Astra::Entity e) const`.

- [ ] **Step 0 (read-first):** In `PickEmit.{hpp,cpp}` and `PickBuffer.cpp`, find how the id table is built: `CollectPickables` returns the ordered `std::vector<PickDrawable>` (each `.entity`), and `Pick()` maps a read-back uint via `id = k+1` (`0` = background). Confirm whether the ordered entity list is retained on `PickBuffer` after `RenderIdPass`/`Pick` or built-and-discarded. Confirm the existing pick test file/tag (`[pick]`) and that `Astra::Entity` compares via `GetValue()` and has `Entity::Invalid()`. Confirm `#include` conventions.

- [ ] **Step 1: Write the failing test** — append to `Arcane/Tests/src/SelectionOutlineTest.cpp` (create the file with this):

```cpp
// Selection outline: PickPassId reverse-lookup ([pick], CPU) + the outline pass ([gpu]).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/PickEmit.hpp>

#include <Astra/Entity/Entity.hpp>   // adapt include to the path found in Step 0

#include <vector>

TEST_CASE("PickPassId maps ordered entities to k+1, 0 for absent/invalid", "[pick]")
{
    const Astra::Entity a = Astra::Entity(1, 0);   // adapt ctor to Step 0 (value,version)
    const Astra::Entity b = Astra::Entity(2, 0);
    const Astra::Entity c = Astra::Entity(3, 0);
    const std::vector<Astra::Entity> ordered{ a, b, c };

    CHECK(Arcane::PickPassId(ordered, a) == 1u);
    CHECK(Arcane::PickPassId(ordered, b) == 2u);
    CHECK(Arcane::PickPassId(ordered, c) == 3u);
    CHECK(Arcane::PickPassId(ordered, Astra::Entity(9, 0)) == 0u);   // absent
    CHECK(Arcane::PickPassId(ordered, Astra::Entity::Invalid()) == 0u);
    CHECK(Arcane::PickPassId({}, a) == 0u);                          // empty
}
```

(Adapt the `Astra::Entity` construction + include to the exact API from Step 0 — mirror how `EntityPickTest`/the pick test built entities.)

- [ ] **Step 2: Run headless, verify fail.** `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` (new test file), build `ArcaneTests`, then from the exe dir `.\ArcaneTests.exe "[pick]"` → FAIL (unresolved `Arcane::PickPassId`).

- [ ] **Step 3: Implement `PickPassId`** — declare in `PickEmit.hpp` (near `CollectPickables`):

```cpp
    // The pass id assigned to `e` under the k+1 convention CollectPickables emits
    // (the k-th entity in `ordered` gets id k+1; 0 = background). Reverse of the
    // read-back mapping in Pick(). 0 if `e` is absent or Astra::Entity::Invalid().
    uint32_t PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e);
```

Implement in `PickEmit.cpp`:

```cpp
    uint32_t PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e)
    {
        if (e == Astra::Entity::Invalid())
            return 0u;
        for (std::size_t k = 0; k < ordered.size(); ++k)
            if (ordered[k] == e)
                return static_cast<uint32_t>(k + 1);
        return 0u;
    }
```

Then wire `PassIdOf` on `PickBuffer`. In `PickBuffer.hpp`, after `IdTarget()`:

```cpp
        // The pass id assigned to `e` in the most recent RenderIdPass/Pick (k+1
        // order; 0 = background/absent). Lets a consumer (e.g. the selection
        // outline) know an entity's id without a GPU readback.
        virtual uint32_t PassIdOf(Astra::Entity e) const = 0;
```

In `PickBuffer.cpp`: retain the ordered entity list from the last pass (reuse the table found in Step 0; if `CollectPickables`' output is not retained, add a member `std::vector<Astra::Entity> m_lastPassEntities;` populated wherever `RenderIdPass`/`Pick` collects — store `drawable.entity` in emit order). Implement:

```cpp
        uint32_t PassIdOf(Astra::Entity e) const override
        {
            return PickPassId(m_lastPassEntities, e);
        }
```

(If Step 0 finds an already-retained ordered table, point `PassIdOf` at that instead of adding `m_lastPassEntities` — do not duplicate state.)

- [ ] **Step 4: Build + run, verify PASS.** Build `ArcaneTests`; `.\ArcaneTests.exe "[pick]"` → all pass. `.\ArcaneTests.exe "~[gpu]"` → 27849 + the new `[pick]` assertions (record the exact new count); no other change.

- [ ] **Step 5: Commit** — `feat(arcane): PickBuffer::PassIdOf -- entity -> pass id for the selection outline`.

---

## Task 2: `selection_outline.hlsl` + `Arcane::SelectionOutline` (+ `[gpu]` test)

The render feature: a fullscreen edge-detect pass. GPU-touching, so build-verified here; the `[gpu]` test runs desk/CI (its render path is the same NVRHI fullscreen idiom `TonemapPass` already GPU-proves).

**Files:** Create `SelectionOutline.{hpp,cpp}`, `Arcane/shaders/selection_outline.hlsl`; extend `SelectionOutlineTest.cpp`.

**Interfaces:**
- Consumes: `Arcane::ShaderLibrary`, `nvrhi`, `PickBuffer::IdTarget()` (an `R32_UINT` texture).
- Produces: `class SelectionOutline` with `Create(nvrhi::IDevice*, ShaderLibrary&) -> unique_ptr`, `Render(nvrhi::ICommandList*, nvrhi::ITexture* idBuffer, nvrhi::IFramebuffer* target, const Params&)`, nested `Params{ selectedId, cursorPx, selectColor, hoverColor, selectThicknessPx, hoverThicknessPx }`.

- [ ] **Step 0 (read-first):** Read `TonemapPass.{hpp,cpp}` (the reference fullscreen pass): how it loads its VS+PS from `ShaderLibrary` by name per backend, builds the binding layout + graphics pipeline (fullscreen triangle, no depth), fills/binds its constant buffer, and issues the draw in `Run(...)`; the blend state it uses. Read `Arcane/shaders/` + `compile-shaders.bat` — whether a new `.hlsl` is auto-globbed or must be registered, and the VS/PS entry-point naming convention (e.g. `VSMain`/`PSMain`) + how `ShaderLibrary` keys the compiled artifact. Confirm `Texture2D<uint>` `.Load` is used for `R32_UINT` (not `Sample`) and the SPIR-V register-shift setup. Read `GpuTestHelpers.hpp` for: creating an `R32_UINT` render/sampled texture, UPLOADING known uint texels (writeTexture or staging→copy), creating a `BGRA8` target, and reading it back. Confirm the `[gpu]` tag + `RenderErrorCount()`.

- [ ] **Step 1: Write `Arcane/shaders/selection_outline.hlsl`:**

```hlsl
// Exterior two-color edge-detect over the R32_UINT hit-proxy id buffer, composited
// (display-referred) over the tonemapped viewport. hoveredId is derived IN-SHADER
// from the cursor pixel -- no CPU readback. Design: 2026-07-21-arcane-selection-outline.

cbuffer OutlineCB : register(b0)
{
    uint   gSelectedId;    // 0 = no selection
    int2   gCursorPx;      // viewport-local pixel; x<0 => no hover
    uint   gSelectThick;   // exterior ring radius (px) for the selected outline
    uint   gHoverThick;    // exterior ring radius (px) for the hover outline
    uint3  _pad;
    float4 gSelectColor;   // display-referred (amber)
    float4 gHoverColor;    // display-referred (cyan)
};

Texture2D<uint> gIds : register(t0);

struct VSOut { float4 pos : SV_Position; };

// Fullscreen triangle from SV_VertexID (no vertex/index buffer).
VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Exterior test: pixel `p` is NOT `target` but a texel within `radius` IS.
bool BordersId(int2 p, uint target, int radius)
{
    if (target == 0u) return false;
    if (gIds.Load(int3(p, 0)) == target) return false;   // interior -> not an exterior edge
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            if (gIds.Load(int3(p + int2(dx, dy), 0)) == target) return true;
        }
    return false;
}

float4 PSMain(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    uint hoveredId = (gCursorPx.x >= 0 && gCursorPx.y >= 0)
                   ? gIds.Load(int3(gCursorPx, 0)) : 0u;

    // Select precedence: the selected outline wins on shared pixels.
    if (BordersId(p, gSelectedId, (int)gSelectThick))
        return gSelectColor;
    if (hoveredId != gSelectedId && BordersId(p, hoveredId, (int)gHoverThick))
        return gHoverColor;
    discard;                 // leave the tonemapped scene untouched
    return float4(0.0, 0.0, 0.0, 0.0);
}
```

(Adapt entry-point names + the `cbuffer` packing to the convention from Step 0. HLSL packs `int2 gCursorPx` + the two `uint` thicknesses into 16 bytes; keep the CB struct in `.cpp` byte-identical. If the compile step needs the shader registered in a list, add `selection_outline` there.)

- [ ] **Step 2: Write `SelectionOutline.hpp`:**

```cpp
#pragma once

// A per-frame GPU screen-space edge-detect that draws two-color EXTERIOR outlines
// (selected + hovered) from an R32_UINT hit-proxy id buffer, composited over a
// display-referred target. Editor- and game-agnostic. Sibling of TonemapPass.
// Design: docs/superpowers/specs/2026-07-21-arcane-selection-outline-design.md.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <memory>

namespace Arcane
{
    class ShaderLibrary;

    class ARCANE_API SelectionOutline
    {
    public:
        static std::unique_ptr<SelectionOutline> Create(nvrhi::IDevice*, ShaderLibrary&);
        virtual ~SelectionOutline() = default;

        struct Params
        {
            uint32_t   selectedId = 0;                              // 0 = no selection
            glm::ivec2 cursorPx   = { -1, -1 };                     // viewport-local; <0 => no hover
            glm::vec4  selectColor = { 1.0f, 0.65f, 0.10f, 1.0f };  // amber (display-referred)
            glm::vec4  hoverColor  = { 0.25f, 0.70f, 1.0f, 1.0f };  // cyan  (display-referred)
            uint32_t   selectThicknessPx = 3;
            uint32_t   hoverThicknessPx  = 2;
        };

        // Edge-detect `idBuffer` (R32_UINT) and composite the two-color outlines into
        // `target` on the OPEN command list. hoveredId is sampled in-shader from
        // Params::cursorPx (no readback). Select takes precedence over hover.
        virtual void Render(nvrhi::ICommandList*,
                            nvrhi::ITexture* idBuffer,
                            nvrhi::IFramebuffer* target,
                            const Params&) = 0;
    };
}
```

- [ ] **Step 3: Write `SelectionOutline.cpp`** — mirror `TonemapPass` for the pipeline/binding/draw (adapt every nvrhi call to the exact idiom from Step 0):

```cpp
#include <Arcane/Render/SelectionOutline.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <cstdint>

namespace Arcane
{
    namespace
    {
        struct OutlineCB
        {
            uint32_t  selectedId;
            int32_t   cursorX;
            int32_t   cursorY;
            uint32_t  selectThick;
            uint32_t  hoverThick;
            uint32_t  pad0, pad1, pad2;
            glm::vec4 selectColor;
            glm::vec4 hoverColor;
        };

        class SelectionOutlineImpl final : public SelectionOutline
        {
        public:
            bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders)
            {
                m_device = device;
                // Load "selection_outline" VS+PS from ShaderLibrary (per backend), create
                // the OutlineCB constant buffer, the binding layout (t0 = id texture,
                // b0 = OutlineCB), and a fullscreen-triangle graphics pipeline (no depth,
                // blend so outline pixels composite over the target; PS `discard`s the rest).
                // MIRROR TonemapPass::Init from Step 0. ARC_ERROR + return false on failure.
                return true;   // replace with the real result
            }

            void Render(nvrhi::ICommandList* cmd, nvrhi::ITexture* idBuffer,
                        nvrhi::IFramebuffer* target, const Params& p) override
            {
                OutlineCB cb{};
                cb.selectedId  = p.selectedId;
                cb.cursorX     = p.cursorPx.x;
                cb.cursorY     = p.cursorPx.y;
                cb.selectThick = p.selectThicknessPx;
                cb.hoverThick  = p.hoverThicknessPx;
                cb.selectColor = p.selectColor;
                cb.hoverColor  = p.hoverColor;
                cmd->writeBuffer(m_cb, &cb, sizeof(cb));
                // Build the binding set {idBuffer SRV, m_cb}, set the graphics state
                // (pipeline, framebuffer = target, bindings, full-target viewport), and
                // draw 3 vertices (fullscreen triangle). MIRROR TonemapPass::Run.
            }

        private:
            nvrhi::IDevice*         m_device = nullptr;
            nvrhi::BufferHandle     m_cb;
            // + pipeline, binding layout, shader handles, binding-set cache -- per Step 0.
        };
    }

    std::unique_ptr<SelectionOutline> SelectionOutline::Create(nvrhi::IDevice* device,
                                                              ShaderLibrary& shaders)
    {
        auto pass = std::make_unique<SelectionOutlineImpl>();
        if (!pass->Init(device, shaders))
            return nullptr;
        return pass;
    }
}
```

(The `Init`/`Render` bodies are enumerated read-first adaptations against `TonemapPass` — fill them with the real nvrhi pipeline/binding/draw calls, not the placeholder returns. Keep `OutlineCB` byte-identical to the HLSL `cbuffer`.)

- [ ] **Step 4: Write the `[gpu]` test** — extend `SelectionOutlineTest.cpp`:

```cpp
#include <Arcane/Render/SelectionOutline.hpp>
// + the GPU test harness includes (device, ShaderLibrary, GpuTestHelpers) per Step 0.

TEST_CASE("SelectionOutline draws amber around selectedId, cyan around hovered", "[gpu]")
{
    // Harness: create device + ShaderLibrary (mirror an existing [gpu] render test).
    auto outline = Arcane::SelectionOutline::Create(device, shaders);
    REQUIRE(outline != nullptr);

    // 64x64 R32_UINT id buffer: fill a rect [8..24) with id=5, a rect [40..56) with id=7,
    // background 0. Upload the known uint pattern (writeTexture / staging per Step 0).
    // 64x64 BGRA8 target cleared to a known background color.
    Arcane::SelectionOutline::Params p;
    p.selectedId = 5;
    p.cursorPx   = glm::ivec2(48, 48);   // inside the id=7 rect -> hoveredId = 7

    cmd->open();
    outline->Render(cmd, idTex, targetFb, p);
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();

    // Read back the BGRA8 target. Assert: at least one amber texel just OUTSIDE the
    // id=5 rect edge; at least one cyan texel just outside the id=7 rect edge; a texel
    // deep in the background (far from both rects) is unchanged (== clear color).
    CHECK(Arcane::RenderErrorCount() == 0);
}
```

(Fill the harness + upload + readback from the exact `GpuTestHelpers` APIs in Step 0. This case is `[gpu]` — build-verify it compiles; it RUNS at the desk/CI, not in `~[gpu]`.)

- [ ] **Step 5: Regenerate + build, verify clean.** `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"`; build `ArcaneTests`. Expect zero errors (the prebuild compiles `selection_outline.hlsl` to DXIL+SPIR-V). `.\ArcaneTests.exe "~[gpu]"` → the Task-1 count unchanged (the new case is `[gpu]`, excluded); `.\ArcaneTests.exe "[pick]"` still green.

- [ ] **Step 6: Commit** — `feat(arcane): SelectionOutline -- GPU edge-detect selection + hover outline pass`.

---

## Task 3: Grimoire integration — run the outline per frame in Edit

Grimoire creates the pass and runs it each Edit-mode frame into the viewport's post-tonemap framebuffer. Build-verified; desk-verified in Task 4.

**Files:** Modify `GrimoireApp.{hpp,cpp}`.

**Interfaces:**
- Consumes: `Arcane::SelectionOutline` (Task 2), `PickBuffer::RenderIdPass`/`IdTarget`/`PassIdOf` (Task 1), `OffscreenCanvas::OutputFramebuffer()`.

- [ ] **Step 0 (read-first):** In `GrimoireApp.cpp` confirm: (a) where `m_pick`/`m_viewport` are created in `Init` (to create `m_outline` beside them — `SelectionOutline::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders())`); (b) the exact insertion point AFTER `m_viewport->Draw(...)` and BEFORE `m_gpu->Imgui().BeginFrame()` — the game-imgui overlay pass is already there (Play-only); the outline goes in the same region, Edit-only, so the two are mutually exclusive; (c) the viewport-local cursor members `m_lastViewportMouse` (glm::vec2, viewport px) + `m_lastInViewport` (bool) set in the input block; (d) `m_runtime->CameraOffset()` / `m_runtime->CameraZoom()` for the `PickView`; (e) the one-off command-list submit idiom used by the game-imgui pass (`m_gpu->Cmd()->open()`/`close()`/`executeCommandList`); (f) `m_selection.HasSelection()` / `m_selection.selected` and `m_play.IsPlaying()`.

- [ ] **Step 1: Add state in `GrimoireApp.hpp`.** Add `#include <Arcane/Render/SelectionOutline.hpp>` and a member beside `m_pick`:

```cpp
        std::unique_ptr<Arcane::SelectionOutline> m_outline;  // per-frame selection/hover outline
```

- [ ] **Step 2: Create it in `Init`,** beside `m_pick` (use the device/shaders handles from Step 0):

```cpp
        m_outline = Arcane::SelectionOutline::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders());
        if (!m_outline) { ARC_ERROR("Grimoire: SelectionOutline creation failed"); return false; }
```

- [ ] **Step 3: Run the outline pass per frame (Edit only),** immediately after `m_viewport->Draw(...)` and before the editor `BeginFrame` (adapt names to Step 0):

```cpp
            // Selection + hover outline -> the viewport's own layer (Edit only). Refreshes
            // the hit-proxy id buffer, then edge-detects it into the post-tonemap output
            // texture (amber selected, cyan hovered). Skipped entirely when there is nothing
            // to outline. Play mode: not run (the game-imgui overlay owns this slot instead).
            if (!m_play.IsPlaying() && (m_selection.HasSelection() || m_lastInViewport))
            {
                const Arcane::PickView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
                m_pick->RenderIdPass(m_runtime->Registry(), view);

                Arcane::SelectionOutline::Params op;
                op.selectedId = m_selection.HasSelection()
                              ? m_pick->PassIdOf(m_selection.selected) : 0u;
                op.cursorPx   = m_lastInViewport
                              ? glm::ivec2((int)m_lastViewportMouse.x, (int)m_lastViewportMouse.y)
                              : glm::ivec2(-1, -1);

                m_gpu->Cmd()->open();
                m_outline->Render(m_gpu->Cmd(), m_pick->IdTarget(),
                                  m_viewport->OutputFramebuffer(), op);
                m_gpu->Cmd()->close();
                m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            }
```

- [ ] **Step 4: Build `ArcaneTests;Grimoire`, verify clean + no regression.** Build both; zero errors. `.\ArcaneTests.exe "~[gpu]"` → Task-1 count unchanged; `.\ArcaneTests.exe "[grimoire]"` → 84/12 (no new tests here).

- [ ] **Step 5: Commit** — `feat(grimoire): draw selection + hover outline in the viewport (Edit mode)`.

---

## Task 4: Desk-verify

- [ ] **Step 1: `[gpu]` test at the desk/CI** — run `.\ArcaneTests.exe "[gpu]"` (or the CI GPU lane) → the `SelectionOutline` case passes, `RenderErrorCount() == 0`. Record the result.
- [ ] **Step 2: Headless gate** (here): `.\ArcaneTests.exe "~[gpu]"` at/above the Task-1 count; `.\ArcaneTests.exe "[pick]"` + `.\ArcaneTests.exe "[grimoire]"` green. Record counts.
- [ ] **Step 3: Desk interactive** (Grimoire, at the desk — GPU hazard headless): select an entity → **amber** outline traces its silhouette and persists without hovering; move the cursor over another entity → **cyan** outline follows and updates as the mouse moves; hover the selected entity → amber only (no cyan); one entity in front of another → only the front (visible) silhouette outlines (occlusion); **no** outline in Play mode; hovering a gizmo handle shows no spurious hover outline; run a while + trigger a shader hot-reload (F5) → no NVRHI/VK-validation noise, outline still correct; editor panels/dockspace unaffected.
- [ ] **Step 4:** Append a completion note to `.superpowers/sdd/progress.md`.

---

## Self-Review

**Spec coverage:** two separate systems (picking unchanged + new outline) → Tasks 1-3; `SelectionOutline` ARCANE_API facade (own shader/pipeline, id buffer in, framebuffer out) → Task 2; hovered id derived in-shader (no readback) → Task 2 shader + Task 3 `cursorPx`; select-over-hover precedence + exterior outline → Task 2 shader; display-referred amber/cyan colors → Task 2 `Params`/shader; `PassIdOf` shared-buffer seam → Task 1; per-frame Edit-only into `OutputFramebuffer` + skip-when-nothing gate → Task 3; occlusion via depth-tested id pass → Task 2 Step 0 (verification) + Task 4 desk; `[gpu]` render test + headless `PassIdOf` test → Tasks 1-2; desk-verify → Task 4; non-goals (multi-select, alpha silhouettes, JFA, interior/tint, Play-mode) untouched. Covered.

**Placeholder scan:** the `/*...*/`-style read-first adaptations are Task 2 Step 3 (`Init`/`Render` nvrhi bodies, against `TonemapPass`) and the Task 2 Step 4 GPU-harness/upload/readback (against `GpuTestHelpers`) — enumerated adaptations against named files with the intended shape shown, matching this repo's prior GPU plans (picking, imgui). The `return true;`/empty-body placeholders in the Step 3 skeleton are explicitly called out to be replaced with the real nvrhi calls. No "TBD"/"add error handling"/uncoded logic steps.

**Type consistency:** `PickPassId(const std::vector<Astra::Entity>&, Astra::Entity) -> uint32_t` / `PickBuffer::PassIdOf(Astra::Entity) -> uint32_t`; `SelectionOutline::Create(nvrhi::IDevice*, ShaderLibrary&)` / `Render(nvrhi::ICommandList*, nvrhi::ITexture*, nvrhi::IFramebuffer*, const Params&)` / `Params{ selectedId:uint32_t, cursorPx:glm::ivec2, selectColor:glm::vec4, hoverColor:glm::vec4, selectThicknessPx:uint32_t, hoverThicknessPx:uint32_t }`; `OutlineCB` byte-matches the HLSL `cbuffer`; `PickView{offset, worldToScreenScale}` and `PickBuffer::RenderIdPass`/`IdTarget` as built. Consistent across tasks and matching the spec.
