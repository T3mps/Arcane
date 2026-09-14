#pragma once

// MeshNode -- THE OPAQUE 3D PASS as a render graph node (NRI Phase 4, Task 7).
//
// This is the node that puts a lit, textured, DEPTH-TESTED mesh through the
// frame graph. It renders into the frame's canvas (the same RGBA16F transient
// the 2D batch draws into) with a D32_SFLOAT depth target it creates itself.
//
// WHAT IT IS DELIBERATELY NOT:
//   * NOT PBR. data/shaders/mesh.hlsl is one directional light, Lambert
//     diffuse plus a constant ambient term, one albedo texture. The GGX
//     material model belongs to the Deadlock-class renderer arc; a half
//     version here would be a second material model to reconcile.
//   * NOT a commitment to forward OR deferred. One opaque pass writing a
//     colour target and a depth target is the shared prefix of both.
//   * NOT a mesh IMPORT path. Geometry comes from MeshBuilder (procedural
//     cube/sphere); cgltf/.arcmesh is a later arc, and no library is vendored
//     for it here.
//   * NOT the material system. A per-instance linear tint (MeshInstance::
//     baseColor) plus, since Task 10, an optional per-instance MATERIAL SLOT
//     (MeshInstance::materialSlot) indexing this node's own BindlessTable is
//     the whole of it. A per-instance albedo Guid resolved into per-image
//     descriptor sets was written and then REMOVED at Task 7's first fix
//     round -- nothing exercised it, and Task 8/10's BindlessTable replaced
//     the fixed white-texel t0 binding it would have competed with outright.
//     Feeding the table from a REAL cooked albedo (rather than a test's own
//     generated textures) is Task 11's -- LANDED, in NriGraphContext::
//     ResolveMeshAlbedoSlot, not in this node -- this node still only ever
//     sees an already-resolved nri::Descriptor* through AddMaterial.
//
// WHERE IT SITS IN THE FRAME: after `batch2d`, before the post chain and the
// tonemap. The canvas is MINTED AND CLEARED by AddBatch2DNode, so the mesh
// pass is necessarily the canvas's second writer -- it clears the DEPTH plane
// (which is a fresh pool slot with undefined contents) and never the colour
// one, or it would wipe the 2D content that ran before it. Rendering 3D UNDER
// the 2D layer instead is a clear-seam question (NriGraphContext::
// DeclareGraphFrame's THE CLEAR SEAM block), not this node's.
//
// WHAT THIS NODE OWNS (all persistent, all created once at Create()):
//   * the BINDLESS MATERIAL TABLE (Task 8/10) -- a BindlessTable of
//     kBindlessCapacity SRV slots, gated on NriDeviceCaps::SupportsBindless()
//     at Create() (tier 0 refuses the node outright rather than render wrong
//     pixels) -- and the ONE
//     descriptor set its array lives in. This REPLACED the node's own 1x1
//     white texel + t0 binding outright: `kInvalidSlot` (MeshInstance::
//     materialSlot's default) now selects a FLAT path in mesh.hlsl that
//     skips sampling entirely, bit-for-bit F2a's old white-times-tint
//     arithmetic;
//   * the pipeline layout -- root constants b0 (the 128-byte MeshConstants
//     from mesh.hlsl: model + tint + the per-instance normal matrix, Task 8/
//     F2a) at the implicit rootRegisterSpace, ONE immutable ROOT SAMPLER at
//     s0 (Task 10's slice-one trilinear sampler -- also rootRegisterSpace;
//     THE REGISTER-SPACE RULE below is why that space can no longer be
//     shared with either descriptor set), plus TWO ordinary descriptor sets:
//     space1 = { b1 frame CB }, space2 = { t0 bindless material array };
//   * one descriptor pool, and ONE descriptor set PER FRAME SLOT for the b1
//     CB (CreateSets) -- written ONCE each, at Create, and never rewritten.
//     That is the same discipline Batch2DNode keeps, and stronger: the sets
//     exist before the first frame does, so there is no window in which the
//     GPU could be reading one (and NRI cannot free a single set anyway).
//     The bindless set is DIFFERENT: allocated ONCE (not per frame slot,
//     since its contents do not vary by frame) and written INCREMENTALLY by
//     AddMaterial as slots are Added -- see that method's own comment for
//     the synchronization contract that discipline costs;
//   * the per-frame-slot constant-buffer arena the b1 views name.
// The PIPELINE is not owned here: it comes from the vehicle's shared
// NriPipelineCache, keyed by (shader pair, layout, canvas format, DEPTH
// format, blend), so a format change is a cache miss rather than a stale PSO.
//
// THE REGISTER-SPACE RULE (Batch2DNode.hpp states it in full, verified
// against Source/Validation/DeviceVal.hpp's `rootDescriptorNum ||
// rootSamplerNum` guard): NRI refuses a pipeline layout whose
// rootRegisterSpace equals any descriptor set's registerSpace, but ONLY
// once the layout carries a root DESCRIPTOR or root SAMPLER -- root
// CONSTANTS alone are exempt (VK lowers them to push constants, outside the
// set-space numbering entirely). Batch2DNode has neither and keeps
// everything at space0; THIS layout is the first in the tree to add a root
// SAMPLER (Task 10's immutable s0), so its root space and its two ordinary
// sets must all be distinct register spaces -- see MeshNode.cpp's
// CreateBindings() for the concrete numbers and mesh.hlsl for the matching
// `register(..., spaceN)` annotations, including the SPIR-V register-shift
// entries (compile-shaders.bat / ShaderConventions.hpp::kSpirvArgs) that
// have to agree with them.
//
// WHY THE SETS ARE PER FRAME SLOT and Batch2DNode's built-in ones are not: a
// set here carries the per-frame constant buffer b1, whose (buffer, offset) is
// baked into its nri::Descriptor at creation. Double-buffering the CB means
// double-buffering the set that names it. See Batch2DNode.hpp's THE
// CONSTANT-BUFFER ARENA for the whole account of why the upload ring cannot
// back a constant buffer that a descriptor set names.
//
// WINDING AND CULLING -- read MeshBuilder.hpp's WINDING block first. That
// module emits triangles that are COUNTER-CLOCKWISE as seen from OUTSIDE the
// surface, and this node's rasterizer state is set to match:
// `frontCounterClockwise = true` + `cullMode = BACK`. See MeshNode.cpp's
// PipelineFor() for the full derivation (and the desk check that falsifies it
// in one look: get it backwards and a closed mesh renders INSIDE-OUT or, for a
// convex one, vanishes entirely).
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR and
// <windows.h> #defines ERROR via wingdi.h).
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/MeshBuilder.hpp>      // MeshData / MeshVertex -- the CPU geometry
#include <Arcane/Render/Nri/BindlessTable.hpp> // MeshInstance::materialSlot's kInvalidSlot default
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/FramePacing.hpp>      // kSwapchainFramesInFlight

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;

    // NormalMatrixFor -- the inverse transpose of `model`'s upper 3x3, which is
    // the transform that keeps a surface normal PERPENDICULAR TO ITS SURFACE
    // under a NON-UNIFORM scale. (Why the upper 3x3 alone is wrong: a surface
    // tangent scales WITH the object, so for dot(normal, tangent) to stay zero
    // after a non-uniform scale, the normal has to scale by the INVERSE along
    // each axis, not the same factor. A uniform scale s*I is the one case
    // where this doesn't matter -- its inverse transpose is (1/s)*I, a
    // positive multiple of I that this file's vs_main normalize() divides
    // straight back out -- which is exactly why the upper-3x3 shortcut looked
    // correct until F1 gave Transform a per-axis glm::vec3 scale the Inspector
    // authors freely: the first non-uniform scale-handle drag on a mesh hits
    // this.)
    //
    // SINGULAR GUARD -- computed, not predicted. `glm::inverse` runs
    // UNCONDITIONALLY below; the guard checks its OUTPUT (all nine elements
    // finite) rather than pre-screening `upper`'s determinant against a fixed
    // threshold. A pre-screen is a CONDITIONING HEURISTIC, not a test for the
    // true singular set, and the difference is not academic: a 3x3
    // determinant scales as s^3 under a uniform scale s, so no fixed
    // threshold is scale-invariant. A too-tight one -- the bug this shape
    // actually had -- lets a small NON-UNIFORM scale (whose determinant can
    // sit anywhere, independent of how ill-conditioned any one axis is) sail
    // past the check and fall through to a normal transform that is silently
    // wrong, reinstating exactly the defect this function exists to fix, with
    // no diagnostic. Checking the RESULT instead is exact by construction:
    // `glm::inverse` divides by the determinant unconditionally, so a genuine
    // singularity (or a non-finite `model`, e.g. a degenerate ancestor in a
    // WorldTransform product) is precisely the input for which the computed
    // inverse transpose comes back non-finite, and nothing else does.
    //
    // WHY GUARD AT ALL: an Inf/NaN normal is undefined behaviour on the GPU
    // rather than a wrong picture -- the same class of guard SceneCamera.hpp's
    // degenerate-basis fallback (ActivePerspectiveSceneCamera) takes, and the
    // same class MeshNode.cpp's own IsFinite/SafeNormalize take for the
    // camera and the light. Identity is the least-wrong answer here: an
    // instance whose model has collapsed a dimension has already lost its
    // geometry to the same degeneracy (its vertices are degenerate too), so
    // there is no "correct" normal direction left to recover -- identity just
    // keeps the pixel shader's arithmetic finite.
    //
    // PURE and header-only, matching SceneCamera.hpp's PerspectiveProjection:
    // no NRI device, no Registry, no MeshNode instance, so ArcaneTests can pin
    // the analytic property directly (MeshNodeTest.cpp).
    //
    // Computed PER INSTANCE inside MeshNode::Record from `model` alone --
    // MeshInstance gains no field for this; see its own comment below.
    [[nodiscard]] inline glm::mat3 NormalMatrixFor(const glm::mat4& model) noexcept
    {
        const glm::mat3 upper = glm::mat3(model);
        const glm::mat3 inverseTranspose = glm::transpose(glm::inverse(upper));
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (!std::isfinite(inverseTranspose[c][r]))
                    return glm::mat3(1.0f);
        return inverseTranspose;
    }

    // =====================================================================
    // THE CPU-SIDE SCENE, and the whole of what a host has to build.
    //
    // Deliberately NOT an ECS view, a Registry walk or a Scene pointer: this
    // is a RENDER vehicle's input, the same shape and the same borrowing rules
    // FrameDesc::pickables carries (Render/PickEmit.hpp is its sibling). The
    // thing that owns a registry and a camera is the FRAME DRIVER, and Task 9
    // is what teaches both hosts to fill this in.
    // =====================================================================
    struct MeshInstance
    {
        // THE MESH'S ASSET GUID, not a borrowed CPU pointer (F2c s7.2). Geometry is
        // resolved through the vehicle's NriMeshBufferCache at DECLARATION time and is
        // already RESIDENT on the device by the time this node records -- so the
        // borrow-lifetime contract this field used to carry (a raw MeshData* that had
        // to outlive the RenderFrame call, copied into the ring every frame) is gone
        // with the ring path it existed for.
        //
        // NIL means "draw nothing", which is not an error (a scene may legitimately
        // carry a slot with no geometry yet) -- unchanged.
        Guid mesh{};

        // Model -> world, METERS (MKS). Rotation, translation AND a
        // NON-UNIFORM scale are all safe here (Task 8/F2a): MeshNode::Record
        // derives NormalMatrixFor(model) fresh for every instance and pushes
        // it alongside `model`, so mesh.hlsl's vs_main transforms normals by
        // the inverse transpose rather than the upper 3x3. (Before Task 8 this
        // comment stated a UNIFORM-scale-only restriction -- F1 gave Transform
        // a glm::vec3 scale the Inspector authors freely, so the first
        // non-uniform scale-handle drag on a mesh hit the old shortcut.
        // NormalMatrixFor, above, is the fix; nothing here still depends on
        // that restriction.)
        glm::mat4 model{1.0f};

        // LINEAR, and may exceed 1.0 (the canvas is RGBA16F). When
        // `materialSlot` is `kInvalidSlot` (the default) this IS the
        // instance's colour outright -- mesh.hlsl's flat path, bit-for-bit
        // F2a's original behaviour (see `materialSlot`'s own comment).
        // Otherwise it multiplies the bindless texture's sample, same as
        // F2a's white-texel arithmetic always meant it to.
        glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};

        // ===== THE MATERIAL SLOT (Task 8/10) =====
        // Indexes THIS NODE'S OWN BindlessTable -- a slot AddMaterial handed
        // back for an SRV Added to a DIFFERENT MeshNode's table names a
        // different node's texture entirely (or nothing), a silent
        // wrong-picture bug the type system cannot catch, so callers must
        // never mix them. `kInvalidSlot`, the default, is what a mesh
        // carrying no bindless material uses -- it selects mesh.hlsl's FLAT
        // path (baseColor alone, no sample), which is F2a's original
        // behaviour: an all-white 1x1 t0 texel times baseColor, and 1.0 * x
        // is exact under IEEE-754, so this is that same result with the now-
        // redundant sample removed, not an approximation of it. A `Guid
        // albedo` resolved through the shared NriTextureCache into a
        // BindlessTable slot was written and then REMOVED at Task 7's first
        // fix round; Task 8/10's BindlessTable + this field is what finally
        // replaces it, and feeding a REAL cooked albedo in here (rather than
        // a test's own generated texture) is Task 11's -- landed: `Guid
        // albedo` now lives on `Arcane::ResolvedMeshMaterial`
        // (Scene/SceneResources.hpp), resolved into a slot by
        // NriGraphContext::ResolveMeshAlbedoSlot and copied onto this field
        // by CollectMeshInstances (Scene/MeshSubmissionSystem.hpp).
        //
        // PACKED INTO THE ROOT CONSTANTS' `normalMatrixCol0.w` by
        // MeshNode::Record (the 128-byte MeshRootConstants budget has ZERO
        // headroom -- see PackedNormalMatrix's comment in MeshNode.cpp --
        // so this field does not grow that struct). `col0.w` is otherwise
        // unused (the shader reads only `.xyz` off every normal-matrix
        // column, mesh.hlsl:111-113, and C++ zeroes every column's `.w` at
        // PackedNormalMatrix construction, MeshNode.cpp), so writing the
        // slot there and reading it back via asuint/asfloat is safe --
        // `kInvalidSlot` (0xFFFFFFFF) is a NaN bit pattern, which is only
        // ever safe as RAW BITS. Never route `col0` through float
        // arithmetic once the slot has been written into it.
        std::uint32_t materialSlot = BindlessTable::kInvalidSlot;

        // WHICH SECTION of `mesh` this instance draws (F2c s7.4). A multi-section prop
        // becomes sections.size() instances, one per section, each carrying the
        // material its slot resolved to -- which is why the 128-byte zero-headroom
        // MeshConstants block (mesh.hlsl:20-28) is UNTOUCHED by sections: the material
        // identity already travels per draw, in materialSlot.
        std::uint32_t indexOffset = 0;
        std::uint32_t indexCount  = 0;   // 0 == "the whole mesh", the F2a shape
    };

    struct MeshSceneDesc
    {
        // BORROWED SPAN for the duration of the RenderFrame call, exactly like
        // FrameDesc::pickables: the declaration copies the SPAN into the
        // node's exec fn, never the elements, and the exec fn runs inside the
        // same call. The elements name meshes by Guid; they no longer point at
        // CPU geometry. EMPTY IS LEGAL and means "no mesh pass this frame" --
        // DeclareGraphFrame declares no node for it, so an empty scene costs
        // the same as no scene at all.
        std::span<const MeshInstance> instances;

        // The camera, already resolved by the frame driver. TWO matrices
        // rather than one product because that is what SceneCamera hands back
        // (PerspectiveCameraView, Task 5) and because a later pass that needs
        // the view alone should not have to un-multiply it.
        //
        // THE CALLER OWES VALID MATRICES. SceneCamera::PerspectiveProjection
        // is an unvalidated pure function -- a zero fov, a non-positive aspect
        // ratio or nearZ >= farZ silently produces NaN/Inf -- so build these
        // through the guarded path (ActivePerspectiveSceneCamera) or check
        // them. MeshNode::Record checks them too and drops the pass with one
        // ERROR rather than recording a draw with a NaN transform, because a
        // NaN clip position is undefined behaviour on the GPU rather than a
        // wrong picture.
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};

        // THE ONE DIRECTIONAL LIGHT. `lightDirection` points TOWARD the light.
        // It does NOT have to be unit length: MeshNode::Record normalizes it
        // once per frame on the CPU and mesh.hlsl consumes the result directly.
        //
        // A ZERO-LENGTH VECTOR IS LEGAL and means "no directional light" --
        // the pass falls back to the ambient term alone. That is a GUARD, not
        // a convention: `normalize()` on a zero vector is a division by zero
        // and yields NaN, which would propagate through N.L into every lit
        // pixel. Normalizing on the CPU is what lets the zero case be handled
        // at all (and costs one normalize per frame instead of one per pixel).
        //
        // `ambient` is a flat term added to every lit surface -- the whole of
        // the indirect lighting model here, deliberately.
        glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
        glm::vec3 lightColor{1.0f, 1.0f, 1.0f};
        glm::vec3 ambient{0.05f, 0.05f, 0.05f};
    };

    class ARCANE_API MeshNode
    {
    public:
        // Loads mesh_vs/mesh_ps through the vehicle, creates the bindless
        // material table + descriptor pool + constant arena, and registers
        // the pipeline layout. Null (already logged + latched) on any
        // failure -- a vehicle that cannot build this node must not render a
        // frame that silently draws nothing. Refuses FIRST, before any of
        // that, on a device whose NriDeviceCaps::SupportsBindless() is false
        // (bindless tier 0) -- the house refuse-loudly posture: this node's
        // material table cannot mean anything on hardware that cannot index
        // it, so it does not get built at all rather than rendering wrong
        // pixels or silently falling back.
        static std::unique_ptr<MeshNode> Create(NriGraphContext& context);

        // SAFETY NET, NOT THE PATH -- same shape as ~Batch2DNode. The
        // sanctioned release is Release() at a fence the owner knows.
        ~MeshNode();

        MeshNode(const MeshNode&)            = delete;
        MeshNode& operator=(const MeshNode&) = delete;

        // Buries every NRI object this node owns at `fence` and empties it.
        // Idempotent. The caller picks the fence for the same reason
        // NriPipelineCache::Clear does. Buries the bindless table's own
        // descriptors too (BindlessTable::Release), so an SRV a caller Added
        // via AddMaterial is discharged the same way everything else here
        // is -- the caller still owns the TEXTURE that view names (see
        // AddMaterial's own comment) and must release that separately.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        // Registers `srv` in this node's BindlessTable and writes it into
        // the bindless descriptor set at the slot BindlessTable::Add
        // returns -- `kInvalidSlot` on refusal (null `srv`, the table is at
        // capacity, or Create() never built one). The returned slot is what
        // a MeshInstance::materialSlot names to select `srv` at draw time.
        //
        // OWNERSHIP: exactly BindlessTable::Add's -- this node's table takes
        // ownership of the VIEW (`srv`) and discharges it at Release()/
        // destruction. The TEXTURE `srv` views is a SEPARATE object this
        // node never touches; the caller (Task 11's feed, or a test's own
        // generated texture) still owns it and must outlive both the view
        // and this node's use of the returned slot.
        //
        // SYNCHRONIZATION (Task 11 revisited this): CreateBindings' bindless
        // range/set/pool now carry ALLOW_UPDATE_AFTER_SET (on top of Task
        // 10's ARRAY | PARTIALLY_BOUND), so this write is safe to issue at
        // ANY time relative to earlier frames' command buffers -- including
        // ones still in flight on the GPU -- as long as the SLOT being
        // written has never been read by any of them, which BindlessTable's
        // own Add-only policy guarantees by construction (a fresh Add always
        // claims the next UNUSED dense slot; nothing here ever rewrites a
        // slot a prior instance's materialSlot could already be indexing).
        // That is exactly the property a live feed needs: SceneRenderResolver
        // ->MeshMaterialCache->NriGraphContext::ResolveMeshAlbedoSlot calls
        // this once per newly-seen albedo Guid, on whatever frame first
        // references it, with no fence wait and no requirement that earlier
        // frames have retired first. Before Task 11 this call was only safe
        // "before this set has ever been bound to a command buffer" -- see
        // git history for that account if the update-after-bind wiring is
        // ever questioned.
        [[nodiscard]] std::uint32_t AddMaterial(nri::Descriptor* srv);

        // Resolves the PIPELINE for the colour format the frame being declared
        // will attach. Called at DECLARATION time for the reason
        // Batch2DNode::Prepare is: a PSO compile must not land inside the
        // recording window, where a first-frame miss would stall it and a
        // FAILED miss would latch an error on a frame that is otherwise fine.
        //
        // Resolves the PIPELINE **and** this frame's mesh residency. The residency half
        // is here for the same reason the pipeline half is: NriMeshBufferCache::Resolve
        // uploads through HelperInterface::UploadData, which SUBMITS AND WAITS
        // INTERNALLY -- doing that inside an open command buffer is exactly the shape
        // the graph's no-hand-barriers rule exists to prevent (NriTextureCache.hpp's NO
        // BARRIERS section states it for images; geometry is the same hazard with a
        // bigger payload). Record() therefore only ever LOOKS UP what Prepare made
        // resident, and never resolves.
        //
        // Safe to skip entirely -- Record() then reports the missing pipeline
        // once and draws nothing. Device-less [nri] frame-shape cases pass a
        // null context to AddMeshNode, which skips this.
        //
        // `meshBuffers` is NULLABLE, and the two halves are independent: a null cache
        // skips the RESIDENCY loop ONLY, and the pipeline is still resolved. Gating
        // both on the cache (the pre-fix call-site shape) meant a hypothetically
        // cache-less context also lost its pipeline and drew nothing behind a
        // "missing pipeline" warning that named the wrong cause.
        void Prepare(nri::Format canvasFormat, const MeshSceneDesc& scene,
                     NriMeshBufferCache* meshBuffers, std::uint64_t frameCounter);

        // Records one scene's opaque geometry into an ALREADY-OPEN raster pass
        // whose colour attachment is the canvas and whose depth attachment is
        // this node's depth target. In order: clear the DEPTH plane (the clear
        // seam -- graph attachments are LOAD/STORE, see
        // NriGraphContext::DeclareGraphFrame), bind each distinct mesh's
        // already-resident vertex/index buffers, then one CmdDrawIndexed per
        // instance. Never uploads; never touches the frame ring for geometry.
        //
        // IT DOES NOT CLEAR THE COLOUR PLANE. batch2d already cleared and drew
        // into the canvas; clearing it here would erase that.
        //
        // Emits NO barrier: the executor derives and batches every one of them
        // from the declarations.
        //
        // `frameSlot` is the vehicle's own per-frame slot -- the SAME number it
        // gave the upload ring, so this node's constant-buffer arena is
        // double-buffered against exactly the fence the swapchain already waits
        // on.
        //
        // NO canvasFormat PARAMETER, unlike Batch2DNode::Record: that node
        // resolves a pipeline PER SPAN at record time and needs the format
        // there, while this one has exactly one pipeline and Prepare already
        // keyed it. A parameter this function did not read would just be
        // something for a reader to reason about.
        void Record(RenderGraphNodeContext& context, const MeshSceneDesc& scene,
                    std::uint32_t frameSlot);

        // The b1 block's region size BEFORE alignment. mesh.hlsl's MeshFrameCB
        // is 112 bytes; 256 is also D3D12's constant-buffer placement
        // alignment, so on that backend this is exactly one region.
        static constexpr std::uint32_t kFrameCbMaxBytes = 256;

        // THE BINDLESS MATERIAL TABLE'S CAPACITY (Task 8/10). PUBLIC (unlike
        // most of this node's internals) for the same reason kFrameCbMaxBytes
        // is: a device-less [nri] case (RenderGraphTest.cpp) recomputes
        // PoolSizes()'s expectations from first principles rather than
        // copying the implementation, and needs a real symbol to do that
        // with rather than a second hardcoded 256. A slice-one FIXED size --
        // BindlessTable never resizes (BindlessTable.hpp's SLOT POLICY) --
        // chosen well above what any scene in this arc names (the four-cube
        // proof uses four slots) and above the spec's cited eventual high-
        // water mark (T3's 144-entry box-projected cubemap array, spec
        // section 6). MUST EQUAL mesh.hlsl's own kMeshBindlessCapacity
        // literal EXACTLY: CreateBindings() sizes the t0/space2 descriptor
        // range to this number, and the shader declares its Texture2D array
        // with the matching literal -- HLSL cannot include this header, so
        // that one comparison has no compiler behind it; this comment (and
        // its mirror in mesh.hlsl) is the whole of that contract.
        static constexpr std::uint32_t kBindlessCapacity = 256;

        // The arena's region stride on a device whose
        // deviceDesc.memoryAlignment.constantBufferOffset is
        // `constantBufferAlignment`. PURE and public for the same reason
        // Batch2DNode::CbRegionStride is: it carries an invariant whose
        // violation would be silent. The result must be BOTH a multiple of the
        // device's alignment AND at least kFrameCbMaxBytes; rounding UP
        // satisfies both for any power-of-two alignment.
        [[nodiscard]] static constexpr std::uint64_t CbRegionStride(
            std::uint64_t constantBufferAlignment) noexcept
        {
            return constantBufferAlignment <= 1
                 ? kFrameCbMaxBytes
                 : ((kFrameCbMaxBytes + constantBufferAlignment - 1) / constantBufferAlignment)
                       * constantBufferAlignment;
        }

        // Byte offset of one frame slot's constant-buffer region. PURE, static
        // and public so the [nri] cases can prove the property no device can
        // show: distinct frame slots never alias, and every region starts on a
        // multiple of `regionStride`.
        [[nodiscard]] static constexpr std::uint64_t CbRegionOffset(
            std::uint64_t regionStride, std::uint32_t frameSlot) noexcept
        {
            return (std::uint64_t)frameSlot * regionStride;
        }

        // THE DESCRIPTOR POOL'S CAPACITY. PUBLIC and separated from the
        // creation call for the reason Batch2DNode::PoolSizes is: a pool's
        // sizes are fixed at creation and NRI cannot free a single set, so a
        // capacity that does not cover what the node allocates is not a
        // compile error and not a wrong pixel -- it is an
        // AllocateDescriptorSets failure part-way through Create at the
        // desk. TWO dimensions now (Task 8/10): kSwapchainFramesInFlight
        // frame-CB sets (one CONSTANT_BUFFER descriptor each) plus ONE
        // bindless set (kBindlessCapacity TEXTURE descriptors, MeshNode.cpp).
        // The root sampler consumes NO pool budget at all -- RootSamplerDesc
        // is "not allocated from a descriptor pool" (NRIDescs.h:1077).
        [[nodiscard]] static nri::DescriptorPoolDesc PoolSizes() noexcept;

    private:
        MeshNode() = default;

        bool Init(NriGraphContext& context);
        // Creates the BindlessTable (gated on Caps().SupportsBindless() by
        // Init(), before this runs). Called before CreateBindings(), which
        // sizes the bindless descriptor range off kBindlessCapacity alone
        // (a compile-time constant) rather than this table, so ordering
        // between the two is not otherwise load-bearing.
        bool CreateBindless();
        bool CreateBindings();
        bool CreateConstantArena();
        // Allocates the per-frame-slot descriptor sets (one b1 CONSTANT_
        // BUFFER range each, written once) and the ONE bindless descriptor
        // set (its range left UNWRITTEN here -- AddMaterial writes it
        // incrementally, and CreateBindings' PARTIALLY_BOUND flag is what
        // makes allocating it with an empty range legal). Written ONCE, at
        // Create, and never again for the per-frame sets -- which is what
        // keeps ResetDescriptorPool and its fence discipline out of this
        // node entirely.
        bool CreateSets();

        // The opaque pipeline for this frame's attachment formats, from the
        // shared cache. Null (already logged) if the cache refused it.
        [[nodiscard]] nri::Pipeline* PipelineFor(nri::Format canvasFormat);

        // How many distinct mesh guids m_residents reserves room for. Prepare
        // builds that table once per frame so a scene of twenty cubes resolves
        // once and draws twenty times; a RESERVED MEMBER rather than a local, so
        // the steady state allocates nothing inside the declaration window. Past
        // this many distinct meshes the vector grows once per high-water mark.
        static constexpr std::size_t kInitialResidentSlots = 16;

        [[nodiscard]] std::uint64_t ArenaOffset(std::uint32_t frameSlot) const
        {
            return CbRegionOffset(m_arenaStride, frameSlot);
        }

        // NriPipelineCache::GraphicsKey::shaderPairId is opaque to the cache
        // and is the CALLER's discriminator for everything the key does not
        // carry (that class's fill-contract rule 3) -- here the vertex input,
        // the rasterizer state and the depth TEST state, none of which are
        // keyed. One shared cache, so the node id spaces must not overlap:
        // Batch2DNode is 0x2000..0x2002, TonemapNode 0x3000, the outline chain
        // 0x4000..0x4002, PickNode 0x4100.
        static constexpr std::uint64_t kShaderPairId = 0x5000;

        NriDevice*        m_device       = nullptr;
        NriPipelineCache* m_pipelines    = nullptr;
        // NO m_owner BACK-POINTER, deliberately: everything this node needs
        // from the vehicle (device, pipeline cache, shader bytecode) is taken
        // at Create, and the two per-frame values it reads -- the canvas format
        // and the frame slot -- arrive as Record/Prepare parameters.
        // Batch2DNode carries one that nothing reads; this does not copy it.
        //
        // NO NriTextureCache POINTER, still: this node's own images are its
        // BindlessTable's SRVs, Added (and therefore owned/resolved) by
        // whoever calls AddMaterial -- a test, or Task 11's scene resolver,
        // which is the consumer of the shared residency cache, not this
        // node. AddMaterial only needs the already-resolved nri::Descriptor*.

        // Bytecode is OWNED BY THE VEHICLE (NriGraphContext's bin cache) and
        // outlives this node -- which the pipeline cache's fill contract
        // (rule 2) requires, since CreateGraphicsPipeline runs after the fill
        // callback returns.
        std::span<const std::uint8_t> m_vs, m_ps;

        // THE BINDLESS MATERIAL TABLE (Task 8/10) and the ONE descriptor set
        // its array lives in -- allocated once, at Create, and written
        // INCREMENTALLY by AddMaterial (contrast m_sets below, written once
        // and never again). Null/kInvalidLayout-shaped until CreateBindless/
        // CreateSets succeed; Create() refuses the whole node rather than
        // leave either half-built (Init()'s `&&` chain).
        std::unique_ptr<BindlessTable> m_bindless;
        nri::DescriptorSet*            m_bindlessSet = nullptr;

        nri::DescriptorPool* m_pool = nullptr;
        std::uint32_t        m_layoutId = NriPipelineCache::kInvalidLayout;

        // The per-frame-slot b1 arena: ONE HOST_UPLOAD buffer, persistently
        // mapped, carved into kSwapchainFramesInFlight regions.
        nri::Buffer*     m_arena       = nullptr;
        void*            m_arenaCpu    = nullptr;
        std::uint64_t    m_arenaStride = 0;
        nri::Descriptor* m_frameCbView[kSwapchainFramesInFlight]{};

        // THE PER-FRAME descriptor sets, one per frame slot. Each binds only
        // that slot's b1 region now (Task 8/10 moved the white texel/sampler
        // out -- see m_bindless/m_bindlessSet above and the root sampler in
        // CreateBindings), written ONCE at Create and never again. One
        // dimension only (the frame slot), because b1 is the only thing in
        // a set that differs frame to frame.
        nri::DescriptorSet* m_sets[kSwapchainFramesInFlight]{};

        // MEMBERS, not locals, and that is load-bearing:
        // nri::GraphicsPipelineDesc::vertexInput is a POINTER into caller
        // memory that CreateGraphicsPipeline dereferences AFTER the fill
        // callback has returned (NriPipelineCache.hpp, fill-contract rule 2).
        nri::VertexAttributeDesc m_attributes[3]{};
        nri::VertexStreamDesc    m_stream{};
        nri::VertexInputDesc     m_vertexInput{};

        // Resolved by Prepare for the canvas format of the frame being
        // declared, so Record never reaches the pipeline cache from inside an
        // open command buffer. Owned by the cache; borrowed here.
        nri::Pipeline* m_pipeline = nullptr;

        // This frame's distinct-guid residency -- filled by Prepare, looked
        // up by Record. Never resolved at record time.
        //
        // BORROWED POINTERS INTO THE CACHE'S MAP, so the table is cleared at BOTH
        // ends of its life: at the top of Prepare (it is about to be rebuilt) and at
        // the bottom of Record (its last reader is done). Without the second clear,
        // an InvalidateMeshGeometry landing between Record(N) and Prepare(N+1) --
        // which erases the map node -- would leave dangling pointers sitting in a
        // live member. Nothing dereferences them today; clearing costs nothing and
        // removes the trap rather than documenting it.
        std::vector<std::pair<Guid, const NriMeshBufferCache::Resident*>> m_residents;

        // One WARN/ERROR each, not one per instance per frame, for the
        // degradations a reader must be able to see.
        bool m_warnedNoPipeline    = false;
        bool m_warnedBadCamera     = false;
    };

    // Declares the opaque mesh node into `graph` and hands back the
    // D32_SFLOAT depth transient it created and attached.
    //
    // THE DEPTH TARGET IS MINTED HERE, in this node's own Setup -- the same
    // create-then-write-then-attach shape AddBatch2DNode uses for the canvas,
    // and the reason Task 4's placeholder "depth" node is not declared on a
    // frame that carries a mesh scene. A transient can only be minted from a
    // RenderGraphBuilder, and a builder exists only inside a node's Setup, so
    // the node that consumes it is the natural one to create it.
    //
    // `canvas` is an INPUT (unlike AddBatch2DNode's, which is an output):
    // batch2d already minted and cleared it, and this pass draws on top.
    //
    // `canvasFormat` IS A PARAMETER AND NOT AN ASSUMPTION. NRI bakes attachment
    // formats into a graphics pipeline, so binding one inside a
    // CmdBeginRendering whose colour attachment carries a different format is
    // undefined on both backends -- and RenderGraph exposes no way to read a
    // handle's format back, so this function cannot derive it. It must be the
    // format `canvas` was CREATED with; the caller that minted the handle is
    // the one that knows. (It was hardcoded to kGraphCanvasFormat until Task
    // 7's first fix round, which made a differently-formatted canvas a silent
    // mismatch with no diagnostic.)
    //
    // `context` is a POINTER because the device-less [nri] frame-shape cases drive
    // the real declarations with no device: with a null context every
    // declaration is identical and the exec fn does nothing, which is exactly
    // what makes those cases able to fail when this function's DECLARATIONS
    // change.
    //
    // `scene` is BORROWED at declaration time and its SPAN is copied into the
    // exec fn -- see MeshSceneDesc::instances for the lifetime rule.
    ARCANE_API RgTexture AddMeshNode(RenderGraph& graph, NriGraphContext* context,
                                      RgTexture canvas, nri::Format canvasFormat,
                                      const MeshSceneDesc& scene,
                                      std::uint32_t width, std::uint32_t height);
}
