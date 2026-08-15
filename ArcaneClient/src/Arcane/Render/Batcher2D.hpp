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
#include <Arcane/Guid.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
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

        // The STITCHED, COMPILED blobs `vs`/`ps` were created from -- retained
        // (NRI Phase 2, Task 9) because a SECOND graphics device in the same
        // process cannot use an nvrhi::ShaderHandle, only the bytecode behind
        // it. The graph path's Batch2DNode builds its own NRI pipelines from
        // exactly these bytes, so both recorders run the same shader rather
        // than two independently compiled ones.
        //
        // ONE target, not both: the producer (SpriteMaterialCache) already
        // picks dxil-or-spirv by the process's GraphicsBackend, and the graph
        // vehicle runs on that same backend (RuntimeApp builds both from one
        // HostConfig). shared_ptr so a Material2DDesc stays cheap to copy and
        // so a RE-compile is observable as a new pointer.
        // Null on a material registered without them: the graph path then
        // falls back to the plain sprite pipeline and says so once.
        std::shared_ptr<const std::vector<std::uint8_t>> vsBytes;
        std::shared_ptr<const std::vector<std::uint8_t>> psBytes;
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

    // =====================================================================
    // THE DRAINED BATCH -- the batcher's READ interface (NRI Phase 2,
    // Task 8).
    //
    // Everything below is what End() used to compute privately and then
    // immediately record: the interleaved vertex stream, the index stream,
    // and the sorted draw spans. It is now named, so the NRI graph path's
    // Batch2DNode can consume the SAME CPU batching this class has always
    // done rather than growing a second copy of it beside this one (the
    // homogenized-submission mandate at the top of this file, applied to
    // the second backend).
    //
    // This is an EXTRACTION, not a redesign: End() computes these three
    // lists exactly as before, through the same code, and then records
    // exactly as before. The NVRHI path is the phase's regression floor.
    // =====================================================================

    // One vertex of the batch stream. THE WIRE FORMAT: it is what the
    // vertex buffer contains on both backends, and what data/shaders/
    // sprite.hlsl's VSInput (POSITION/TEXCOORD0/COLOR0) declares.
    struct Batch2DVertex
    {
        glm::vec2 pos;
        glm::vec2 uv;
        glm::vec4 color;
    };
    static_assert(sizeof(Batch2DVertex) == 32, "vertex layout is the wire format");

    // One contiguous run of sorted quads sharing a material and a texture --
    // i.e. exactly one draw call. `firstIndex`/`indexCount` index the drained
    // INDEX stream; `material` is a Batcher2D::kMaterial* built-in or a
    // registered id.
    struct Batch2DDrawSpan
    {
        uint16_t material = 0;
        // THE NVRHI KEY. Still here, still what End() binds -- the NVRHI
        // recorder owns real texture objects and must keep naming them.
        nvrhi::ITexture* texture = nullptr;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        // THE ASSET KEY (NRI Phase 3, Task 2), APPENDED so nothing above it
        // moves. The image ASSET this run samples at t0, independent of any
        // device: the second recorder (Batch2DNode) cannot use `texture` --
        // that object lives on the engine's NVRHI device -- so it resolves
        // this Guid through the shared NriTextureCache instead. Nil means
        // "untextured": Rect/Line/Circle/Triangle, an atlas glyph, and any
        // texture that is not an asset (a render target, the editor's
        // checkerboard) all record nil and bind the white texel.
        //
        // BOTH KEYS, ONE TRUTH. A device-carrying batcher fills both and they
        // describe the same image; a DEVICE-LESS one (Batcher2D::Create with a
        // null device) fills only this one, because there is no texture object
        // to point at. Runs split on BOTH, so the two recorders always draw
        // the same number of draws in the same order.
        Guid textureId{};
    };

    // A VIEW over the batcher's own storage -- it owns nothing. The spans stay
    // valid until the next Begin() on the batcher that produced them, which is
    // the whole of the consumer's contract (one graph frame is declared,
    // recorded and submitted well inside that window).
    struct Batch2DDrained
    {
        std::span<const Batch2DVertex>   vertices;
        std::span<const uint32_t>        indices;
        std::span<const Batch2DDrawSpan> spans;
        // The viewport Begin() was given -- the canvas extent this batch's
        // pixel coordinates were built against, and therefore what a consumer
        // must derive its projection push-constants from (Batcher2D.cpp's
        // PushConstants{2/viewport}).
        glm::vec2 viewport{ 0.0f };
        // What the host last SetGlobals()'d -- the engine-global constants
        // every REGISTERED material samples (b2 on the sprite register map,
        // GlobalParams.hpp). Points at the batcher's own member, so it is
        // valid for as long as the batcher is; Drain() sets it unconditionally
        // (an empty drain still gets a non-null pointer). The only null case
        // is the interface's default Drain() override, which returns a
        // default-constructed Batch2DDrained -- e.g. a test double that never
        // overrides it. Built-in spans ignore it, exactly as End() does.
        const GlobalParams* globals = nullptr;

        [[nodiscard]] bool Empty() const noexcept { return spans.empty(); }
    };

    class ARCANE_API Batcher2D
    {
    public:
        // Returns null (with ARC_ERROR) when the batch shaders are missing.
        //
        // BOTH ARGUMENTS MAY BE NULL (NRI Phase 3, Task 2) -- and null in
        // EITHER makes a DEVICE-LESS batcher, because the GPU half needs both.
        // A device-less instance skips every GPU creation in Init (the white
        // texel, the sampler, the binding layout, the input layout, the
        // built-in shader probe) and therefore never returns null for a
        // missing shader either -- there is nothing to build.
        //
        // WHAT STILL WORKS device-less, and it is the whole data-supply side:
        // Begin, SetLayer, every Quad*/Rect/Line/Circle/Triangle/Glyph,
        // RegisterMaterial/UpdateMaterial (bytes-only, see BuildEntry),
        // SetGlobals, MaterialDesc, Drain, Stats, RemoveTexture. WHAT DOES
        // NOT: End(), which IS the NVRHI recorder -- it refuses loudly and
        // records nothing.
        //
        // This is the phase's severance: the frame's data supply stops
        // depending on an NVRHI device so Task 6 can drop that device without
        // dropping the batching with it.
        static std::unique_ptr<Batcher2D> Create(nvrhi::IDevice* device,
                                                 ShaderLibrary* shaders);

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
        // | materialId(16) | textureSlot(16) -- where a texture SLOT is one
        // (texture object, image-asset Guid) pair in first-use order, so a
        // device-less batcher (whose texture objects are all null) still
        // separates distinct assets. Giving correct transparency
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
        // lifetimes -- i.e. whatever populates the scene's SpriteTable
        // (SceneResources.hpp) with Assets-loaded textures, since the Assets
        // facade's budget eviction can release a texture the batcher has
        // already drawn with.
        virtual void RemoveTexture(nvrhi::ITexture* texture) = 0;

        // Stats for the most recently End()ed batch.
        // Valid after End() until the next Begin(); an empty batch reports all-zero.
        virtual Batch2DStats Stats() const = 0;

        // Sorts this batch's recorded quads and builds the index + draw-span
        // streams WITHOUT recording anything, then hands back a view of all
        // three (see Batch2DDrained). The NRI graph path's Batch2DNode calls
        // this instead of End(): it consumes the same CPU batching and issues
        // its own draws through NRI.
        //
        // Idempotent inside one Begin() bracket -- End() runs the same work
        // through the same code, so calling BOTH (never done today, but
        // harmless) sorts and builds exactly once. Interface DEFAULT (not
        // pure), for the same reason RegisterMaterial is: the geometry-
        // recording test doubles stay valid, and a double that records nothing
        // drainable honestly reports an empty batch.
        //
        // DECLARED LAST, AND THAT IS NOT TIDINESS. A game module compiles its
        // OWN copy of this header (RenderContext2D in SceneResources.hpp ->
        // RenderSubmissionSystem) and dispatches through this vtable, so
        // inserting a virtual anywhere ABOVE an existing one slides every slot
        // after it -- a stale module would call Stats() into
        // RemoveTexture(ITexture*). Appending cannot slide anything. The
        // plugin ABI gate was ALSO bumped (PluginABI.hpp v11) so a stale module
        // is refused outright rather than merely surviving by luck; keep both,
        // and keep new virtuals at the end.
        virtual Batch2DDrained Drain() { return {}; }

        // The registration data behind a REGISTERED material id -- shaders,
        // template layout, values instance -- for a consumer that has to build
        // its OWN pipeline and bindings from it. The NRI graph path's
        // Batch2DNode is that consumer (Phase 2, Task 9): a drained span names
        // a material id, and this is the only way back from the id to what the
        // id means.
        //
        // Null for a built-in id (0..2), an id this batcher never issued, and
        // on a test double that registers nothing. The returned pointer is
        // owned by the batcher and is invalidated by UpdateMaterial on the SAME
        // id -- read it inside the frame that drained the span, never store it.
        //
        // APPENDED, and it stays appended -- see Drain()'s comment above for
        // why every new virtual goes at the end of this class. ABI v12.
        virtual const Material2DDesc* MaterialDesc(uint16_t /*id*/) const { return nullptr; }

        // THE ASSET-IDENTITY SUBMISSION (NRI Phase 3, Task 2). Identical to
        // QuadMaterial in every respect -- same geometry, same stream, same
        // material dispatch (pass kMaterialSprite for the plain path) -- with
        // ONE addition: `textureId` names the image ASSET behind `texture`, and
        // it travels into the drained span (Batch2DDrawSpan::textureId).
        //
        // WHY A PARAMETER AND NOT STATE. A sticky "current texture id" would
        // have to be cleared by every one of the batcher's many other callers
        // (physics debug draw, gizmos, the editor's own overlays) or they would
        // inherit the last sprite's identity and sample the wrong image. There
        // is no such failure mode when the id rides the call that uses it.
        //
        // WHY THE HOST MUST USE IT. In graph mode there is no NVRHI device, so
        // Assets::GetTexture yields null for every sprite and `texture` is
        // ALWAYS null -- the pointer can no longer tell "untextured" from
        // "textured on a device that does not exist". The Guid can.
        //
        // Interface DEFAULT (not pure), like QuadMaterial: a geometry-recording
        // test double records the same quad, minus an identity it has nowhere
        // to put. APPENDED, ABI v13.
        virtual void QuadTextured(uint16_t materialId, const Guid& /*textureId*/,
                                  glm::vec2 dstPos, glm::vec2 dstSize,
                                  nvrhi::ITexture* texture,
                                  glm::vec2 uvMin, glm::vec2 uvMax,
                                  glm::vec4 color, float rotation = 0.0f)
        {
            QuadMaterial(materialId, dstPos, dstSize, texture, uvMin, uvMax,
                         color, rotation);
        }
    };
}
