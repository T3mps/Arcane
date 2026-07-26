#pragma once

// Render module: THE 2D submission path. Sprites, rects, lines, circles --
// and, in M2b, text glyphs and ImGui -- all flow through this batcher into
// the same vertex stream and pipeline family. No bespoke draw paths grow
// beside it (homogenized-rendering mandate).
//
// Coordinates are canvas pixels, y down. Colors are LINEAR floats and may
// exceed 1.0 (HDR canvas). Blending is straight (non-premultiplied) alpha
// to match the LOVE client's semantics for 1:1 screen ports; migrating to
// premultiplied is a deliberate future decision.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace Arcane
{
    class ShaderLibrary;
    class MaterialTemplate;
    class MaterialInstance;
    struct GlobalParams;

    // A registered scene sprite material (Slice 8): shaders compiled from the
    // SPRITE template (sprite_material.hlsl register map), the layout/values
    // pair, and the declared texture params already resolved to GPU handles
    // (ordinal order -- they bind at t1..; t0 is the sprite's own texture).
    // The instance is the SAVED asset's values -- scene sprites never render a
    // document's working copy.
    struct Material2DDesc
    {
        nvrhi::ShaderHandle vs;
        nvrhi::ShaderHandle ps;
        std::shared_ptr<const MaterialTemplate> templ;
        std::shared_ptr<const MaterialInstance> instance;
        std::vector<nvrhi::TextureHandle> paramTextures;
    };

    // The 4 corners of a quad, in the order Batcher2D emits them: TL, TR, BR, BL.
    // Given a top-left `pos` + `size`, rotated by `rotation` radians about the
    // quad's CENTER. rotation 0 returns EXACTLY the axis-aligned corners (the
    // byte-identical legacy path). The rotation matches the engine convention
    // R(a)*v = (c*vx - s*vy, s*vx + c*vy) (Transform::ToMatrix /
    // Physics::RotateVec), so a sprite turns in lockstep with its physics body.
    [[nodiscard]] inline std::array<glm::vec2, 4>
    QuadCorners(glm::vec2 pos, glm::vec2 size, float rotation) noexcept
    {
        const glm::vec2 half(size.x * 0.5f, size.y * 0.5f);
        const glm::vec2 center = pos + half;
        if (rotation == 0.0f)
        {
            return { center - half,
                     glm::vec2(center.x + half.x, center.y - half.y),
                     center + half,
                     glm::vec2(center.x - half.x, center.y + half.y) };
        }
        const float c = std::cos(rotation);
        const float s = std::sin(rotation);
        const auto rot = [&](float ox, float oy) {
            return glm::vec2(center.x + c * ox - s * oy,
                             center.y + s * ox + c * oy);
        };
        return { rot(-half.x, -half.y),   // TL
                 rot( half.x, -half.y),   // TR
                 rot( half.x,  half.y),   // BR
                 rot(-half.x,  half.y) }; // BL
    }

    struct Batch2DStats
    {
        uint32_t drawCalls = 0;
        uint32_t quads = 0;  // every primitive is quads under the hood
    };

    class ARCANE_API Batcher2D
    {
    public:
        // Returns null (with ARC_ERROR) when the batch shaders are missing.
        static std::unique_ptr<Batcher2D> Create(nvrhi::IDevice* device,
                                                 ShaderLibrary& shaders);

        virtual ~Batcher2D() = default;

        // The material table (Slice 8): every draw carries a material id; the
        // built-in pipelines are entries 0..2, so the pre-material path is the
        // degenerate case. Registered ids start at 3.
        static constexpr uint16_t kMaterialSprite    = 0;
        static constexpr uint16_t kMaterialCircle    = 1;
        static constexpr uint16_t kMaterialText      = 2;
        static constexpr uint16_t kInvalidMaterialId = 0xFFFF;

        // Register a compiled sprite-surface material; returns its id
        // (kInvalidMaterialId with ARC_WARN on bad inputs / resource failure).
        // UpdateMaterial replaces a slot in place (recompile / asset re-save)
        // and drops that material's cached pipelines + binding sets. Interface
        // DEFAULTS (not pure): geometry-recording test doubles stay valid --
        // they refuse registration and QuadMaterial below degrades to Quad.
        virtual uint16_t RegisterMaterial(Material2DDesc) { return kInvalidMaterialId; }
        virtual bool UpdateMaterial(uint16_t, Material2DDesc) { return false; }

        // Engine-global shader constants (Time/DeltaTime/ViewportSize) for
        // registered materials, uploaded once per End(). Sticky -- the host
        // sets them once per frame before recording. Built-ins ignore them.
        virtual void SetGlobals(const GlobalParams&) {}

        // Begin/End bracket one target per command list recording. The
        // command list must be open; End() records the draws.
        virtual void Begin(nvrhi::ICommandList* commandList,
                           nvrhi::IFramebuffer* target,
                           uint32_t viewportWidth,
                           uint32_t viewportHeight) = 0;

        // Sorting: every draw carries the current (layer, orderInLayer).
        // End() stable-sorts draws by a 64-bit key -- layer(16) | order(16)
        // | materialId(16) | textureSlot(16) -- giving correct transparency
        // ordering AND minimal state changes in one pass. Ordering WITHIN one
        // (layer, order) is non-deterministic and can change frame to frame
        // (texture slots are assigned per-Begin in first-use order) --
        // overlapping translucent content at the same (layer, order) can
        // flicker. Give overlapping content distinct orderInLayer values.
        // Resets to (0, 0) at Begin().
        virtual void SetLayer(uint16_t layer, uint16_t orderInLayer) = 0;

        // Textured quad: dstPos/dstSize in pixels, uvMin/uvMax in [0,1].
        // `rotation` (radians) spins the quad about its CENTER (default 0 ==
        // axis-aligned, the legacy path); used by RenderSubmissionSystem to turn
        // sprites with their entity's WorldTransform rotation.
        virtual void Quad(glm::vec2 dstPos, glm::vec2 dstSize,
                          nvrhi::ITexture* texture,
                          glm::vec2 uvMin, glm::vec2 uvMax,
                          glm::vec4 color, float rotation = 0.0f) = 0;

        // Textured quad through a REGISTERED material's pipeline: identical
        // geometry/stream to Quad (the tint rides as vertex color -- the
        // snippet sees it as Varyings.color), the material's params/textures
        // bind beside it. An unknown/built-in id falls back to the plain
        // sprite pipeline. Interface default: plain Quad (test doubles record
        // the same geometry).
        virtual void QuadMaterial(uint16_t /*materialId*/,
                                  glm::vec2 dstPos, glm::vec2 dstSize,
                                  nvrhi::ITexture* texture,
                                  glm::vec2 uvMin, glm::vec2 uvMax,
                                  glm::vec4 color, float rotation = 0.0f)
        {
            Quad(dstPos, dstSize, texture, uvMin, uvMax, color, rotation);
        }

        // MSDF glyph quad: same geometry as Quad but rendered through the
        // msdf pipeline (median-of-3 distance + screen-space AA). Text
        // stays on the single submission path.
        virtual void Glyph(glm::vec2 dstPos, glm::vec2 dstSize,
                           nvrhi::ITexture* atlas,
                           glm::vec2 uvMin, glm::vec2 uvMax,
                           glm::vec4 color) = 0;

        // Untextured primitives (white-texture quads / SDF circle quads).
        // `rotation` (radians) spins the rect about its CENTER (default 0).
        virtual void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color,
                          float rotation = 0.0f) = 0;
        virtual void Line(glm::vec2 a, glm::vec2 b, float thickness,
                          glm::vec4 color) = 0;
        virtual void Circle(glm::vec2 center, float radius, glm::vec4 color) = 0;

        // Filled solid-color triangle (untextured). Emitted through the shared
        // quad path with the 4th vertex collapsed onto the 3rd, so the second
        // sub-triangle is degenerate (the rasterizer is cull-none, so winding is
        // irrelevant). For gizmo arrowheads and any solid 2D triangle.
        virtual void Triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c,
                              glm::vec4 color) = 0;

        virtual void End() = 0;

        // Drops the cached texture->binding-set entry for `texture` (no-op
        // when absent or null). Call this BEFORE releasing a texture the
        // batcher has drawn with: the cached set holds a reference that pins
        // the texture alive (leak), and a stale entry would be served for a
        // DIFFERENT texture if the allocator reuses the freed address (ABA).
        // Mirrors ImGuiNvrhiRenderer::DestroyTexture's evict-before-release
        // order. Intended caller: whichever host/system owns dynamic texture
        // lifetimes -- today no engine path feeds dynamically-freed textures
        // into a batcher (the Assets facade's budget eviction releases
        // textures, but nothing routes Assets textures here yet); the
        // Assets/TextureTable integration must call this when it lands.
        virtual void RemoveTexture(nvrhi::ITexture* texture) = 0;

        // Stats for the most recently End()ed batch.
        // Valid after End() until the next Begin(); an empty batch reports all-zero.
        virtual Batch2DStats Stats() const = 0;
    };
}
