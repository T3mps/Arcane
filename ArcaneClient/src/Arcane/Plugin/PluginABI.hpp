#pragma once

// The ONLY unmangled C surface in the engine. The game DLL exports the seven
// GamePlugin_* functions; the host resolves them by name into a PluginVTable.
// EngineContext is the C++ facade handed to the plugin (Arcane::Runtime).

#include <Arcane/Base/Api.hpp>

#include <cstdint>

namespace Astra { class TypeContext; class BinaryWriter; class BinaryReader; }
namespace Mosaic { struct IWorkScheduler; }   // the shared data-parallel seam (Astra aliases this)

namespace Arcane
{
    class Runtime;  // defined in Arcane.dll; the plugin holds it opaquely via EngineContext
    struct ITaskExecutor;  // <Arcane/Jobs/TaskExecutor.hpp>; same enki pool, worker-index face

    // Bump on ANY change to EngineContext layout or the entry-point set/signatures.
    // v2 (2026-06-20): added the ImGui cross-DLL handoff fields below + GamePlugin_DrawUI.
    // v3 (2026-06-26): added taskExecutor (the ITaskExecutor face of the engine scheduler).
    // v4 (2026-07-10): foundations branch changed the cross-DLL C++ surface plugins
    //     consume through EngineContext: Runtime::SnapshotRegistry now returns
    //     Astra::Result (E02-4), Batcher2D gained the pure virtual RemoveTexture
    //     mid-vtable (E01-2), and Assets gained SetDevice before GetTexture. Any of
    //     these mismatched across the boundary is a vtable/stack smash; the version
    //     gate must reject a cross-build pairing.
    // v5 (2026-07-19): the re-vendored Astra migrated its threading seam to
    //     Mosaic, so workScheduler is now Mosaic::IWorkScheduler (per-lane worker id
    //     + FunctionRef callback) -- a DIFFERENT vtable than the prior Astra-native
    //     IWorkScheduler. A stale plugin would vtable-mismatch; reject the pairing.
    // v6 (2026-07-24): shader-editor Slice 8 changed the cross-DLL surface twice:
    //     Batcher2D gained RegisterMaterial/UpdateMaterial/SetGlobals/QuadMaterial
    //     mid-vtable, and SpriteRenderer grew a Guid `material` field (+16 bytes;
    //     plugins compile the header-only RenderSubmissionSystem + components).
    //     A stale plugin's SetLayer call lands on UpdateMaterial and its Quad on
    //     SetGlobals -- observed as an AV on project open; reject the pairing.
    // v7 (2026-07-24): LocalTransform renamed to Transform. The reflected type
    //     name IS the cross-module component identity (TypeID name hash +
    //     name-keyed scene JSON), so a stale plugin would register/query the
    //     old name and silently diverge from engine systems; reject the pairing.
    //     NOTE (2026-07-25, post arc slices 2+3, deliberately NO bump):
    //     OffscreenCanvas gained SetPostChain/SetPostGlobals APPENDED at the
    //     END of its vtable (every pre-existing slot index unchanged; only
    //     hosts -- in-tree, rebuilt with the engine -- call the new slots),
    //     and PostProcess is an ADDED component type (name-keyed identity; a
    //     stale plugin's roster simply lacks it -- typed views come back
    //     empty, nothing smashes). Neither changes EngineContext, the entry
    //     points, or any existing layout, so a v7<->v7 cross-build pairing
    //     stays memory-safe in both directions.
    //     Outliner slice 1 rides the same argument: Identity + Hidden are
    //     two more appended name-keyed component types, and the Not<Hidden>
    //     filter in the plugin-compiled RenderSubmissionSystem is behavioral
    //     (a stale plugin just doesn't honor hiding). Still NO bump.
    // v8 (2026-07-28): the sprite-asset arc re-shaped SpriteRenderer's LAYOUT --
    //     textureId (uint32) + size (vec2) removed, a Guid `sprite` added first --
    //     so tint/sortingLayer/orderInLayer/shape/material all sit at new
    //     offsets, and TextureTable was deleted from SceneResources.hpp. Plugins
    //     compile both headers themselves (components + the header-only
    //     RenderSubmissionSystem), which is the same change class the v6 entry
    //     above (:34-35) bumped for. Unlike v6 the failure is not a vtable slide
    //     but the plugin's own copy of submission reading the component at stale
    //     offsets: tint moved 12 -> 16 and sortingLayer 28 -> 32, so a stale tint
    //     read at 12 comes out of the tail of the new `sprite` Guid plus the
    //     first three tint floats, and a stale sortingLayer read at 28 comes out
    //     of tint.w. It also resolves a TextureTable resource the engine no
    //     longer registers. Corruption rather than a guaranteed AV, and silent
    //     either way; reject the pairing.
    // v9 (2026-07-29): Astra re-vendored to dev f8c9998 -- 40 of 60 shared
    //     headers changed, 8 added, 1 removed (Archetype/ArchetypeGraph.hpp).
    //     Same reasoning as the v5 entry above (:28-31), which bumped for a
    //     previous Astra re-vendor: plugins compile Astra's headers THEMSELVES
    //     (Registry, ComponentRegistry, Archetype storage, EntityTable,
    //     Serialization's Binary{Reader,Writer} -- the very types crossing the
    //     boundary through EngineContext::typeContext and the SaveState/
    //     LoadState entry points), so a plugin built against the old headers
    //     and a host built against the new ones disagree on layout and on
    //     TypeContext identity rules. Concretely observable: the new
    //     TypeContext REFUSES a type whose unqualified name-hash collides with
    //     an already-registered one (it used to alias them silently), handing
    //     back an INVALID ComponentID that then overflows the component bitmap
    //     -- an assert in a checked build, a wrong-component read in a
    //     shipping one. Reject the pairing.
    // v10 (2026-08-09): Astra re-vendored to dev f8a75a9 -- the ComponentModule
    //     program. ReRegisterComponent NO LONGER EXISTS (plugins own their types
    //     via RAII ComponentModule handles, heap-held + reset in Shutdown), and
    //     ComponentRegistry grew owner/shadow/meta-thunk state, so a v9 plugin's
    //     inlined template code manipulates a registry whose layout changed.
    //     Layout mismatch plus a removed API: reject the pairing.
    // v11 (2026-08-14): Batcher2D grew a virtual -- Batch2DDrained Drain(), the
    //     read interface Batch2DNode consumes instead of
    //     End(). Plugins compile their own copy of Batcher2D.hpp (through
    //     SceneResources.hpp's RenderContext2D, which the header-only
    //     RenderSubmissionSystem calls) and dispatch through this vtable, so a
    //     module built against a Batcher2D with a different virtual set is the
    //     same failure class as the v6 entry above (:34-35). Drain() is declared
    //     LAST specifically so no EXISTING slot moves -- a v10 module would in
    //     fact still dispatch correctly -- but "correct by luck" is not what the
    //     gate is for: the header a v10 module baked in has no Drain() at all,
    //     so anything that calls it through a plugin-provided Batcher2D
    //     implementation (a test double, a future editor batcher) hits a slot
    //     that module never emitted. Reject the pairing; the failure mode of
    //     NOT rejecting it is silent.
    // v12 (2026-08-14): Batcher2D grew a second appended virtual --
    //     `const Material2DDesc* MaterialDesc(uint16_t)`, the id -> registration
    //     lookup the render path needs to build a REGISTERED material's own
    //     pipeline and bindings. Two data layouts moved with
    //     it and each is independently sufficient to require this bump:
    //     Material2DDesc gained the retained shader BLOBS (vsBytes/psBytes) and
    //     Batch2DDrained gained `globals`. Both are passed BY VALUE across the
    //     Batcher2D vtable (RegisterMaterial takes a Material2DDesc; Drain
    //     returns a Batch2DDrained), so a module compiled against the v11
    //     layouts and a host compiled against these disagree about object size
    //     -- which corrupts the stack rather than merely misdispatching. Same
    //     verdict as v11, with a harder reason.
    // v13 (2026-08-14): THE SEVERANCE -- the frame's data-supply side stops
    //     depending on an NVRHI device, and three plugin-compiled surfaces
    //     move with it. ONE bump covers all three, deliberately:
    //       * Batcher2D grew a THIRD appended virtual, `QuadTextured(uint16_t,
    //         const Guid&, ...)` -- QuadMaterial plus the image ASSET's Guid.
    //         Appended, so no existing slot moves; the v11 entry above (:96-103)
    //         states why "correct by luck" is still refused.
    //       * Batch2DDrawSpan gained `Guid textureId` (+16 bytes) and it is
    //         returned BY VALUE across the vtable inside Batch2DDrained (a span
    //         over an array of them, whose ELEMENT STRIDE just changed), so a
    //         v12 module iterating a v13 host's drained spans reads every field
    //         of every span after the first at the wrong offset. Same failure
    //         class as v12's Material2DDesc growth, one indirection deeper.
    //       * SpriteEntry (Scene/SceneResources.hpp) gained `Guid textureId`,
    //         and the header-only RenderSubmissionSystem the plugin compiles
    //         ITSELF now reads it and calls QuadTextured. A v12 module's copy
    //         of that system reads a SpriteEntry one size smaller out of a map
    //         the host filled -- every entry past the first at a stale offset.
    //     Why any of this: no NVRHI device exists, so
    //     Assets::GetTexture yields null for every sprite and the
    //     `nvrhi::ITexture*` in a span can no longer tell "untextured" from
    //     "textured on a device that is not there". The Guid can, and the NRI
    //     recorder resolves it through the shared NriTextureCache. Reject the
    //     pairing.
    // v14 (2026-08-19): TWO independent reasons, and the SECOND is the
    //     stronger one -- it gates a layout change that had been sitting in
    //     the tree UNGATED.
    //       * THE RENDER SURFACE NO LONGER EXPOSES NVRHI TYPES. Runtime lost
    //         SetRenderResources(nvrhi::IDevice*, ShaderLibrary*), Device() and
    //         Shaders() outright (Base/Runtime.hpp). Both hosts had passed
    //         (nullptr, nullptr) by then and both accessors could only
    //         return null, so nothing observable changes -- but Runtime is a
    //         concrete class a plugin links against, and removing three exported
    //         members changes what a module built against v13 resolves at load.
    //       * MATERIAL2DDESC SHRANK AT ABI 13 AND WAS NEVER GATED: `vs`,
    //         `ps` and `paramTextures` were deleted from Material2DDesc.
    //         That struct crosses the plugin boundary BY VALUE:
    //         Runtime::SetRenderContext(Batcher2D*) hands the plugin a batcher,
    //         plugins compile their OWN copy of Batcher2D.hpp (:92 above), and
    //         RegisterMaterial(Material2DDesc)/UpdateMaterial are by-value
    //         vtable slots. This is EXACTLY the class of change v12 was bumped
    //         for ("Material2DDesc gained the retained shader BLOBS", :105-111)
    //         and v13 ("same class as v12's growth, one indirection deeper",
    //         :125-129) -- a host and a module that disagree about the size of a
    //         by-value argument corrupt the stack rather than merely
    //         misdispatching. Until this bump a module built before that
    //         shrink loaded into the current engine with NO gate at all: the
    //         v12/v13 failure class with its safety catch disarmed. Closed
    //         here.
    //     WHAT v14 DOES NOT COVER -- read this before assuming Batcher2D is
    //     clean: Batcher2D.hpp still carries NVRHI in nine places, including
    //     `Batch2DDrawSpan::texture` (an nvrhi::ITexture* returned BY VALUE
    //     inside Batch2DDrained) and the Begin/End/Quad/Glyph/RemoveTexture
    //     recorder virtuals. That whole half is dead (every production
    //     Batcher2D::Create passes (nullptr, nullptr); End() has no caller
    //     outside SeveranceTest), but it cannot be removed here: the internal
    //     run list IS Batch2DDrawSpan (`using BatchRun = Batch2DDrawSpan`), so
    //     the field and the recorder go together, and the recorder's signatures
    //     reach into Text/TextSystem and ShaderLibrary. Those go in a later
    //     bump, and they will need one. Reject the pairing.
    //     (Both named files are gone as of v15; the entry was accurate when
    //     it was written.)
    // v15 (2026-08-19): exactly the bump v14's closing paragraph above owed.
    //     NVRHI LEAVES THE PLUGIN SURFACE
    //     ENTIRELY: no header a game module compiles includes <nvrhi/...> any
    //     more, which is what lets Task 10 delete ThirdParty/nvrhi. Three
    //     independent reasons, each on its own sufficient:
    //       * BATCH2DDRAWSPAN SHRANK. `nvrhi::ITexture* texture` is deleted
    //         from the MIDDLE of the struct, so `firstIndex`/`indexCount`/
    //         `textureId` all move. It is returned BY VALUE across the vtable
    //         inside Batch2DDrained (a span over an array of them, whose
    //         ELEMENT STRIDE changed again), so a v14 module reads every field
    //         of every span after the first at the wrong offset. Precisely the
    //         v13 failure class (:124-129), running the other direction.
    //       * SPRITEENTRY SHRANK, AND AT ITS FIRST FIELD. `nvrhi::ITexture*
    //         texture` was SpriteEntry's leading member (Scene/
    //         SceneResources.hpp); removing it moves every remaining field,
    //         and the header-only RenderSubmissionSystem the plugin compiles
    //         ITSELF reads them out of a map the host filled. Same reasoning
    //         as the v13 entry's third bullet (:130-134), inverted.
    //       * THE BATCHER2D VTABLE CHANGED SHAPE, NOT JUST ITS SIGNATURES.
    //         Five virtuals lost NVRHI parameters (Begin, Quad, QuadMaterial,
    //         Glyph, QuadTextured), Create's static factory beside them
    //         changed signature too (never a vtable slot), and
    //         `RemoveTexture(nvrhi::ITexture*)` was REMOVED FROM THE
    //         MIDDLE of the class -- which slides Stats/Drain/MaterialDesc/
    //         QuadTextured up one slot each. That is the exact hazard the v11
    //         entry (:96-103) explains and Drain()'s "declared last" comment
    //         guards against; it is done here deliberately, and it is safe
    //         ONLY because this gate refuses every pre-v15 module outright.
    //     Assets also lost SetDevice, both GetTexture overloads and the three
    //     NVRHI free functions (LoadDisplayTexture/ReadTexturePixels/
    //     SaveTexturePng), and ShaderLibrary was deleted outright -- all
    //     callerless, none of them by-value plugin surface, so they add
    //     nothing to the verdict above. NOTHING SHIPPED between v14 and v15,
    //     so no compatibility shim is owed. Reject the pairing.
    // v16 (2026-08-21): Transform went 3D. position/scale widened from
    //     glm::vec2 to glm::vec3, rotation from a float (radians) to a
    //     glm::quat, and ToMatrix from mat3 to mat4; WorldTransform::matrix and
    //     PreviousTransform moved with them.
    //     This is the class of change the v7 entry above (:38-42) established --
    //     these are name-keyed reflected components that a plugin COMPILES
    //     ITSELF from this header set -- except that where v7 changed the
    //     component's identity, v16 changes its LAYOUT: Transform grows
    //     20 -> 40 bytes with every field at a new offset -- vec2 + float + vec2
    //     = 8 + 4 + 8, all align-4 and unpadded, becomes vec3 + quat + vec3 =
    //     12 + 16 + 12 (glm's quat is packed_highp here, align 4, not the
    //     16-byte-aligned SIMD flavour) -- and WorldTransform 36 -> 64
    //     (mat3 -> mat4). That is the same hazard as the v8 SpriteRenderer
    //     re-shape (:55-62), on the type EVERY spatial entity carries.
    //     The header-only systems a plugin instantiates moved with the types:
    //     a v15 copy of RenderSubmissionSystem reads a sprite's world
    //     translation from matrix[2], which in a mat4 is the Z BASIS AXIS, so
    //     every sprite it submits would draw at the origin; a v15
    //     TransformPropagationSystem would compose mat3s over mat4 storage.
    //     Reject the pairing.
    //     Edit::WorldMatrix/ParentWorldMatrix and DecomposeTRS/ComposeTRS
    //     (ARCANE_API, mat3 -> mat4) also changed signature, but a mangled-name
    //     mismatch fails loudly at load; it adds nothing to the verdict above.
    // v16, amended (2026-08-22, F1 Task 4): the transform PROPAGATION was
    //     replaced -- TransformSystems.hpp gained a public TransformOrder
    //     (a Registry resource holding a flat topological order + dirty state)
    //     and TransformPropagationSystem stopped walking
    //     Relations::ForEachDescendant. The version STAYS AT 16 deliberately:
    //     no existing type's layout moved, no ARCANE_API signature changed, and
    //     TransformOrder is new, so a module built against the earlier v16
    //     header cannot reference it. Both copies of the header-only system
    //     compute the same `parentWorld * local` product and this system is the
    //     sole writer of WorldTransform::matrix, so a mixed pairing is
    //     redundant, not wrong -- which is the whole reason it does not need a
    //     rejection.
    //     Say what that costs, though: a module carrying the earlier v16 header
    //     REGISTERS ITS OWN copy of the system (ReferenceGame.cpp does exactly
    //     this into fixedUpdate), so it reintroduces the two per-frame
    //     ForEachDescendant traversals inside its own scheduler. Task 4's
    //     headline property -- a steady frame walks no descendants -- is
    //     therefore HOST-SIDE ONLY under such a pairing, and is only a
    //     whole-process property once the module is rebuilt against this
    //     header. Accept the pairing; rebuild the module to get the win.
    // v16, amended (2026-08-22, F1 final review): two additive changes, and the
    //     version STAYS AT 16 for both.
    //       * `ARCANE_API bool IsPlanarBasis(const glm::mat4&)` joins
    //         DecomposeTRS/ComposeTRS in Edit/Gizmo.hpp. A NEW export: no
    //         existing signature moved, and a module built against the earlier
    //         v16 header cannot reference a symbol it has no declaration for.
    //       * `Arcane::Collider2D::fixtures` gained Serializable(false). That is
    //         a reflection ATTRIBUTE, not layout -- the struct is byte-identical
    //         and the binary Serialize(Archive&) path does not consult it. A
    //         mixed pairing disagrees only about whether the name-keyed JSON
    //         walk visits a field that neither side could read or write anyway
    //         (it has no container branch), which is why this could brick a
    //         scene rather than corrupt one. Accept the pairing.
    //     THE SCENE FILE FORMAT DID MOVE, though, and it has its own number:
    //     Scene::kSceneJsonVersion went 2 -> 3 for the 3D Transform's on-disk
    //     shape, which the v16 entry above describes and which nothing had
    //     stamped until now. That gate is enforced per FILE (SceneAsset.hpp)
    //     and per CLIPBOARD PAYLOAD (Edit::InstantiateSubtrees) -- both HOST
    //     paths, so it is not a plugin-pairing question today (no module in the
    //     tree loads a scene itself). It is recorded here because every
    //     .arcscene beside a v16 module has to carry 3, the same way the
    //     .arcproj beside it has to carry 16: ReferenceProject and Gacha's Game
    //     were both restamped with this change.
    // v17 (2026-08-22, F2a): the mesh vocabulary lands -- MeshRenderer, the
    //     .arcmesh asset, and the caches/resources/exports that resolve one
    //     into the other. Checked against the SAME "does this actually
    //     corrupt a mixed pairing" bar the v7 and v16-amended entries above
    //     apply, and on that bar every individual piece is additive:
    //       * `MeshRenderer` (Scene/Components.hpp) is a NEW name-keyed
    //         reflected component -- the v7 PostProcess/Identity/Hidden case
    //         (:51-54 above): a stale plugin's roster simply lacks it, typed
    //         views come back empty, nothing smashes.
    //       * `MeshTable`/`MeshMaterialTable` (Scene/SceneResources.hpp) are
    //         NEW registry resources, published through TWO NEW methods
    //         APPENDED to Runtime -- `SetMeshTable`/`SetMeshMaterials`,
    //         siblings of `SetSpriteTable`/`SetSpriteMaterials`
    //         (Base/Runtime.hpp:162,169). `MeshTable`'s value type,
    //         `MeshEntry` (geometry + bounds + a `material` Guid copied off
    //         the loaded .arcmesh), crosses too, not just the resource
    //         wrapping it: MeshSubmissionSystem.hpp is HEADER-ONLY and
    //         plugin-compiled, and reads `MeshEntry` straight off the
    //         resolved table (Scene/MeshSubmissionSystem.hpp:113), including
    //         `MeshEntry::material` for the override -> mesh-default
    //         fallback (:58) -- the v8/v15 class of plugin-crossing surface,
    //         additive here only because the type is brand new. Runtime
    //         crosses the boundary by POINTER (EngineContext::engine) and,
    //         unlike Batcher2D (v6/v11/v12/v13 above), declares no virtual
    //         anywhere -- no vtable, so there is no slot for an appended
    //         member to slide (contrast the v14 entry's "concrete class a
    //         plugin links against" warning, :143-149, which was about
    //         REMOVING members, not adding one).
    //       * `MaterialSurface` (Material/MaterialSource.hpp) gained a THIRD
    //         value, `Mesh`, APPENDED after `Sprite` -- Fullscreen and Sprite
    //         keep their existing integer values, so nothing that already
    //         switches on this enum reads a different case.
    //       * Everything else named below -- `MeshCache`, `MeshMaterialCache`,
    //         `ComputeMeshBounds`, `BuildPlane`/`BuildCylinder`/
    //         `BuildCapsule`, `LoadMaterialParentChain`, `SaveMeshAsset`/
    //         `LoadMeshAsset`/`ValidateMeshAsset`/`BuildMeshData` -- is a
    //         brand-NEW exported symbol; a stale plugin never links against a
    //         symbol it has no declaration for, and MeshCache/
    //         MeshMaterialCache fill the same ENGINE-SIDE per-frame-
    //         resolution role SpriteCache/SpriteMaterialCache already have
    //         (MeshCache.hpp's own header comment), not a plugin-driven one.
    //     So this bump is NOT a corruption finding, and is recorded as such
    //     rather than dressed up as one. It is the same desk-legibility call
    //     the v16 entry's closing paragraph (:274-277) made for scene schema
    //     3 beside ABI 16: a game module now has an entire vocabulary --
    //     MeshRenderer, the .arcmesh round trip (MeshSource/MeshAssetData/
    //     SaveMeshAsset/LoadMeshAsset/ValidateMeshAsset/BuildMeshData), the
    //     "mesh" .arcmat kind, MeshCache/MeshMaterialCache resolving both
    //     into MeshTable/MeshMaterialTable each frame, ComputeMeshBounds, and
    //     the three new unit generators (BuildPlane/BuildCylinder/
    //     BuildCapsule) -- it could not reference before this header, and 17
    //     is the stamp that says a module was built knowing it exists.
    //     ReferenceProject and Gacha's Game are restamped with this change,
    //     the same precedent.
    //     THE SCENE FILE FORMAT DID NOT MOVE: MeshRenderer is additive the
    //     same way PostProcess/Identity/Hidden were (v7 above), so
    //     Scene::kSceneJsonVersion (Serialization/SceneSerializer.hpp:62)
    //     stays at 3.
    // v18 (2026-08-30, arc 2): the automation-tooling close-out moved three
    //     ARCANE_API surfaces. Checked against the same "does this actually
    //     corrupt a mixed pairing" bar the v7, v16-amended and v17 entries
    //     apply -- and unlike v17, TWO of these are genuine LAYOUT changes, so
    //     this one clears that bar rather than resting on desk legibility:
    //       * `HostConfig` (Host/HostConfig.hpp:13, an ARCANE_API STRUCT)
    //         gained `settleTimeoutMs` (uint64). Layout. A v17 module holding
    //         one by value disagrees with the host about its SIZE.
    //       * `VerifyReport` (Host/VerifyReport.hpp:134, an ARCANE_API CLASS)
    //         gained SetSettle plus five members, and its JSON went
    //         schemaVersion 2 -> 3 with `mode` changing value "offscreen" ->
    //         "headless". Layout, and a WIRE contract change besides -- the
    //         version field is what announces it, which is the whole reason a
    //         wire value may move on a bump and not otherwise.
    //       * `Project::Open` and `Runtime::OpenProject` each gained a
    //         DEFAULTED third parameter, `ProjectOpenOptions`. Defaulted is a
    //         source-compatibility convenience only: it changes the MANGLED
    //         NAME either way, so a module built against the v17 header that
    //         referenced either symbol fails IMPORT RESOLUTION at load. That
    //         is loud and immediate, not a silent misread -- but it is exactly
    //         what this gate exists to catch before LoadLibrary rather than
    //         after.
    //     Additive and harmless on their own, recorded so the list is complete:
    //     `ProjectOpenOptions` (Project/ProjectOpenOptions.hpp) and
    //     `SettleBound.hpp` are new headers -- the latter is constexpr-only and
    //     pulls in nothing but <cstdint>; `BootContext` gained a field, but
    //     Host/ProjectBoot.hpp is consumed by the two HOSTS and ArcaneTests and
    //     by no game module.
    //     MEASURED, not assumed: nothing in ReferenceProject's game module
    //     references HostConfig, VerifyReport, ProjectBoot, Project::Open or
    //     OpenProject, so no module in the tree breaks on this bump.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject and Gacha's Game are restamped with this
    //     change, the same precedent v16 and v17 set.
    // v19 (2026-09-03, Arc A -- the automation verdict vocabulary): the two
    //     ARCANE_API surfaces v18 moved MOVED AGAIN, and a third joined them.
    //     Checked against the same "does this actually corrupt a mixed
    //     pairing" bar the v18 entry applies -- all three clear it on layout
    //     or on mangled name, so this one does not rest on desk legibility
    //     either:
    //       * `HostConfig` (Host/HostConfig.hpp:13, an ARCANE_API STRUCT)
    //         gained `std::optional<double> fixedTimeSeconds` -- --fixed-time,
    //         which pins the scene clock independently of the frame count.
    //         LAYOUT: `optional<double>` is 16 bytes, so a v18 module holding
    //         a HostConfig by value disagrees with the host about its SIZE.
    //         Identical failure to the one the v18 entry recorded for
    //         `settleTimeoutMs`.
    //       * `VerifyReport` (Host/VerifyReport.hpp, an ARCANE_API CLASS)
    //         gained `m_compareMaxLocalDifference`, and `SetCompare` gained a
    //         NON-DEFAULTED eleventh parameter (`double maxLocalDifference`).
    //         Both at once: the member is LAYOUT, and the new parameter moves
    //         SetCompare's MANGLED NAME, so a v18 module that called it fails
    //         import resolution at load. The report's JSON went schemaVersion
    //         3 -> 4 with an added `compare.maxLocalDifference`. Unlike the
    //         2 -> 3 move, NO existing field changed meaning -- which is why
    //         `kOldestSupportedSchemaVersion` stays 3 and a 3-era consumer
    //         still reads a v4 report correctly.
    //       * `Arcane::Compare` (Assets/ImageCompare.hpp:186-191, ARCANE_API)
    //         gained a DEFAULTED trailing parameter,
    //         `std::array<std::uint64_t, 100>* localBlocks = nullptr`.
    //         Defaulted is a source-compatibility convenience only -- the same
    //         reasoning the v18 entry gives for `ProjectOpenOptions`: it moves
    //         the mangled name either way, so a v18 module that referenced
    //         this symbol fails import resolution at load. Loud and immediate,
    //         not a silent misread, but still exactly what this gate exists to
    //         catch before LoadLibrary rather than after.
    //     Additive and harmless on their own, recorded so the list is complete:
    //     `Host/Verdict.hpp` and `Host/ExclusionList.hpp` are new ARCANE_API
    //     headers, and `ImageCompareOptions`/`ImageCompareResult` each gained
    //     a member -- all of them consumed by the two HOSTS and ArcaneTests and
    //     by no game module.
    //     MEASURED, not assumed: `grep -rn` for HostConfig, VerifyReport,
    //     ImageCompare, CompareImages, Verdict and ExclusionList over BOTH
    //     game modules in the two trees -- ReferenceProject/Source/
    //     (GameApi.hpp, ReferenceGame.cpp) and Gacha's Game/Source/
    //     (GameApi.hpp, Aphelyon.cpp) -- returns nothing, so no module in
    //     either tree breaks on this bump. The gate catches a stale stamp; the
    //     compiler would not have.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject and Gacha's Game are restamped with this
    //     change, the same precedent v16, v17 and v18 set.
    // v20 (2026-09-03, Arc B -- the host witness harness): the verify-report
    //     surface v19 moved MOVED AGAIN, plus the resolver struct it reads
    //     from. Both clear the v18 entry's "layout or mangled name" bar:
    //       * `ReferenceResolution` (Host/ReferenceImages.hpp, returned BY
    //         VALUE from the exported `ResolveReference`) gained `triedPaths`
    //         (std::vector<std::filesystem::path>) -- the ordered candidate
    //         search space the resolver probed. LAYOUT: a v19 module calling
    //         `ResolveReference` disagrees with the host about the return
    //         struct's SIZE.
    //       * `VerifyReport` (Host/VerifyReport.hpp:134, an ARCANE_API CLASS)
    //         gained `m_compareTriedPaths`, and `SetCompare` gained a
    //         NON-DEFAULTED twelfth parameter
    //         (`std::vector<std::string> triedPaths`). The same pairing the
    //         v19 entry records: the member is LAYOUT, and the new parameter
    //         moves SetCompare's MANGLED NAME, so a v19 module that called it
    //         fails import resolution at load. The report's JSON went
    //         schemaVersion 4 -> 5 with an added `compare.triedPaths`
    //         (deliberately in the REPORT, not the log -- UE logs its tried
    //         list and discards it). NO existing field changed meaning, so
    //         `kOldestSupportedSchemaVersion` stays 3 and a 3-era consumer
    //         still reads a v5 report correctly.
    //     MEASURED, not assumed: `grep -rn` for SetCompare and
    //     ReferenceResolution over BOTH game modules in the two trees --
    //     ReferenceProject/Source/ (GameApi.hpp, ReferenceGame.cpp) and
    //     Gacha's Game/Source/ (GameApi.hpp, Aphelyon.cpp) -- returns
    //     nothing, so no module in either tree breaks on this bump. The gate
    //     catches a stale stamp; the compiler would not have.
    //     THE SCENE FILE FORMAT DID NOT MOVE: Scene::kSceneJsonVersion stays
    //     at 3. ReferenceProject and Gacha's Game are restamped with this
    //     change, the same precedent v16 through v19 set.
    inline constexpr uint32_t kGamePluginABIVersion = 20;

    // The ABI version compiled into the LOADED Arcane.dll -- i.e. the one the
    // plugin gate actually enforces at runtime.
    //
    // kGamePluginABIVersion above is a header constant, so every module bakes in
    // its OWN copy at compile time. A host exe reporting that constant reports
    // what IT was built against, which is not necessarily what the DLL beside it
    // enforces: a partially-updated install (fresh Arcane.dll, stale
    // ArcaneEditor.exe, or the reverse) stamps a number the runtime will reject.
    // Anything PUBLISHING the ABI -- the `--print-engine-info` probe the Arcane
    // Hub stamps into new .arcproj files -- must ask the DLL, not itself.
    ARCANE_API uint32_t PluginABIVersion();

    struct EngineContext
    {
        uint32_t               abiVersion;     // == kGamePluginABIVersion at the host
        Astra::TypeContext*    typeContext;    // plugin calls Astra::SetTypeContext(this) FIRST
        Mosaic::IWorkScheduler* workScheduler; // the one engine enkiTS adapter (shared instance)
        Arcane::ITaskExecutor* taskExecutor;   // SAME enki pool, worker-index ParallelFor (physics/general)
        Arcane::Runtime*       engine;         // registry, schedulers, snapshot/restore, render ctx

        // ImGui cross-DLL handoff (v2). ImGui's globals (GImGui) and heap do not
        // cross the DLL boundary; a plugin that wants to draw ImGui must adopt the
        // host's context + allocators via ImGui::SetCurrentContext / SetAllocatorFunctions
        // in Init. Kept as void* so this ABI header stays imgui-include-free; the
        // plugin reinterpret_casts them to ImGuiContext* / ImGuiMemAllocFunc /
        // ImGuiMemFreeFunc. Null in headless hosts (no ImGuiLayer) -> plugin skips.
        void* imguiContext  = nullptr;   // ImGuiContext*
        void* imguiAlloc    = nullptr;   // ImGuiMemAllocFunc
        void* imguiFree     = nullptr;   // ImGuiMemFreeFunc
        void* imguiUserData = nullptr;
    };

    namespace PluginEntry
    {
        inline constexpr const char* kABIVersion  = "GamePlugin_ABIVersion";
        inline constexpr const char* kInit        = "GamePlugin_Init";
        inline constexpr const char* kShutdown    = "GamePlugin_Shutdown";
        inline constexpr const char* kFixedUpdate = "GamePlugin_FixedUpdate";
        inline constexpr const char* kUpdate      = "GamePlugin_Update";
        inline constexpr const char* kSaveState   = "GamePlugin_SaveState";
        inline constexpr const char* kLoadState   = "GamePlugin_LoadState";
        // v2: the host calls DrawUI between ImGuiLayer::BeginFrame and ::Render --
        // the only window where ImGui draw calls are valid. The plugin's Update runs
        // in the sim phase (before BeginFrame), so it is too early for ImGui.
        inline constexpr const char* kDrawUI      = "GamePlugin_DrawUI";
    }

    // Host-side resolved function-pointer table for one loaded plugin image.
    struct PluginVTable
    {
        uint32_t (*ABIVersion)()                         = nullptr;
        bool     (*Init)(EngineContext*)                 = nullptr;
        void     (*Shutdown)()                           = nullptr;
        void     (*FixedUpdate)(double dt)               = nullptr;
        void     (*Update)(double dt, double alpha)      = nullptr;
        // SaveState is void because BinaryWriter is error-latching; callers check writer.HasError()
        // after the call, mirroring how LoadState signals failure via its bool return.
        void     (*SaveState)(Astra::BinaryWriter&)      = nullptr;
        bool     (*LoadState)(Astra::BinaryReader&)      = nullptr;
        // v2: host calls this between ImGuiLayer BeginFrame and Render (the only valid
        // ImGui draw window). Update is sim-phase -- too early. May be null if a plugin
        // does not export it (resolution is lenient); the host null-checks before calling.
        void     (*DrawUI)()                             = nullptr;
    };
}
