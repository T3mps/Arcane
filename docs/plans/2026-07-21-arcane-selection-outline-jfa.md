# Selection + Hover Outline v2 (Jump-Flood distance field) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the shipped single-pass brute-force outline with an AAA Jump-Flood-Algorithm (JFA) distance-field pipeline so the selected (amber) / hovered (cyan) outline is round, anti-aliased, and cheap at any thickness.

**Architecture:** The id pass renders at 2x supersample so silhouettes carry sub-pixel coverage. `Arcane::SelectionOutline` becomes a stateful three-pass GPU pipeline that owns its render targets: **seed** (2x id -> per-1x-pixel coverage + sub-pixel silhouette position + select/hover tag), **JFA** (~log2 ping-pong passes -> unsigned distance field), **composite** (`smoothstep` on distance -> two-color exterior outline, display-referred). Hovered id is derived in-shader from the cursor (no CPU readback); occlusion is inherited from the depth-tested id pass.

**Tech Stack:** C++23, NVRHI (D3D12 + Vulkan), HLSL (DXC -> DXIL+SPIR-V), Astra ECS, glm, Catch2. Spec: `docs/superpowers/specs/2026-07-21-arcane-selection-outline-jfa-design.md`. The v1 pass (`SelectionOutline.{hpp,cpp}` @1daa2aea, `selection_outline.hlsl`) is the per-pass NVRHI template — read it; every pass mirrors its pipeline/binding/CB/draw idiom.

## Global Constraints

- **`ARCANE_API`, editor-free, game-agnostic:** `SelectionOutline` knows nothing about Grimoire or any game — it consumes a (supersampled) id buffer + ids/colors/cursor + a target framebuffer, owns its internal targets, and produces outlines. Nothing else.
- **No CPU readback for hover:** `hoveredId` is `gIds.Load(cursor*ss)` IN THE SEED SHADER. Never read the id buffer on the CPU per-frame.
- **Colors are display-referred:** written directly into the gamma-encoded `BGRA8` target (like TonemapPass / the game-imgui overlay). NO linear->sRGB conversion in any outline shader.
- **Each per-pass C++ constant-buffer struct is BYTE-IDENTICAL to its HLSL `cbuffer`** — guard every one with `static_assert(sizeof(...) == N)` (and an `offsetof` where a `float4` follows scalars), exactly as v1's `OutlineCB` does.
- **Thickness is `float`** (distance-ramp half-width); `edgeSoftnessPx` is the `smoothstep` width. NOT the v1 `uint32_t`.
- **/MD everywhere; no `/fp:fast`; UTF-8 without BOM; ASCII-only comments.**
- **Shaders are data:** HLSL in `Arcane/shaders/`, compiled by the Arcane prebuild (DXC -> DXIL+SPIR-V). Register EACH new `.hlsl` in `compile-shaders.bat` (explicit list, not globbed). SPIR-V register shifts match `nvrhi::VulkanBindingOffsets` (t=0 s=128 b=256 u=384); plain `register(b0)`/`register(t0)` (real constant buffers, not push constants — no `#if SPIRV` guard, per v1). Entry points `vs_main`/`ps_main`. `Texture2D<uint>` read with `.Load` (integer coords), never `.Sample`; RGBA16 seed targets also read with `.Load`.
- **Build (use the PowerShell tool, NOT the Bash tool — Git Bash mangles `/p:` `/m`):**
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /t:<Target> /p:Configuration=Debug /m /nologo /v:minimal`
  New source/shader files -> run `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` ONCE first (it also makes the prebuild compile the new shaders). Targets: `ArcaneTests`, `Grimoire`.
- **`[gpu]` tests are DESK/CI-ONLY** (Parsec GPU-driver crash hazard headless). BUILD-VERIFY them (compile + link); never RUN a bare `[gpu]` filter and never launch `Grimoire.exe` headless. Their runtime pass is desk/CI (Task 5). `[gpu]` tests assert `Arcane::RenderErrorCount() == 0`.
- **Run headless tests** from the exe dir: `cd "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"` then `.\ArcaneTests.exe "<filter>"`.
- **Clang/clangd diagnostics in this workspace are NOISE** (wrong toolchain — "Api.hpp not found", "glm undeclared", "expected Clang 20", "catch2 not found", and any `static_assert sizeof` failure clang reports because it cannot resolve glm). The MSVC build is the SOLE source of truth.
- **Commits:** single-line `type(scope): summary`, NO body, NO AI trailers (no Co-Authored-By / Claude-Session / "Generated with").
- **Baselines (must not drop):** current HEAD (v1 shipped) `~[gpu]` **27868/346**, `[grimoire]` **97/13**. Tasks 1-2 add headless `[pick]`/`[outline]` assertions (grow `~[gpu]`); Tasks 2-3 add `[gpu]` cases (NOT in `~[gpu]`). Record exact new counts as you go.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Render/PickBuffer.hpp` | Modify | Add a supersample factor to `Create`/`Resize`; declare `Supersample()`. |
| `Arcane/Arcane/src/Arcane/Render/PickBuffer.cpp` | Modify | Render the id target at `ss*w x ss*h`; `Pick()` reads the center subsample; pure `PickSampleTexel` helper. |
| `Arcane/Arcane/src/Arcane/Render/PickEmit.hpp` | Modify | Declare pure `glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH)`. |
| `Arcane/Arcane/src/Arcane/Render/PickEmit.cpp` | Modify | Implement `PickSampleTexel`. |
| `Arcane/Arcane/src/Arcane/Render/SelectionOutline.hpp` | Rewrite | v2 stateful facade: `Create(device, shaders, w, h)`, `Render`, `Resize`, `DebugDistanceField`, new `Params`. |
| `Arcane/Arcane/src/Arcane/Render/SelectionOutline.cpp` | Rewrite | 3 pipelines + owned seed0/pingA/pingB targets; seed+JFA (Task 2), composite+Render tie (Task 3). |
| `Arcane/shaders/outline_seed.hlsl` | Create | 2x id -> RGBA16 seed (coverage + sub-pixel pos + tag). |
| `Arcane/shaders/outline_jfa.hlsl` | Create | One jump-flood step (ping-pong). |
| `Arcane/shaders/outline_composite.hlsl` | Create | Distance -> `smoothstep` two-color exterior outline. |
| `Arcane/shaders/selection_outline.hlsl` | Delete | v1 single-pass shader, superseded. |
| `Arcane/Tests/src/SelectionOutlineTest.cpp` | Modify | Add `[outline]` pass-count test + `[gpu]` field + full-outline tests; drop the v1 `[gpu]` case. |
| `Arcane/Grimoire/src/GrimoireApp.cpp` | Modify | Create `PickBuffer` at ss=2 + `SelectionOutline` with a size; `Resize` both; per-frame `Render`. |

---

## Task 1: PickBuffer 2x supersample (+ pure `PickSampleTexel`)

Give the id pass a supersample factor so the highlighted silhouettes carry sub-pixel coverage. Picking reads the center subsample. The coordinate map is a pure function unit-tested headless; the render change is build-verified (its runtime lives in the desk/CI `[gpu]` pick test).

**Files:** Modify `PickBuffer.{hpp,cpp}`, `PickEmit.{hpp,cpp}`; test `Arcane/Tests/src/SelectionOutlineTest.cpp`.

**Interfaces:**
- Produces: `PickBuffer::Create(device, shaders, w, h, uint32_t supersample = 1)`; `PickBuffer::Resize(w, h)` keeps the supersample set at Create; `uint32_t PickBuffer::Supersample() const`. `IdTarget()` is now `supersample*w x supersample*h`. Free `glm::ivec2 Arcane::PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH)` (clamped center-subsample texel).

- [ ] **Step 0 (read-first):** In `PickBuffer.cpp`: how `Create`/`Resize` size the `R32_UINT` id target; how `RenderIdPass` sets the viewport and how the id **vertex shader** maps world->NDC (confirm it is resolution-independent — NDC from the `PickView` scale/offset, NOT from physical target dims — so a larger target + larger viewport supersamples the SAME logical content with NO vertex-shader change; if it bakes physical dims, pass logical 1x dims in the id CB instead). How `Pick()` converts the click `pixel` to the readback texel (this is where `ss` scaling goes). Confirm `PickView`, `Pick`, the id CB layout, and that `entity_id.hlsl` needs no change. In `PickEmit.{hpp,cpp}`: the `#include`/namespace conventions and where free helpers like `PickPassId` live.

- [ ] **Step 1: Write the failing test** — append to `Arcane/Tests/src/SelectionOutlineTest.cpp`:

```cpp
#include <Arcane/Render/PickEmit.hpp>
#include <glm/vec2.hpp>

TEST_CASE("PickSampleTexel maps a 1x click to the center subsample, clamped", "[pick]")
{
    // ss=2, id buffer 128x128 (1x 64x64). Click at 1x pixel (10,20) -> 2x texel (21,41).
    CHECK(Arcane::PickSampleTexel(glm::vec2(10.4f, 20.9f), 2u, 128u, 128u) == glm::ivec2(21, 41));
    // ss=1 is identity (floored), clamped to bounds.
    CHECK(Arcane::PickSampleTexel(glm::vec2(3.7f, 4.2f), 1u, 64u, 64u) == glm::ivec2(3, 4));
    // out-of-range clamps into the buffer.
    CHECK(Arcane::PickSampleTexel(glm::vec2(999.0f, -5.0f), 2u, 128u, 128u) == glm::ivec2(127, 0));
}
```

(`floor(10.4)*2 + 2/2 = 20+1 = 21`; `floor(20.9)*2 + 1 = 41`. Adapt the include path to Step 0.)

- [ ] **Step 2: Run headless, verify fail.** `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` (new symbol) is not needed (no new file), but the test references an undeclared function — build `ArcaneTests`, then `.\ArcaneTests.exe "[pick]"` -> FAIL (unresolved `PickSampleTexel`).

- [ ] **Step 3: Implement `PickSampleTexel`** — declare in `PickEmit.hpp` near `PickPassId`:

```cpp
    // The id-buffer texel to sample for a 1x viewport click at `pixel1x` when the
    // id buffer is supersampled by `ss` (center subsample), clamped to [0, dim).
    glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH);
```

Implement in `PickEmit.cpp`:

```cpp
    glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH)
    {
        const int s = (int)ss;
        int x = (int)std::floor(pixel1x.x) * s + s / 2;
        int y = (int)std::floor(pixel1x.y) * s + s / 2;
        x = std::clamp(x, 0, (int)idW - 1);
        y = std::clamp(y, 0, (int)idH - 1);
        return glm::ivec2(x, y);
    }
```

(Add `<algorithm>`/`<cmath>` if not already included.)

- [ ] **Step 4: Wire the supersample into `PickBuffer`.** In `PickBuffer.hpp`: add `uint32_t supersample = 1` as the last `Create` param; add `virtual uint32_t Supersample() const = 0;`. In `PickBuffer.cpp` (adapt to Step 0):
  - Store `m_ss`; size the id target `m_ss*width x m_ss*height` in `Create`/`Resize` (the 1x1 staging texture is unchanged — but confirm the readback copies from the supersampled target at the right texel).
  - `RenderIdPass`: set the viewport to the full supersampled target size; leave the resolution-independent NDC mapping as-is (Step 0) so content supersamples.
  - `Pick()`: replace the click->texel conversion with `PickSampleTexel(pixel, m_ss, IdTarget()->getDesc().width, ...->height)` for the readback source texel.
  - `Width()/Height()` semantics: KEEP returning the LOGICAL 1x size (callers reason in 1x); add `Supersample()` for the factor. Confirm every internal user of `Width()/Height()` still wants 1x (Resize round-trips through them).

- [ ] **Step 5: Build + verify.** Build `ArcaneTests`. `.\ArcaneTests.exe "[pick]"` -> PASS. `.\ArcaneTests.exe "~[gpu]"` -> 27868 + the 3 new `[pick]` assertions (record the exact count); no other change. The `[gpu]` entity-pick test is build-verified (compiles/links); its runtime pass (picking still correct at 2x) is desk/CI.

- [ ] **Step 6: Commit** — `feat(arcane): PickBuffer 2x supersample for sub-pixel outline coverage`.

---

## Task 2: SelectionOutline v2 — seed + JFA distance field (+ shaders, `[gpu]` field test)

Rewrite `SelectionOutline` into a stateful pass that owns its targets and produces a distance field from the supersampled id buffer. The composite (Task 3) consumes the field. GPU-touching -> build-verified here; `[gpu]` test runs desk/CI.

**Files:** Rewrite `SelectionOutline.{hpp,cpp}`; create `Arcane/shaders/outline_seed.hlsl`, `outline_jfa.hlsl`; delete `selection_outline.hlsl`; extend `SelectionOutlineTest.cpp`.

**Interfaces:**
- Consumes: `Arcane::ShaderLibrary`, `nvrhi`, `PickBuffer::IdTarget()` (a supersampled `R32_UINT` texture). The v1 `SelectionOutline.cpp` @1daa2aea is the pipeline/binding/CB/draw template.
- Produces: `SelectionOutline::Create(nvrhi::IDevice*, ShaderLibrary&, uint32_t w, uint32_t h)`; `void Render(cmd, idBuffer, target, const Params&)` (Task 2: runs seed+JFA into the internal field; composite into `target` is Task 3); `void Resize(w, h)`; `nvrhi::ITexture* DebugDistanceField() const` (final JFA target — debug-viz + test seam, mirroring `PickBuffer::IdTarget`); nested `Params{ selectedId, cursorPx, selectColor, hoverColor, selectThicknessPx(float), hoverThicknessPx(float), edgeSoftnessPx(float) }`; free `uint32_t Arcane::JfaPassCount(uint32_t maxThicknessPx)`.

- [ ] **Step 0 (read-first):** Read v1 `SelectionOutline.cpp` @1daa2aea end-to-end — the `Init` (load VS+PS by name per backend, non-volatile CB via `setIsConstantBuffer`+`setInitialState(ConstantBuffer)`+`setKeepInitialState`, binding layout, fullscreen pipeline, blend), `Render` (writeBuffer -> binding set {SRV, CB} -> setGraphicsState(pipeline, framebuffer, full viewport) -> draw 3), the pipeline cache keyed on `FramebufferInfo`, the binding-set cache keyed on the input texture pointer. Read `TonemapPass.cpp` for how it CREATES an owned render target + framebuffer (seed0/pingA/pingB need this — v1 owned none). Read `PickBuffer.cpp`/`OffscreenCanvas` `Resize` for the tear-down/rebuild idiom. Read `GpuTestHelpers.hpp` for creating an `R32_UINT` texture + uploading known texels (`writeTexture`) and reading back a texture. Confirm `compile-shaders.bat` is an explicit list; the `[gpu]`/`[outline]` tags + `RenderErrorCount()`.

- [ ] **Step 1: Write the failing headless test** — `JfaPassCount`, append to `SelectionOutlineTest.cpp`:

```cpp
#include <Arcane/Render/SelectionOutline.hpp>

TEST_CASE("JfaPassCount = ceil(log2(maxThickness)) + 1", "[outline]")
{
    CHECK(Arcane::JfaPassCount(1)  == 1u);
    CHECK(Arcane::JfaPassCount(3)  == 3u);   // ceil(log2 3)=2, +1
    CHECK(Arcane::JfaPassCount(16) == 5u);   // ceil(log2 16)=4, +1
    CHECK(Arcane::JfaPassCount(32) == 6u);
}
```

- [ ] **Step 2: Run headless, verify fail.** After adding the file/symbol, `& GenerateProjects.bat` (new shader files), build `ArcaneTests`, `.\ArcaneTests.exe "[outline]"` -> FAIL (unresolved `JfaPassCount`).

- [ ] **Step 3: Write `Arcane/shaders/outline_seed.hlsl`:**

```hlsl
// Pass 1 of the JFA selection outline: read the SUPERSAMPLED (ss x) R32_UINT id
// buffer; compute per-1x-pixel coverage of the selected + hovered silhouettes and
// write an RGBA16_SNORM seed { sub-pixel silhouette pos (normalized), tag, coverage }
// at 1x (composite) resolution. hoveredId is the id at the cursor texel, in-shader.
// Design: docs/superpowers/specs/2026-07-21-arcane-selection-outline-jfa-design.md.

cbuffer SeedCB : register(b0)
{
    uint  gSelectedId;   // 0 = no selection
    int2  gCursorPx;     // 1x viewport px; x<0 => no hover
    uint  gSuperSample;  // id-buffer supersample factor (e.g. 2)
    int2  gDim;          // 1x (composite) dimensions
    uint2 _pad;
};

Texture2D<uint> gIds : register(t0);   // supersampled id buffer (ss*gDim)

struct VSOutput { float4 pos : SV_Position; };

VSOutput vs_main(uint vid : SV_VertexID)
{
    VSOutput o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// .xy = normalized 1x position [-1,1], .z = tag (+1 select, -1 hover),
// .w = coverage (0..1; 0 => empty/background).
float4 ps_main(VSOutput i) : SV_Target0
{
    int2 p  = int2(i.pos.xy);
    int  ss = (int)gSuperSample;

    uint hoveredId = 0u;
    if (gCursorPx.x >= 0 && gCursorPx.y >= 0)
    {
        int2 c = gCursorPx * ss + (ss / 2);
        hoveredId = gIds.Load(int3(c, 0));
        if (hoveredId == gSelectedId) hoveredId = 0u;   // hovering the selection => amber only
    }

    int    nSel = 0, nHov = 0;
    float2 sumSel = float2(0, 0), sumHov = float2(0, 0);
    int2   base = p * ss;
    [loop] for (int sy = 0; sy < ss; ++sy)
    [loop] for (int sx = 0; sx < ss; ++sx)
    {
        uint   id  = gIds.Load(int3(base + int2(sx, sy), 0));
        float2 sub = (float2(base + int2(sx, sy)) + 0.5) / (float)ss;   // 1x-space subsample center
        if (gSelectedId != 0u && id == gSelectedId)      { nSel++; sumSel += sub; }
        else if (hoveredId != 0u && id == hoveredId)     { nHov++; sumHov += sub; }
    }

    int   total  = ss * ss;
    float covSel = (float)nSel / (float)total;
    float covHov = (float)nHov / (float)total;

    float  tag, cov;
    float2 centroid;
    if (nSel > 0 && covSel >= covHov) { tag =  1.0; cov = covSel; centroid = sumSel / (float)nSel; }
    else if (nHov > 0)                { tag = -1.0; cov = covHov; centroid = sumHov / (float)nHov; }
    else return float4(0, 0, 0, 0);   // background: empty seed

    float2 nrm = (centroid / float2(gDim)) * 2.0 - 1.0;
    return float4(nrm, tag, cov);
}
```

- [ ] **Step 4: Write `Arcane/shaders/outline_jfa.hlsl`:**

```hlsl
// Pass 2 (run N times, halving gJump from 2^(N-1) down to 1): one jump-flood step.
// For each 1x pixel, among itself + 8 neighbors at +-gJump, keep the seed whose
// stored silhouette position is nearest this pixel. RGBA16_SNORM in/out (ping-pong).

cbuffer JfaCB : register(b0)
{
    int  gJump;   // jump distance (1x px)
    int2 gDim;    // 1x dimensions
    int  _pad;
};

Texture2D<float4> gSeed : register(t0);

struct VSOutput { float4 pos : SV_Position; };

VSOutput vs_main(uint vid : SV_VertexID)
{
    VSOutput o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VSOutput i) : SV_Target0
{
    int2   p  = int2(i.pos.xy);
    float2 pf = (float2)p + 0.5;

    float4 best  = float4(0, 0, 0, 0);   // .w = coverage; 0 => empty
    float  bestD = 1e20;

    [unroll] for (int oy = -1; oy <= 1; ++oy)
    [unroll] for (int ox = -1; ox <= 1; ++ox)
    {
        int2 q = p + int2(ox, oy) * gJump;
        if (q.x < 0 || q.y < 0 || q.x >= gDim.x || q.y >= gDim.y) continue;
        float4 s = gSeed.Load(int3(q, 0));
        if (s.w <= 0.0) continue;
        float2 sp = (s.xy * 0.5 + 0.5) * float2(gDim);
        float2 dv = sp - pf;
        float  d  = dot(dv, dv);
        if (d < bestD) { bestD = d; best = s; }
    }
    return best;
}
```

- [ ] **Step 5: Write `SelectionOutline.hpp`** (v2 facade):

```cpp
#pragma once

// A per-frame GPU Jump-Flood distance-field outline: seed (from a SUPERSAMPLED
// R32_UINT id buffer) -> JFA -> composite two-color EXTERIOR outlines (selected +
// hovered) into a display-referred target. Owns its render targets. Editor- and
// game-agnostic. Design: docs/superpowers/specs/2026-07-21-arcane-selection-outline-jfa-design.md.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <memory>

namespace Arcane
{
    class ShaderLibrary;

    // Number of jump-flood passes to resolve distances up to `maxThicknessPx`:
    // ceil(log2(maxThicknessPx)) + 1 (jumps 2^(N-1) .. 1).
    ARCANE_API uint32_t JfaPassCount(uint32_t maxThicknessPx);

    class ARCANE_API SelectionOutline
    {
    public:
        static std::unique_ptr<SelectionOutline> Create(nvrhi::IDevice*, ShaderLibrary&,
                                                        uint32_t width, uint32_t height);
        virtual ~SelectionOutline() = default;

        struct Params
        {
            uint32_t   selectedId = 0;                              // 0 = no selection
            glm::ivec2 cursorPx   = { -1, -1 };                     // viewport-local; <0 => no hover
            glm::vec4  selectColor = { 1.0f, 0.65f, 0.10f, 1.0f };  // amber (display-referred)
            glm::vec4  hoverColor  = { 0.25f, 0.70f, 1.0f, 1.0f };  // cyan  (display-referred)
            float      selectThicknessPx = 3.0f;                    // outline half-width
            float      hoverThicknessPx  = 2.0f;
            float      edgeSoftnessPx    = 1.0f;                    // AA ramp width
        };

        // Seed -> JFA -> composite. `idBuffer` is the SUPERSAMPLED R32_UINT id target;
        // the supersample factor is derived from idBuffer size / this pass's size.
        // hoveredId is sampled in-shader from Params::cursorPx (no readback). Owns its
        // ping-pong targets at Create/Resize size. Select takes precedence over hover.
        virtual void Render(nvrhi::ICommandList*,
                            nvrhi::ITexture* idBuffer,
                            nvrhi::IFramebuffer* target,
                            const Params&) = 0;

        virtual void Resize(uint32_t width, uint32_t height) = 0;

        // The final JFA distance-field target after the last Render (nearest-seed
        // RGBA16). For debug visualization + tests; not needed by consumers.
        virtual nvrhi::ITexture* DebugDistanceField() const = 0;
    };
}
```

- [ ] **Step 6: Write `SelectionOutline.cpp`** — seed + JFA (composite is Task 3). MIRROR v1 `SelectionOutline.cpp` for EACH pipeline; adapt every nvrhi call to the real idiom from Step 0. Keep each CB byte-identical to its HLSL `cbuffer`:

```cpp
#include <Arcane/Render/SelectionOutline.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <cmath>
#include <cstdint>

namespace Arcane
{
    uint32_t JfaPassCount(uint32_t maxThicknessPx)
    {
        if (maxThicknessPx <= 1u) return 1u;
        uint32_t n = 0; uint32_t v = maxThicknessPx - 1u;
        while (v) { v >>= 1; ++n; }          // n = floor(log2(maxThicknessPx-1))+1 = ceil(log2(maxThicknessPx))
        return n + 1u;
    }

    namespace
    {
        constexpr uint32_t kMaxThicknessPx = 32;   // sizes the JFA pass count (jumps 32..1)

        struct SeedCB { uint32_t selectedId; int32_t cursorX, cursorY; uint32_t superSample; int32_t dimX, dimY; uint32_t pad0, pad1; };
        static_assert(sizeof(SeedCB) == 32, "SeedCB must match outline_seed.hlsl SeedCB");

        struct JfaCB { int32_t jump; int32_t dimX, dimY; int32_t pad; };
        static_assert(sizeof(JfaCB) == 16, "JfaCB must match outline_jfa.hlsl JfaCB");

        class SelectionOutlineImpl final : public SelectionOutline
        {
        public:
            bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders, uint32_t w, uint32_t h)
            {
                m_device = device;
                // Load "outline_seed" + "outline_jfa" VS+PS (per backend). Create the
                // SeedCB + JfaCB constant buffers, two binding layouts (seed: t0 id +
                // b0 SeedCB; jfa: t0 seed + b0 JfaCB), and two fullscreen-triangle
                // pipelines (no depth; seed writes RGBA16_SNORM opaque; jfa writes
                // RGBA16_SNORM opaque). MIRROR v1 Init. Then BuildTargets(w,h).
                // ARC_ERROR + return false on any failure.
                return BuildTargets(w, h);   // + the pipeline/CB creation above
            }

            void Render(nvrhi::ICommandList* cmd, nvrhi::ITexture* idBuffer,
                        nvrhi::IFramebuffer* /*target*/, const Params& p) override
            {
                const int ss = (int)(idBuffer->getDesc().width / m_width);

                // --- Seed pass: id -> seed0 ---
                SeedCB sc{};
                sc.selectedId = p.selectedId;
                sc.cursorX = p.cursorPx.x; sc.cursorY = p.cursorPx.y;
                sc.superSample = (uint32_t)ss;
                sc.dimX = (int)m_width; sc.dimY = (int)m_height;
                cmd->writeBuffer(m_seedCb, &sc, sizeof(sc));
                // bind {idBuffer SRV, m_seedCb}; setGraphicsState(seed pipeline, m_seed0Fb,
                // full viewport); draw 3.  MIRROR v1 Render.

                // --- JFA ping-pong: seed0 -> ... -> field ---
                const uint32_t passes = JfaPassCount(kMaxThicknessPx);
                nvrhi::ITexture* src = m_seed0;                 // first read source
                nvrhi::IFramebuffer* dstFb = m_pingFb[0];       // first write
                nvrhi::ITexture* dst = m_ping[0];
                for (uint32_t i = 0; i < passes; ++i)
                {
                    JfaCB jc{}; jc.jump = 1 << (passes - 1 - i); jc.dimX = (int)m_width; jc.dimY = (int)m_height;
                    cmd->writeBuffer(m_jfaCb, &jc, sizeof(jc));
                    // bind {src SRV, m_jfaCb}; setGraphicsState(jfa pipeline, dstFb, full viewport); draw 3.
                    src = dst;
                    const uint32_t nextIdx = (i + 1) & 1u;      // ping <-> pong
                    dst = m_ping[nextIdx]; dstFb = m_pingFb[nextIdx];
                }
                m_field = src;   // the last-written target holds the distance field
            }

            void Resize(uint32_t w, uint32_t h) override { if (w && h && (w != m_width || h != m_height)) BuildTargets(w, h); }
            nvrhi::ITexture* DebugDistanceField() const override { return m_field; }

        private:
            bool BuildTargets(uint32_t w, uint32_t h)
            {
                m_width = w; m_height = h;
                // Create m_seed0 + m_ping[0] + m_ping[1] as RGBA16_SNORM render targets
                // (w x h) with keepInitialState, and a framebuffer for each
                // (m_seed0Fb, m_pingFb[0], m_pingFb[1]). MIRROR TonemapPass's owned-RT
                // creation. Invalidate any binding-set cache. ARC_ERROR + return false
                // on failure. m_field = m_seed0 initially.
                return true;   // replace with the real result
            }

            nvrhi::IDevice*        m_device = nullptr;
            uint32_t               m_width = 0, m_height = 0;
            nvrhi::BufferHandle    m_seedCb, m_jfaCb;
            nvrhi::TextureHandle   m_seed0, m_ping[2];
            nvrhi::FramebufferHandle m_seed0Fb, m_pingFb[2];
            nvrhi::ITexture*       m_field = nullptr;
            // + seed/jfa pipelines, binding layouts, shader handles, binding-set caches.
        };
    }

    std::unique_ptr<SelectionOutline> SelectionOutline::Create(nvrhi::IDevice* device, ShaderLibrary& shaders,
                                                              uint32_t w, uint32_t h)
    {
        auto pass = std::make_unique<SelectionOutlineImpl>();
        if (!pass->Init(device, shaders, w, h)) return nullptr;
        return pass;
    }
}
```

(Replace the `BuildTargets`/`Init`/`Render` comment blocks + the `return true` with the real nvrhi calls, adapted 1:1 from v1 `SelectionOutline.cpp` + `TonemapPass.cpp`. Keep `SeedCB`/`JfaCB` byte-identical to the HLSL. The ping-pong `m_field` bookkeeping above is the load-bearing logic — implement it exactly.)

- [ ] **Step 7: Register the shaders.** Add `outline_seed` and `outline_jfa` to `compile-shaders.bat` (the explicit list, mirroring how `selection_outline` was added). Delete the `selection_outline` entry + `Arcane/shaders/selection_outline.hlsl` (superseded — its only consumer, v1 `SelectionOutline.cpp`, is being rewritten).

- [ ] **Step 8: Write the `[gpu]` distance-field test** — extend `SelectionOutlineTest.cpp` (replace the v1 `[gpu]` case):

```cpp
TEST_CASE("SelectionOutline JFA builds a nearest-seed distance field", "[gpu][selection]")
{
    // device + ShaderLibrary per Step 0. 64x64 pass; 128x128 (ss=2) R32_UINT id.
    auto outline = Arcane::SelectionOutline::Create(device, shaders, 64, 64);
    REQUIRE(outline != nullptr);

    // Upload a 128x128 id buffer: a single filled rect [16..48)^2 = id 5, else 0.
    // (ss=2 => that rect is 1x pixels [8..24)^2.)
    Arcane::SelectionOutline::Params p; p.selectedId = 5; p.cursorPx = glm::ivec2(-1, -1);
    cmd->open(); outline->Render(cmd, idTex, dummyFb, p); cmd->close();
    device->executeCommandList(cmd); device->waitForIdle();

    // Read back DebugDistanceField(): every non-empty texel's stored seed (.w>0) must
    // point at a position INSIDE the 1x silhouette [8..24)^2; a background pixel far
    // outside (e.g. (60,60)) resolves to the nearest silhouette edge (~x=23 or y=23).
    // Assert a couple of known pixels + RenderErrorCount()==0.
    CHECK(Arcane::RenderErrorCount() == 0);
}
```

(Fill the harness/upload/readback from `GpuTestHelpers` per Step 0. `[gpu]` — build-verify only; runs desk/CI.)

- [ ] **Step 9: Regenerate + build, verify clean.** `& GenerateProjects.bat`; build `ArcaneTests` -> zero errors (prebuild compiles both new shaders to DXIL+SPIR-V). `.\ArcaneTests.exe "[outline]"` -> PASS; `.\ArcaneTests.exe "~[gpu]"` -> Task-1 count + the 4 new `[outline]` assertions (record it); the `[gpu][selection]` case excluded.

- [ ] **Step 10: Commit** — `feat(arcane): SelectionOutline v2 -- JFA seed + flood distance field`.

---

## Task 3: SelectionOutline v2 — composite + Render tie (+ full `[gpu]` outline test)

Add the third pass and finish `Render` so the distance field becomes the anti-aliased two-color outline in the target.

**Files:** Modify `SelectionOutline.cpp`; create `Arcane/shaders/outline_composite.hlsl`; extend `SelectionOutlineTest.cpp`.

**Interfaces:**
- Consumes: Task 2's `m_field` (final JFA target) + `m_seed0` (original coverage); the v1 blend/draw idiom.
- Produces: the completed `SelectionOutline::Render` (composites into `target`).

- [ ] **Step 0 (read-first):** Re-read v1 `SelectionOutline.cpp` Render's blend state (`SrcAlpha/InvSrcAlpha`) + how it targets an external `IFramebuffer`. Confirm a pass can bind TWO SRVs (t0 field, t1 seed0) in one binding layout/set.

- [ ] **Step 1: Write `Arcane/shaders/outline_composite.hlsl`:**

```hlsl
// Pass 3: distance-field -> anti-aliased two-color EXTERIOR outline. Reads the final
// JFA target (nearest silhouette seed) + the original seed0 (this pixel's own
// coverage, for the exterior test), blends amber/cyan (display-referred) over the
// target. No CPU readback; no sRGB conversion.

cbuffer CompositeCB : register(b0)
{
    float  gSelectThick;   // outline half-width (px)
    float  gHoverThick;
    float  gEdgeSoft;      // AA ramp width (px)
    float  _pad0;
    int2   gDim;
    int2   _pad1;
    float4 gSelectColor;   // display-referred (amber)
    float4 gHoverColor;    // display-referred (cyan)
};

Texture2D<float4> gField : register(t0);
Texture2D<float4> gSeed0 : register(t1);

struct VSOutput { float4 pos : SV_Position; };

VSOutput vs_main(uint vid : SV_VertexID)
{
    VSOutput o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VSOutput i) : SV_Target0
{
    int2 p = int2(i.pos.xy);

    // Exterior test: skip pixels the silhouette already (mostly) covers.
    if (gSeed0.Load(int3(p, 0)).w > 0.5) discard;

    float4 s = gField.Load(int3(p, 0));
    if (s.w <= 0.0) discard;                       // nothing selected/hovered anywhere

    float2 sp    = (s.xy * 0.5 + 0.5) * float2(gDim);
    float  d     = distance((float2)p + 0.5, sp);
    float  thick = (s.z >= 0.0) ? gSelectThick : gHoverThick;
    float  alpha = 1.0 - smoothstep(thick - gEdgeSoft, thick, d);
    if (alpha <= 0.0) discard;

    float4 col = (s.z >= 0.0) ? gSelectColor : gHoverColor;
    return float4(col.rgb, col.a * alpha);
}
```

- [ ] **Step 2: Register the shader.** Add `outline_composite` to `compile-shaders.bat`.

- [ ] **Step 3: Add the composite pipeline + CB to `SelectionOutline.cpp`.** Add:

```cpp
        struct CompositeCB {
            float selectThick, hoverThick, edgeSoft, pad0;
            int32_t dimX, dimY, pad1a, pad1b;
            glm::vec4 selectColor, hoverColor;
        };
        static_assert(sizeof(CompositeCB) == 64, "CompositeCB must match outline_composite.hlsl");
        static_assert(offsetof(CompositeCB, selectColor) == 32, "selectColor at offset 32");
```

In `Init`, additionally load `outline_composite` VS+PS, create `m_compositeCb`, a binding layout (t0 field, t1 seed0, b0 CompositeCB), and a fullscreen pipeline with `SrcAlpha/InvSrcAlpha` blend targeting a `BGRA8` framebuffer (mirror v1's blend pipeline).

- [ ] **Step 4: Finish `Render`** — after the JFA loop sets `m_field`, append the composite:

```cpp
                CompositeCB cc{};
                cc.selectThick = p.selectThicknessPx;
                cc.hoverThick  = p.hoverThicknessPx;
                cc.edgeSoft    = p.edgeSoftnessPx;
                cc.dimX = (int)m_width; cc.dimY = (int)m_height;
                cc.selectColor = p.selectColor;
                cc.hoverColor  = p.hoverColor;
                cmd->writeBuffer(m_compositeCb, &cc, sizeof(cc));
                // bind {m_field SRV (t0), m_seed0 SRV (t1), m_compositeCb (b0)};
                // setGraphicsState(composite pipeline, `target`, full viewport); draw 3.
                // MIRROR v1 Render's external-framebuffer draw + blend.
```

(Change the `Render` signature's `target` back to used — drop the `/*target*/`.)

- [ ] **Step 5: Write the full `[gpu]` outline test** — extend `SelectionOutlineTest.cpp`:

```cpp
TEST_CASE("SelectionOutline draws an anti-aliased amber/cyan exterior outline", "[gpu][selection]")
{
    auto outline = Arcane::SelectionOutline::Create(device, shaders, 64, 64);
    REQUIRE(outline != nullptr);

    // 128x128 (ss=2) id: rect [16..48)^2 = id 5, rect [80..112)^2 = id 7, else 0.
    // 64x64 BGRA8 target cleared to a known background.
    Arcane::SelectionOutline::Params p;
    p.selectedId = 5;
    p.cursorPx   = glm::ivec2(48, 48);   // inside the id=7 1x rect [40..56)^2 -> hovered = 7

    cmd->open(); outline->Render(cmd, idTex, targetFb, p); cmd->close();
    device->executeCommandList(cmd); device->waitForIdle();

    // Read back BGRA8. Assert: an AMBER texel just outside the id=5 rect edge; a CYAN
    // texel just outside the id=7 rect edge; INTERMEDIATE-alpha texels at a ring edge
    // (the anti-aliasing the redesign exists for); a background texel far from both is
    // unchanged; the ring thickness on the id=5 rect is uniform on all four sides
    // (round metric, not square). RenderErrorCount()==0.
    CHECK(Arcane::RenderErrorCount() == 0);
}
```

(Fill from `GpuTestHelpers`. `[gpu]` — build-verify only; runs desk/CI.)

- [ ] **Step 6: Regenerate + build, verify clean.** `& GenerateProjects.bat`; build `ArcaneTests` -> zero errors. `.\ArcaneTests.exe "~[gpu]"` -> unchanged from Task 2 (new case is `[gpu]`); `.\ArcaneTests.exe "[outline]"` still green.

- [ ] **Step 7: Commit** — `feat(arcane): SelectionOutline v2 -- smoothstep composite + Render`.

---

## Task 4: Grimoire integration — drive the JFA outline

Wire the size + supersample through Grimoire.

**Files:** Modify `GrimoireApp.cpp` (and `GrimoireApp.hpp` only if the `m_outline` type/line needs it — the member already exists).

**Interfaces:**
- Consumes: `SelectionOutline::Create(device, shaders, w, h)` + `Resize` (Task 2/3); `PickBuffer::Create(..., supersample)` (Task 1).

- [ ] **Step 0 (read-first):** In `GrimoireApp.cpp` confirm: where `m_pick` is created (`PickBuffer::Create(device, shaders, w, h)`) and where `m_outline` is created (`SelectionOutline::Create(device, shaders)` — v1 had no size); the viewport `Resize` block that already calls `m_pick->Resize(w,h)`; the per-frame outline block (unchanged in shape). Confirm the viewport size source (`m_viewport`/canvas dims) passed to those creates.

- [ ] **Step 1: Create `m_pick` at supersample 2** — pass `2` as the new `Create` arg:

```cpp
        m_pick = Arcane::PickBuffer::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(),
                                            viewportW, viewportH, /*supersample*/ 2);
```

- [ ] **Step 2: Create `m_outline` with the viewport size:**

```cpp
        m_outline = Arcane::SelectionOutline::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(),
                                                     viewportW, viewportH);
        if (!m_outline) { ARC_ERROR("Grimoire: SelectionOutline creation failed"); return false; }
```

- [ ] **Step 3: Resize the outline with the viewport** — beside the existing `m_pick->Resize(...)`:

```cpp
        m_outline->Resize(newW, newH);
```

- [ ] **Step 4: Per-frame block** — unchanged in shape (the `Render(cmd, m_pick->IdTarget(), OutputFramebuffer(), params)` call already matches; `IdTarget()` is now 2x, handled inside `Render`). Confirm `params` uses the `float` thickness fields (defaults are fine). No other change.

- [ ] **Step 5: Build both, verify no regression.** Build `Grimoire;ArcaneTests` -> zero errors. `.\ArcaneTests.exe "~[gpu]"` -> Task-2 count unchanged; `.\ArcaneTests.exe "[grimoire]"` -> 97/13.

- [ ] **Step 6: Commit** — `feat(grimoire): drive the JFA selection outline (2x id + Resize)`.

---

## Task 5: Desk-verify

- [ ] **Step 1: `[gpu]` at the desk/CI** — `.\ArcaneTests.exe "[gpu][selection]"` (or CI GPU lane) -> both `SelectionOutline` cases pass, `RenderErrorCount()==0` on D3D12 AND Vulkan. Vulkan is the watch item (3 real `register(b0)` CBs + `register(t0/t1)` SRVs on the SPIR-V b/t shifts). Record.
- [ ] **Step 2: Headless gates** (here): `.\ArcaneTests.exe "~[gpu]"` at/above the Task-2 count; `.\ArcaneTests.exe "[outline]"` + `.\ArcaneTests.exe "[pick]"` + `.\ArcaneTests.exe "[grimoire]"` green. Record counts.
- [ ] **Step 3: Desk interactive** (Grimoire): select an entity -> a **smooth, round, anti-aliased amber** outline traces its silhouette (no stair-steps; uniform thickness all the way around a circle — the v1 lumpiness is gone); hover another entity -> smooth cyan follows the cursor; hover the selected entity -> amber only; one entity in front of another -> only the front (visible) silhouette outlines; **no** outline in Play; no spurious outline over gizmo handles; drag the viewport border (resize) -> outline stays correct (targets rebuilt); shader hot-reload (F5) -> outline still correct, no NVRHI/VK-validation noise; picking (click-select) still lands on the right entity at 2x.
- [ ] **Step 4:** Append a completion note to `.superpowers/sdd/progress.md`.

---

## Self-Review

**Spec coverage:** JFA seed->flood->composite pipeline -> Tasks 2-3; Approach-1 2x-supersampled id for sub-pixel coverage -> Task 1 (PickBuffer) + Task 2 (seed pass coverage); single tagged JFA for two colors + select-over-hover -> Task 2 seed tag + Task 3 composite; hover derived in-shader (no readback) -> Task 2 seed shader; stateful pass owns seed0 + 2 ping-pong targets + Resize -> Task 2 (`BuildTargets`/`Resize`); thin crisp ~3px default with thickness/softness Params -> Task 2 `Params`; display-referred colors -> Task 3 composite; exterior test via retained seed0 -> Task 3 composite; occlusion via depth-tested 2x id -> Task 1; `DebugDistanceField` debug/test seam -> Task 2; JFA pass count `ceil(log2)+1` -> Task 2 `JfaPassCount` (headless `[outline]`); `[gpu]` field + full-outline tests -> Tasks 2-3; Grimoire drive -> Task 4; desk-verify (incl. Vulkan b0, roundness, resize, occlusion) -> Task 5; non-goals (multi-select, animate/glow, interior, Play, ss>2) untouched. Covered.

**Placeholder scan:** the `Init`/`Render`/`BuildTargets` comment blocks in Task 2 Step 6 are enumerated read-first adaptations against a NAMED committed reference (v1 `SelectionOutline.cpp` @1daa2aea + `TonemapPass.cpp`) with the intended shape and the load-bearing logic (CB structs, ping-pong `m_field` bookkeeping, pass loop) shown in full — the same convention this repo's prior GPU plans (v1 outline, picking, imgui) used and their reviews accepted. The `return true;`/comment placeholders are explicitly flagged to be replaced with the real nvrhi calls. All shaders, the `.hpp`, `JfaPassCount`, `PickSampleTexel`, every CB struct, and all test bodies are complete code. No "TBD"/"add error handling"/uncoded logic.

**Type consistency:** `PickBuffer::Create(device, shaders, w, h, supersample=1)`, `Supersample()->uint32_t`, `PickSampleTexel(glm::vec2, uint32_t, uint32_t, uint32_t)->glm::ivec2`; `JfaPassCount(uint32_t)->uint32_t`; `SelectionOutline::Create(device, shaders, uint32_t, uint32_t)`, `Render(cmd, ITexture*, IFramebuffer*, const Params&)`, `Resize(uint32_t,uint32_t)`, `DebugDistanceField()->ITexture*`; `Params{ selectedId:uint32_t, cursorPx:glm::ivec2, selectColor/hoverColor:glm::vec4, selectThicknessPx/hoverThicknessPx/edgeSoftnessPx:float }`. CB structs byte-match their HLSL: `SeedCB`(32), `JfaCB`(16), `CompositeCB`(64, selectColor@32). Shader seed encoding (`.xy` norm pos, `.z` tag +1/-1, `.w` coverage) is written by `outline_seed` and read identically by `outline_jfa` + `outline_composite`. Consistent across tasks and matching the spec.
