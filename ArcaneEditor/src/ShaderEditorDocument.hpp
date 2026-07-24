#pragma once

// ShaderEditorDocument: the shader editor MVP (Slice 5) -- the first real
// EditorDocument. One window, four panels over one .armat asset:
//   snippet text (InputTextMultiline + click-to-jump via the input callback)
//   live preview (own OffscreenCanvas + FullscreenMaterialPass, animating Time)
//   params (auto-widgets from the //@param decls; edits write the CB LIVE, no
//           recompile, and land on the shared CommandStack as undo steps)
//   errors (structured ShaderDiag list; click jumps the text cursor)
// Live loop: a text edit resubmits both stages through the app-shared
// ShaderCompiler (the service's per-key debounce coalesces keystrokes); the
// LAST-GOOD pipeline keeps rendering while a compile is in flight or failing.
// EditorApp drains the service once per frame and offers each result to every
// document via ConsumeResult.

#include "EditorDocument.hpp"

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/FullscreenMaterialPass.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Arcane
{
    class CommandStack;
    class Runtime;
    class ShaderLibrary;
}

namespace ax::NodeEditor
{
    struct EditorContext;   // imgui-node-editor (opaque; graph canvas, Slice 9)
}

namespace Arcane::Editor
{
    // Everything a document borrows from the app (all outlive the host's
    // document list -- see EditorApp member ordering).
    struct DocServices
    {
        nvrhi::IDevice*               device = nullptr;
        Arcane::ShaderLibrary*        shaders = nullptr;    // OffscreenCanvas tonemap
        Arcane::ShaderCompiler*       compiler = nullptr;   // app-shared service
        Arcane::ShaderSourceProvider* sources = nullptr;    // template text
        Arcane::Runtime*              runtime = nullptr;    // Assets facade + open project (picker)
        Arcane::CommandStack*         undo = nullptr;       // the ONE undo history
        const double*                 clock = nullptr;      // app compile clock (Poll's `now`)
        Arcane::GraphicsBackend       backend{};
        // Fired after a successful Save with the asset's Guid -- the app
        // invalidates the sprite-material cache so scene sprites pick up the
        // SAVED asset (Slice 8; scene sprites never render the working copy).
        std::function<void(const Arcane::Guid&)> onAssetSaved;
    };

    class ShaderEditorDocument final : public EditorDocument
    {
    public:
        ShaderEditorDocument(DocServices services, std::filesystem::path path,
                             Arcane::MaterialAssetData data);
        ~ShaderEditorDocument() override;   // destroys the node-editor context

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        // Text dirtiness is tracked directly; param dirtiness derives from the
        // instance's EffectiveSerial vs the saved baseline, so undo/redo of a
        // param edit flips the verdict correctly (review m1).
        bool Dirty() const override { return m_dirty || ParamsDirty(); }
        bool Save() override;
        void Tick(double dt) override;
        void Draw(bool& requestClose) override;

        // True when the result belonged to this document's in-flight compiles.
        bool ConsumeResult(const Arcane::ShaderCompileResult& result);

        bool IsInstance() const { return m_data.IsInstance(); }
        // Graph-owned (Slice 9): the node canvas is the editing surface and the
        // snippet buffer holds GENERATED text (shown read-only).
        bool IsGraphOwned() const { return m_data.graph.has_value(); }

        // Undo plumbing for graph edits (same doc-identity anchor pattern as
        // ApplyParamEdit): swap in a whole graph state -- nullopt = text-owned
        // (convert-to-text and its redo) -- and regenerate/recompile.
        void ApplyGraphState(std::optional<Arcane::MaterialGraph> state);

        // Parse/chain-resolution errors (what the errors panel shows above the
        // compile diags). Exposed for the headless tests.
        const std::vector<std::string>& ParseErrors() const { return m_parseErrors; }

        // Undo plumbing (doc-identity commands, review M3): apply a param
        // override edit to the CURRENT instance. Undo steps hold the document
        // through m_anchor -- recompiles swap m_instance underneath them, so
        // forwarding by name hash here is what lets history survive rebinds.
        void ApplyParamEdit(std::uint32_t nameHash, bool hasValue,
                            const Arcane::MatParamValue& value);

    private:
        double Now() const { return m_services.clock ? *m_services.clock : 0.0; }
        void   Rebuild();          // parse + stitch + submit both stages (structural edit)
        void   BindIfComplete();   // both stages landed -> createShader + SetMaterial
        bool   HasErrors() const;
        bool   ParamsDirty() const;   // EffectiveSerial vs the saved baseline
        // Instance mode: walk parent -> ... -> base through the project registry
        // (cycle-guarded). Fills m_parentChain ([immediate parent, ..., base]);
        // false (with a parse error) when a hop cannot resolve or load.
        bool ResolveParentChain();
        // The snippet the compile sees: my own (base) or the chain's base's.
        const std::string& SnippetSource() const;

        void DrawToolbar();
        void DrawSnippetEditor(float height);
        void DrawErrorsPanel();
        // Graph mode (Slice 9, imgui-node-editor canvas).
        void RegenerateFromGraph();          // codegen -> snippet -> Rebuild; errors keep last-good
        void DrawGraphPanel(float height);
        void DrawGraphNode(Arcane::GraphNode& node);
        void HandleGraphEdits();             // link create/delete queries (inside Begin/End)
        // One undo step per completed graph gesture: `before` was captured at
        // the gesture start; `after` is the current graph. The live edit
        // already happened (ICommand contract).
        void PushGraphUndo(const char* label, std::optional<Arcane::MaterialGraph> before);
        bool NodeBadged(std::uint32_t nodeId) const;
        void RebuildDiagBadges();            // compile diags -> line map -> node ids
        void DrawPreviewPanel(float height);
        void DrawParamsPanel();
        // True when the ACTIVE surface has something bound to show.
        bool PreviewReady() const;
        // Sprite surface: re-register the preview material's binding data
        // (texture params resolved fresh) without a recompile.
        void RefreshSpritePreviewBinding();
        void DrawTextureParam(const Arcane::ParamDecl& decl,
                              const Arcane::MatParamValue& current);
        void SetParamWithUndo(const Arcane::ParamDecl& decl,
                              const Arcane::MatParamValue& value);

        DocServices                     m_services;
        std::filesystem::path           m_path;
        Arcane::MaterialAssetData       m_data;      // id/name/kind (+ save target)
        std::string                     m_title;     // display name
        std::string                     m_windowLabel;   // display###stable-guid-id
        std::string                     m_snippet;   // the live edit buffer
        bool                            m_dirty = false;  // TEXT dirtiness (params: ParamsDirty)
        bool                            m_live = true;    // auto-compile on edit
        bool                            m_confirmSaveWithErrors = false;

        // Param-dirtiness baseline: the instance serial at the last Save (or
        // rebind carrying no unsaved edits). m_paramsBaseDirty carries unsaved
        // param edits ACROSS an instance swap, whose fresh serial is unrelated.
        std::uint64_t m_savedParamSerial = 0;
        bool          m_paramsBaseDirty = false;

        // Doc-identity handle for undo steps (review M3): commands hold this
        // weakly and forward through the pointee -- the anchor dies with the
        // document (steps go inert), but SURVIVES m_instance swaps.
        std::shared_ptr<ShaderEditorDocument*> m_anchor;

        // Authoring state: bound = what the pass renders (last-good); pending =
        // the latest Rebuild, promoted by BindIfComplete on dual-stage success.
        std::shared_ptr<Arcane::MaterialTemplate>  m_pendingTemplate;
        std::shared_ptr<Arcane::MaterialTemplate>  m_boundTemplate;
        std::shared_ptr<Arcane::MaterialInstance>  m_instance;   // over m_boundTemplate
        std::vector<Arcane::ParamMeta>             m_metas;      // parallel to pending->Params()
        std::vector<Arcane::ParamMeta>             m_boundMetas; // parallel to bound->Params()
        std::vector<std::string>                   m_parseErrors;
        std::vector<Arcane::ShaderDiag>            m_diags;      // last compile (active backend)

        std::unique_ptr<Arcane::OffscreenCanvas>        m_preview;
        std::unique_ptr<Arcane::FullscreenMaterialPass> m_pass;

        // Instance mode (Slice 7): the resolved ancestry, immediate parent first,
        // BASE (the snippet owner) last. Empty for base materials.
        std::vector<Arcane::MaterialAssetData> m_parentChain;
        bool m_showOnlyOverridden = false;   // instance params filter

        // Preview surface (Slice 8): 0 = fullscreen (FullscreenMaterialPass),
        // 1 = sprite (a QuadMaterial on a checkerboard through the preview
        // canvas's own Batcher2D). Initialized from the material's kind;
        // switching it on a base material re-kinds the asset (structural edit).
        int m_surface = 0;
        std::uint16_t       m_previewSpriteMaterial = 0xFFFF;   // Batcher2D id
        nvrhi::ShaderHandle m_previewVs, m_previewPs;           // for binding refresh

        std::uint64_t m_vsJob = 0, m_psJob = 0;      // in-flight ids (0 = none)
        std::vector<std::uint8_t> m_vsBytes, m_psBytes;
        double m_animTime = 0.0;                     // preview Time uniform
        int    m_jumpToLine = 0;                     // 1-based; 0 = no pending jump
        bool   m_focusSnippet = false;               // focus the input so the jump lands
        // Armed jump consumed by the input callback. Per-document (review m2): a
        // shared static could deliver one doc's jump to another on a same-frame
        // focus race.
        int    m_callbackJumpLine = 0;

        // Param-widget gesture capture (before-state on activation, step pushed
        // on release). Per-document (review m3): same-frame keyboard-nav active-
        // ID transfer between documents could cross shared statics.
        bool                  m_gestureHadBefore = false;
        Arcane::MatParamValue m_gestureBefore{};

        // ---- Graph mode (Slice 9) ----
        ax::NodeEditor::EditorContext* m_graphCtx = nullptr;   // lazy; dtor destroys
        std::vector<Arcane::GraphError> m_graphErrors;   // last codegen's errors (badges + panel)
        std::vector<std::uint32_t> m_lineNodeIds;        // last GOOD codegen's line map
        std::vector<std::uint32_t> m_diagBadgeNodes;     // compile-diag nodes via the line map
        // First snippet line's 0-based offset inside the stitched HLSL -- maps
        // compiler diag lines back into snippet space (jump + badges).
        int  m_snippetLineOffset = 0;
        bool m_graphPositionsApplied = false;   // canvas seeded from stored node positions
        bool m_showGeneratedText = false;       // toolbar toggle: canvas <-> read-only HLSL
        std::uint32_t m_focusNode = 0;          // errors-panel click -> select + navigate
        float m_graphPopupX = 0.0f, m_graphPopupY = 0.0f;   // create-menu screen pos
        // Whole-graph gesture capture for value drags (small graphs; the SG
        // full-snapshot-undo pathology was per-edit reserialization + full
        // preview regeneration, neither of which applies here).
        std::optional<Arcane::MaterialGraph> m_graphGestureBefore;
        // In-progress param-name edit (InputText needs a stable buffer while
        // active; committed on deactivate-after-edit).
        std::uint32_t m_nameEditNode = 0;
        char          m_nameBuf[64] = {};
        // Custom-node HLSL body editing: the node shows a plain-text preview
        // (child-window widgets drift inside the canvas); the body edits in a
        // Suspend'ed MODAL. Request set by the node's button, consumed in
        // DrawGraphPanel's popup block; the buffer holds the working copy
        // until Apply commits it as one undo step.
        std::uint32_t m_bodyEditRequest = 0;
        std::uint32_t m_bodyEditNode = 0;
        char          m_bodyBuf[4096] = {};

        friend struct SnippetCallbackForwarder;
    };
}
