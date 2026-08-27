#pragma once

// MeshDocument: the .arcmesh editor (F2a, Task 9) -- gives the procedural mesh
// asset its own document with a live preview, so a user can author one
// without hand-writing JSON. Two halves, deliberately unequal in weight:
//
//   DATA + UNDO   follows SpriteDocument.{hpp,cpp} almost exactly: a flat
//                 MeshAssetData held by value, dirty tracking, whole-data undo
//                 steps built from an activation-time COPY compared against
//                 the live data (PushDataEdit -- SpriteDocument.cpp:123-134),
//                 and a doc-identity anchor so a step left on the shared
//                 CommandStack after this document closes goes inert instead
//                 of dereferencing a dead `this`.
//   PREVIEW       follows ShaderEditorDocument's offscreen-NriGraphContext
//                 pattern ONLY for that one mechanism (its own small vehicle
//                 over the process's one device, built lazily, retired
//                 through the app's one-frame hand-off) -- NOT its scale,
//                 its multi-panel structure, or its async compile machinery.
//                 A mesh needs none of that: BuildMeshData is synchronous and
//                 pure, so there is no compile step to coalesce and no
//                 in-flight job to route a result back to.
//
// TWO CACHES, AND ONLY ONE OF THEM IS THIS WINDOW'S PREVIEW. The preview
// image reads `m_data` DIRECTLY every rebuild (RebuildPreviewMesh), so there
// is nothing in front of THAT to invalidate: an edit or an undo shows up in
// this window the instant it lands. The SCENE is the other story, and it is
// not this document's preview at all: SceneRenderResolver owns a live
// MeshCache (Host/SceneRenderResolver.cpp) whose published MeshTable both
// hosts sweep every frame through CollectMeshInstances (EditorAppFrame.cpp:
// 1586, RuntimeFrame.cpp:379), and MeshCache::Request memoises per Guid --
// entries leave only via Invalidate/Clear. So a save or an undo that
// invalidates nothing leaves every MeshRenderer in the open scene drawing the
// PRE-edit geometry until the project is switched.
//
// Hence Services::invalidateMesh, called from BOTH Save() and ApplyMeshData()
// -- the same two sites SpriteDocument calls its own invalidateSprite from
// (SpriteDocument.cpp:119-120, :144-145), for the reason stated there: "an
// undo the user cannot see in the scene is indistinguishable from an undo
// that did not happen."
//
// This block used to argue the opposite -- that CollectMeshInstances had no
// call site outside tests and no live cache existed. Both were true when
// Task 9 wrote it and Task 10 falsified both; the note stays so the old
// conclusion is not re-derived from the same (now wrong) premise.
//
// Implements EditorDocument's five pure virtuals (EditorDocument.hpp:15-44);
// DocumentHost owns the open-document list, the asset-type -> factory
// routing, and the unsaved-close confirm modal.

#include "Scene/EditGesture.hpp"
#include "Documents/EditorDocument.hpp"

#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>          // MeshAssetData; also brings MeshBuilder.hpp (MeshData)

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// CommandStack arrives complete via EditGesture.hpp. The preview vehicle is
// forward-declared only -- see the block below for why (same reasoning
// ShaderEditorDocument.hpp states at its own forward-declare block).
namespace Arcane
{
    class CommandStack;
    class Runtime;

    // Forward-declared, never included here: NriGraphContext.hpp pulls
    // <NRI.h> plus every render-graph node header, and this header is
    // included by the whole editor AND source-compiled into ArcaneTests. The
    // unique_ptr member below is legal against an incomplete type because
    // ~MeshDocument is out of line (MeshDocument.cpp).
    struct HostConfig;
    class ImGuiNriNode;
    class NriDevice;
    class NriGraphContext;
}

namespace Arcane::Editor
{
    class MeshDocument final : public EditorDocument
    {
    public:
        // Everything the document borrows from the app. Two shapes glued
        // together: the small "services struct" SpriteDocument::Services
        // uses (runtime + undo), plus the three-borrow preview seam
        // DocServices carries for ShaderEditorDocument (ShaderEditorDocument.
        // hpp:120-172) -- read there for the full mechanism; restated only
        // briefly below.
        struct Services
        {
            // Resolves the material Guid field's DISPLAY name (the current
            // project's registry, mount path for a Guid) -- the only thing
            // this document reads a Runtime for. Null just means the field
            // shows the raw Guid instead of a mount path; the assignment
            // itself (drag-drop from the Asset Browser) needs no Runtime at
            // all.
            Arcane::Runtime* runtime = nullptr;

            // The SAME shared editor CommandStack every other surface pushes
            // to (EditorAppProject.cpp's MakeDocServices hands the identical
            // pointer to DocServices::undo) -- one global history, so a mesh
            // param edit undoes alongside everything else in the order it
            // happened. Null = no undo coverage (the EditGesture bracket
            // no-ops whole); the document still edits and saves.
            Arcane::CommandStack* undo = nullptr;

            // Fired with this asset's Guid after a successful Save AND after
            // every undo/redo apply -- routed to SceneRenderResolver::
            // InvalidateMesh, which evicts the scene's cached resolve and
            // re-reads the saved file synchronously.
            //
            // NOT SPECULATIVE PLUMBING: MeshCache::Request is a once-per-Guid
            // cache and the resolver's MeshTable is what every MeshRenderer in
            // the open scene draws through, so this callback IS the whole
            // mechanism that makes a mesh edit (or an undo of one) visible in
            // the viewport -- exactly what SpriteDocument::Services::
            // invalidateSprite is for its own asset. See the file-top block.
            //
            // Null (the headless tests; any host with no resolver) means no
            // scene invalidation -- the document still edits, previews and
            // saves, so an unwired hook degrades to "the viewport lags the
            // file", never to a lost edit.
            std::function<void(const Arcane::Guid&)> invalidateMesh;

            // ===== THE PREVIEW SEAM ==========================================
            // Borrowed from EditorApp, which outlives the document list.
            //   nriDevice/hostConfig -- what CreateOffscreen needs to build
            //                           this document's own small vehicle
            //                           over the process's one device.
            //   chromeHud            -- the chrome context's ImGuiNriNode,
            //                           owed an InvalidateUserTextureNow
            //                           before this preview's texture dies
            //                           on the no-sink teardown path.
            // All three null in the headless tests (no EditorApp at all) --
            // what keeps EnsurePreviewContext() a single `if`, and what this
            // task's pinned behaviour ("device-less services allocate no
            // preview resources") exercises directly.
            Arcane::NriDevice*        nriDevice  = nullptr;
            const Arcane::HostConfig* hostConfig = nullptr;
            Arcane::ImGuiNriNode*     chromeHud  = nullptr;

            // ===== AND THE ONE-FRAME RETIRE, NOT OPTIONAL =====
            // A document can be destroyed INSIDE the editor's ImGui pass
            // while a still-to-be-recorded chrome frame names this preview's
            // output by raw pointer -- see DocServices::retireGraphPreview
            // (ShaderEditorDocument.hpp:149-172) for the full ordering
            // argument; it applies here unchanged. Null in the headless
            // tests and at shutdown; a document with no sink destroys its
            // vehicle inline (see DestroyPreviewContext).
            std::function<void(std::unique_ptr<Arcane::NriGraphContext>)> retireGraphPreview;
        };

        // `data` is already loaded (LoadMeshAsset happens in the factory,
        // which returns null on failure -- same split as SpriteDocument's
        // ctor / EditorApp.cpp's spriteFactory).
        MeshDocument(Services services, std::filesystem::path path,
                    Arcane::MeshAssetData data);
        ~MeshDocument() override;   // closes a parked edit gesture, retires the preview vehicle

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        bool Dirty() const override { return m_dirty; }
        bool Save() override;
        bool WindowFocused() const override { return m_windowFocused; }
        void Tick(double dt) override;
        void Draw(bool& requestClose) override;

        // Undo plumbing (doc-identity commands, the same shape as
        // SpriteDocument::ApplySpriteData): swap the whole authored data in
        // and rebuild the preview mesh from it, so an undo shows up in the
        // preview rather than living only inside the ImGui form.
        void ApplyMeshData(const Arcane::MeshAssetData& data);

        // One undo step for a COMPLETED field gesture (or a single-frame
        // commit -- the source combo, a material drag-drop/clear): `before`
        // is the pre-edit copy, `after` is m_data as it stands now. Pushes
        // nothing when the two match, so a click that moved no value leaves
        // no step behind.
        void PushDataEdit(std::string label, const Arcane::MeshAssetData& before);

        // The live authored data. Exposed for the headless [editor] units --
        // Draw is the only ImGui method and they never call it, so this is
        // how they observe what a command did.
        const Arcane::MeshAssetData& Data() const noexcept { return m_data; }

        // The CURRENT preview geometry, rebuilt every time m_data changes
        // (construction, ApplyMeshData, or a live field edit in Draw).
        // nullopt exactly when ValidationReason() is set -- BuildMeshData's
        // own contract, restated here so a headless test can observe both
        // halves of "an invalid param set yields no geometry and surfaces
        // the reason" without needing ImGui at all.
        const std::optional<Arcane::MeshData>& PreviewMesh() const noexcept { return m_previewMesh; }

        // nullopt == the current data is valid. Otherwise ValidateMeshAsset's
        // human-readable reason, naming the offending field -- what the panel
        // shows in place of the preview image when a hand-edited file loads
        // outside the widgets' own bounds.
        const std::optional<std::string>& ValidationReason() const noexcept { return m_validationReason; }

        // The preview's offscreen output as an ImGui texture id, and 0 for
        // "nothing to draw" -- no vehicle at all (device-less services, or
        // one that failed to build), same convention as ShaderEditorDocument
        // ::GraphPreviewTextureId. Exposed so the headless units can pin
        // "device-less services allocate no preview resources" without
        // reaching into a private member.
        [[nodiscard]] std::uint64_t PreviewTextureId() const noexcept;

    private:
        // Recompute m_previewMesh/m_validationReason from the CURRENT
        // m_data. Called from the ctor and from every path that mutates
        // m_data (ApplyMeshData, and Draw's live field edits) -- there is no
        // cache in front of the preview, so this is the whole of keeping it
        // in sync; see the file-top comment for why that is deliberately
        // simpler than SpriteDocument's cache-invalidate story.
        void RebuildPreviewMesh();

        // Build this document's own offscreen vehicle, once, the moment
        // Services says a device exists. A no-op when one already exists or
        // when nriDevice/hostConfig is null -- the whole of what makes
        // "device-less services allocate no preview resources" true, and
        // called from the CONSTRUCTOR (not lazily from Tick/Draw) because a
        // mesh preview has no bind/compile event to wait for: the moment
        // Services is known, whether a device exists is already decided.
        void EnsurePreviewContext();

        // Render one frame of the preview: with a valid m_previewMesh, a
        // single instance framed from ComputeMeshBounds so the whole mesh is
        // in view regardless of source/topology. With NO valid preview mesh
        // (an invalid param set), the frame STILL RECORDS -- with an EMPTY
        // instance span, which NriGraphContext's `wantsMesh` gate reads as
        // "no mesh pass this frame" -- so the offscreen texture ends up
        // holding Batch2DNode's plain clear colour rather than whatever
        // undefined bytes CreateOffscreen left it with.
        //
        // Recording unconditionally (rather than returning early on no
        // preview mesh) is Task 9 fix-round Finding 2: OffscreenTextureId()
        // returns the raw texture pointer, non-zero the INSTANT
        // CreateOffscreen succeeds -- long before any frame renders into it
        // (NriGraphContext.cpp:582-588) -- so a Draw() that trusts a non-zero
        // texture id as "there is something to show" was, before this fix,
        // sampling creation-time-undefined contents for every invalid asset.
        // A no-op ONLY when there is no vehicle at all.
        void RenderPreview();

        // Hand the vehicle to the app's one-frame retire (or destroy it
        // inline with the cross-context invalidate when there is no sink) --
        // mirrors ShaderEditorDocument::DestroyGraphPreview exactly; see its
        // comment for the full ordering argument.
        void DestroyPreviewContext();

        Services                 m_services;
        std::filesystem::path    m_path;
        Arcane::MeshAssetData    m_data;
        std::string              m_title;         // display name (Title())
        std::string              m_windowLabel;    // "name (Mesh)###meshdoc_<guid>" (stable across rename)
        bool                     m_dirty = false;  // set by any field edit; cleared only by a SUCCESSFUL Save
        // Latched each Draw; read by DocumentHost::FocusedDoc so the app's
        // scene-level Ctrl+S stands down while this document is focused.
        bool                     m_windowFocused = false;

        // Kept in lockstep with m_data by RebuildPreviewMesh -- never written
        // anywhere else.
        std::optional<Arcane::MeshData> m_previewMesh;
        std::optional<std::string>      m_validationReason;

        // THE PREVIEW IS A STATIC IMAGE. Nothing in RenderPreview reads a
        // clock -- there is no time input anywhere in this document -- so the
        // rendered picture can only change when m_previewMesh does. Without
        // this flag Tick would re-record a 512x512 offscreen graph frame for
        // EVERY open mesh document on EVERY editor frame, because
        // DocumentHost::TickAll ticks them all unconditionally, with no
        // visibility or focus gate (DocumentHost.cpp:152-156) -- a collapsed
        // background tab pays the same as the focused one.
        //
        // ShaderEditorDocument's own per-tick render is NOT a precedent for
        // doing the same here: that preview genuinely animates (its
        // m_animTime += dt, ShaderEditorDocument.cpp:1819), so its picture
        // really is different every frame.
        //
        // Set by RebuildPreviewMesh -- the one place m_previewMesh and
        // m_validationReason ever move -- and cleared only once a frame has
        // actually landed in the texture (FrameOutcome::Presented). True at
        // construction so the opening image records on the first Tick.
        bool m_previewDirty = true;

        // The document's ONE edit-gesture bracket (the ScopeGuard at the top
        // of Draw is its guaranteed close). Every topology drag shares it --
        // same shape as SpriteDocument::m_gesture.
        EditGesture::GestureState m_gesture;

        // Doc-identity handle for undo steps, mirroring SpriteDocument's
        // m_anchor: commands hold this WEAKLY and forward through the
        // pointee, so steps left on the shared stack after this document
        // closes go inert instead of dereferencing a dead `this`.
        std::shared_ptr<MeshDocument*> m_anchor;

        // This document's own offscreen preview vehicle -- null in every
        // device-less test (Services carries no device there) and whenever
        // CreateOffscreen itself refuses (already logged).
        std::unique_ptr<Arcane::NriGraphContext> m_preview;

        // Square, matching ShaderEditorDocument::kGraphPreviewSize -- there is
        // no shape reason for a mesh preview to differ, and reusing the same
        // constant keeps every open-document preview texture the same size
        // class.
        static constexpr std::uint32_t kPreviewSize = 512;
    };
}
