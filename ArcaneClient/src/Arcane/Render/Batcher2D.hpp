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

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Arcane
{
    class MaterialTemplate;
    class MaterialInstance;
    struct GlobalParams;

    // A registered scene sprite material (Slice 8): the shader bytecode
    // compiled from the SPRITE template (sprite_material.hlsl register map)
    // plus the layout/values pair. The instance is the SAVED asset's values --
    // scene sprites never render a document's working copy.
    //
    // NO GPU OBJECTS LIVE HERE (NRI Phase 5a, Task 7). This struct used to
    // carry `nvrhi::ShaderHandle vs/ps` and a
    // `std::vector<nvrhi::TextureHandle> paramTextures` beside the bytes
    // below. Those three were BINDING STATE FOR A BINDER THAT NO LONGER
    // EXISTS: the only code that ever read them through a Material2DDesc was
    // the NVRHI recorder inside Batcher2D.cpp, and the graph's Batch2DNode --
    // the one recorder left -- has always resolved its own shaders from
    // `vsBytes`/`psBytes` and its own textures from the instance's Guids
    // through NriTextureCache. They were deleted rather than retyped: a
    // backend-neutral restatement of them would have had no reader.
    //
    // The DECLARED TEXTURE COUNT, which `paramTextures` used to carry
    // implicitly as its size, was never the authority -- `templ->TextureCount()`
    // is, and is what sized the vector. Consumers that need the t1.. width ask
    // the template.
    struct Material2DDesc
    {
        std::shared_ptr<const MaterialTemplate> templ;
        std::shared_ptr<const MaterialInstance> instance;

        // The STITCHED, COMPILED shader blobs -- retained (NRI Phase 2,
        // Task 9) because a SECOND graphics device in the same process cannot
        // use a compiled shader OBJECT, only the bytecode behind it. The graph
        // path's Batch2DNode builds its NRI pipelines from exactly these
        // bytes, so a recompile and a render always agree about which shader
        // ran.
        //
        // ONE target, not both: the producer (SpriteMaterialCache) already
        // picks dxil-or-spirv by the process's GraphicsBackend, and the graph
        // vehicle runs on that same backend (RuntimeApp builds both from one
        // HostConfig). shared_ptr so a Material2DDesc stays cheap to copy and
        // so a RE-compile is observable as a new pointer.
        // Null on a material registered without them -- which is now a REFUSED
        // registration, since nothing else could ever record it.
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
    // and the sorted draw spans. It was named by that task so the NRI graph
    // path's Batch2DNode could consume the SAME CPU batching this class has
    // always done rather than growing a second copy of it beside this one
    // (the homogenized-submission mandate at the top of this file, applied
    // to the second backend).
    //
    // It began as an EXTRACTION, not a redesign -- the two recorders ran the
    // same three lists through the same code, which is what made the NVRHI
    // path a regression floor for the graph path. That floor did its job and
    // is gone: End()'s recording body was deleted at NRI Phase 5a, Task 9.5b
    // and NVRHI left this header entirely at ABI v15 (Task 9.5b-ii). This is
    // now simply the batcher's only output.
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
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        // THE TEXTURE KEY, and since ABI v15 the ONLY one. The image ASSET
        // this run samples at t0, independent of any device: the recorder
        // (Batch2DNode) resolves this Guid through the shared NriTextureCache.
        // Nil means "untextured": Rect/Line/Circle/Triangle, an atlas glyph,
        // and any texture that is not an asset (a render target, the editor's
        // checkerboard) all record nil and bind the white texel. Runs split on
        // it, so two distinct assets are two draws and one shared asset is one.
        //
        // An `nvrhi::ITexture* texture` sat above it from Task 8 until ABI v15
        // (NRI Phase 5a, Task 9.5b-ii), naming the same image as an object on
        // the engine's NVRHI device. It was deleted, not retyped: no such
        // device is created in any configuration, so it was null in every span
        // this process ever built, and the recorder that bound it (End()) went
        // with it.
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
        // Never returns null. It took an `(nvrhi::IDevice*, ShaderLibrary*)`
        // pair from NRI Phase 3, Task 2 until ABI v15 (Phase 5a, Task
        // 9.5b-ii), where null in either argument meant a DEVICE-LESS batcher
        // that skipped every GPU creation in Init. Every call site tree-wide
        // passed (nullptr, nullptr), so device-less was the only state that
        // ever existed; the parameters and the GPU half they selected are gone
        // together, and ShaderLibrary -- which nothing but this signature
        // pinned -- was deleted with them.
        //
        // WHAT A BATCHER DOES, all of it CPU: Begin, SetLayer, every
        // Quad*/Rect/Line/Circle/Triangle/Glyph, RegisterMaterial/
        // UpdateMaterial (bytes-only, see BuildEntry), SetGlobals,
        // MaterialDesc, Drain, Stats. The recorded batch reaches a GPU through
        // Drain(), which the NRI graph path's Batch2DNode consumes.
        static std::unique_ptr<Batcher2D> Create();

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

        // Opens one batch against a canvas of the given extent: clears the
        // recorded streams and resets (layer, order). The batch is read back
        // with Drain(). Took a leading (nvrhi::ICommandList*,
        // nvrhi::IFramebuffer*) pair until ABI v15 -- every caller passed
        // (nullptr, nullptr), and End(), the only thing that read them, is
        // gone.
        virtual void Begin(uint32_t viewportWidth,
                           uint32_t viewportHeight) = 0;

        // Sorting: every draw carries the current (layer, orderInLayer).
        // Drain() stable-sorts draws by a 64-bit key -- layer(16) | order(16)
        // | materialId(16) | textureSlot(16) -- where a texture SLOT is one
        // image-asset Guid in first-use order. Giving correct transparency
        // ordering AND minimal state changes in one pass. Ordering WITHIN one
        // (layer, order) is non-deterministic and can change frame to frame
        // (texture slots are assigned per-Begin in first-use order) --
        // overlapping translucent content at the same (layer, order) can
        // flicker. Give overlapping content distinct orderInLayer values.
        // Resets to (0, 0) at Begin().
        virtual void SetLayer(uint16_t layer, uint16_t orderInLayer) = 0;

        // Quad with explicit UVs: dstPos/dstSize in pixels, uvMin/uvMax in
        // [0,1]. `rotation` (radians) spins the quad about its CENTER
        // (default 0 == axis-aligned, the legacy path).
        //
        // NAMES NO IMAGE, and since ABI v15 it cannot: it carried an
        // `nvrhi::ITexture*` between uvs and size, and that pointer was the
        // only thing that made it "textured". Its span records a nil Guid, so
        // the recorder binds the white texel and the UVs ride the vertices.
        // QuadTextured below is the one that names an asset.
        virtual void Quad(glm::vec2 dstPos, glm::vec2 dstSize,
                          glm::vec2 uvMin, glm::vec2 uvMax,
                          glm::vec4 color, float rotation = 0.0f) = 0;

        // Quad through a REGISTERED material's pipeline: identical
        // geometry/stream to Quad (the tint rides as vertex color -- the
        // snippet sees it as Varyings.color), the material's params/textures
        // bind beside it. An unknown/built-in id falls back to the plain
        // sprite pipeline. Interface default: plain Quad (test doubles record
        // the same geometry).
        virtual void QuadMaterial(uint16_t /*materialId*/,
                                  glm::vec2 dstPos, glm::vec2 dstSize,
                                  glm::vec2 uvMin, glm::vec2 uvMax,
                                  glm::vec4 color, float rotation = 0.0f)
        {
            Quad(dstPos, dstSize, uvMin, uvMax, color, rotation);
        }

        // MSDF glyph quad: same geometry as Quad but recorded against the
        // msdf material (median-of-3 distance + screen-space AA). Text
        // stays on the single submission path.
        //
        // THE ATLAS IS NOT A PARAMETER any more (ABI v15): it was an
        // `nvrhi::ITexture*`, and a glyph atlas is a RUNTIME texture
        // (SkylinePacker output) that no asset Guid names, so the span records
        // nil either way. Atlas residency on the graph path is an open gap,
        // not something this signature ever solved -- see the Batcher2D.cpp
        // Glyph body.
        virtual void Glyph(glm::vec2 dstPos, glm::vec2 dstSize,
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

        // CLOSES THE BRACKET AND RECORDS NOTHING -- a vestige, named as one
        // rather than left to look load-bearing. It WAS the NVRHI recorder;
        // NRI Phase 5a, Task 9.5b deleted its recording body once every
        // Batcher2D in the process proved device-less, and ABI v15 (Task
        // 9.5b-ii) took the NVRHI types out of the class around it. The
        // implementation now refuses once, loudly, and clears the bracket.
        // Draws reach a GPU through Drain(), never through here.
        //
        // Kept, not deleted, only because it is not itself an NVRHI signature
        // and removing a live vtable slot is a separate decision from this
        // task's. A future task may delete it outright.
        virtual void End() = 0;

        // RemoveTexture(nvrhi::ITexture*) is GONE at ABI v15. It evicted a
        // cached texture->binding-set entry before the caller released the
        // texture; the binding-set cache went with the recorder at Task 9.5b,
        // so its loop had been running over a permanently empty map, and its
        // only two callers (SpriteCache::Invalidate/Clear) were guarded on a
        // SpriteEntry::texture that nothing had set since Task 7. All four
        // pieces are deleted together.

        // Stats for the most recently drained batch.
        // Valid after Drain() until the next Begin(); an empty batch reports all-zero.
        virtual Batch2DStats Stats() const = 0;

        // Sorts this batch's recorded quads and builds the index + draw-span
        // streams, then hands back a view of all three (see Batch2DDrained).
        // The NRI graph path's Batch2DNode is its consumer: it reads this CPU
        // batching and issues its own draws through NRI.
        //
        // Idempotent inside one Begin() bracket -- re-entry (declaration-time
        // vs render-time within one bracket) sorts and builds exactly once.
        // Interface DEFAULT (not pure), for the same reason RegisterMaterial
        // is: the geometry-recording test doubles stay valid, and a double
        // that records nothing drainable honestly reports an empty batch.
        //
        // NEW VIRTUALS GO AT THE END OF THIS CLASS, AND THAT IS NOT TIDINESS.
        // A game module compiles its OWN copy of this header (RenderContext2D
        // in SceneResources.hpp -> RenderSubmissionSystem) and dispatches
        // through this vtable, so inserting a virtual anywhere ABOVE an
        // existing one slides every slot after it and a stale module calls one
        // slot into the next. Appending cannot slide anything. The plugin ABI
        // gate is bumped alongside (PluginABI.hpp v11 for this one) so a stale
        // module is refused outright rather than merely surviving by luck;
        // keep both.
        //
        // ABI v15 BROKE THAT RULE DELIBERATELY, once: removing RemoveTexture
        // from the middle of the class slid Stats/Drain/MaterialDesc/
        // QuadTextured up one slot each. That is exactly the hazard above, and
        // it is safe only because the gate refuses every pre-v15 module. The
        // rule still stands for everything additive.
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

        // THE ASSET-IDENTITY SUBMISSION (NRI Phase 3, Task 2), and since ABI
        // v15 the ONLY way to draw an image. Identical to QuadMaterial in
        // every respect -- same geometry, same stream, same material dispatch
        // (pass kMaterialSprite for the plain path) -- with ONE addition:
        // `textureId` names the image ASSET, and it travels into the drained
        // span (Batch2DDrawSpan::textureId) for the recorder to resolve
        // through NriTextureCache.
        //
        // WHY A PARAMETER AND NOT STATE. A sticky "current texture id" would
        // have to be cleared by every one of the batcher's many other callers
        // (physics debug draw, gizmos, the editor's own overlays) or they would
        // inherit the last sprite's identity and sample the wrong image. There
        // is no such failure mode when the id rides the call that uses it.
        //
        // IT USED TO CARRY BOTH KEYS. An `nvrhi::ITexture*` sat beside the
        // Guid from Task 2 until v15, because the batcher then had an NVRHI
        // recorder to feed. No NVRHI device is created in any configuration,
        // so that pointer was null at every call; the Guid always was the one
        // that could tell "untextured" from "textured on a device that is not
        // there", and now it is the only one.
        //
        // Interface DEFAULT (not pure), like QuadMaterial: a geometry-recording
        // test double records the same quad, minus an identity it has nowhere
        // to put.
        virtual void QuadTextured(uint16_t materialId, const Guid& /*textureId*/,
                                  glm::vec2 dstPos, glm::vec2 dstSize,
                                  glm::vec2 uvMin, glm::vec2 uvMax,
                                  glm::vec4 color, float rotation = 0.0f)
        {
            QuadMaterial(materialId, dstPos, dstSize, uvMin, uvMax,
                         color, rotation);
        }
    };
}
