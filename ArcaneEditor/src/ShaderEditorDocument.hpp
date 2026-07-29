#pragma once

// ShaderEditorDocument: the shader editor MVP (Slice 5) -- the first real
// EditorDocument. One window, three panels over one .arcmat asset:
//   snippet text (InputTextMultiline; the line-jump seam is dormant -- the
//                 errors panel was its only driver, see m_jumpToLine)
//   live preview (own OffscreenCanvas + FullscreenMaterialPass, animating Time)
//   params (auto-widgets from the //@param decls; edits write the CB LIVE, no
//           recompile, and land on the shared CommandStack as undo steps)
// Diagnostics have NO panel of their own: canvas node badges still mark the
// offending nodes, and the row text is published to the editor Console by
// PublishDiagnostics -- once per CHANGE of the set, not once per compile.
// Live loop: a text edit resubmits both stages through the app-shared
// ShaderCompiler (the service's per-key debounce coalesces keystrokes); the
// LAST-GOOD pipeline keeps rendering while a compile is in flight or failing.
// EditorApp drains the service once per frame and offers each result to every
// document via ConsumeResult.

#include "EditGesture.hpp"
#include "EditorDocument.hpp"
#include "EditorWidgets.hpp"   // TextCommitState / StableTextEdit

#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/FullscreenMaterialChain.hpp>
#include <Arcane/Render/FullscreenMaterialPass.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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
    // Graph-canvas rendering level of detail -- Unreal's EGraphRenderingLOD,
    // ported including its ORDERING, which every gate depends on: the enum runs
    // from "zoomed all the way out" to "zoomed in past 1:1", so GREATER MEANS
    // MORE DETAIL and a degradation is always written as `lod <= Tier` (or
    // `lod < Tier`), exactly as UE writes them. Vendored source:
    // Arcane/.example/UnrealEngine-release/Engine/Source/Editor/GraphEditor/
    // Public/SNodePanel.h:70-90; the per-tier comments below are UE's own.
    //
    // The zoom boundaries and the per-tier degradation both live in
    // ShaderEditorDocument.cpp (kLod* constants, NodeLODForScale, and the
    // branches in DrawGraphNode).
    enum class NodeLOD
    {
        LowestDetail = 0,   // zoomed all the way out (all optimizations on)
        LowDetail,          // text is unreadable, so it starts being dropped
        MediumDetail,       // text is hard to read but is still drawn
        DefaultDetail,      // zoomed in at 1:1
        FullyZoomedIn,      // zoomed in past 1:1
    };

    // The graph canvas's shader-rendered backdrop (GraphGridPass.hpp). Held by
    // unique_ptr behind a forward declaration so the nvrhi pass machinery stays
    // out of every translation unit that merely opens a document; the
    // destructor below is out-of-line, which is what makes that legal.
    class GraphGridPass;

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
        // Fired by assisted param rename for each rewritten INSTANCE file: the
        // app patches any OPEN document for that asset in memory (re-key only;
        // unsaved edits stay). (guid, oldName, newName).
        std::function<void(const Arcane::Guid&, const std::string&, const std::string&)>
            onParamRenamed;
    };

    class ShaderEditorDocument final : public EditorDocument
    {
    public:
        ShaderEditorDocument(DocServices services, std::filesystem::path path,
                             Arcane::MaterialAssetData data);
        // Closes a parked edit gesture, then destroys the node-editor contexts.
        ~ShaderEditorDocument() override;

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        // Text dirtiness is tracked directly; param dirtiness derives from the
        // instance's EffectiveSerial vs the saved baseline, so undo/redo of a
        // param edit flips the verdict correctly (review m1).
        bool Dirty() const override { return m_dirty || ParamsDirty(); }
        bool Save() override;
        bool WindowFocused() const override { return m_windowFocused; }
        void Tick(double dt) override;
        void Draw(bool& requestClose) override;

        // Draw the dockable "Material" panel for THIS document -- the preview
        // image over the params editor, split vertically. Owns its own
        // ImGui::Begin/End (and its own EditGesture::ScopeGuard, because the
        // param rows that open gestures are submitted inside it).
        //
        // The panel is a SEPARATE dock window from the document's center tab:
        // it lives beside the Inspector, so a material's parameters sit where
        // an entity's components do. The free DrawMaterialPanel below routes
        // the active document here (and submits nothing when there is none).
        void DrawMaterialWindow();

        // True on the ONE frame this document's window became the visible tab
        // (opened, or its tab was selected). Latched by Draw from
        // ImGui::IsWindowAppearing (imgui.cpp:9236-9240 returns
        // window->Appearing, which imgui.cpp:7905-7907 raises when a docked
        // window's tab becomes visible after being hidden). The host reads it
        // to fire the center-tab -> side-panel focus follow exactly once per
        // transition rather than every frame.
        bool TabBecameVisible() const { return m_tabBecameVisible; }

        // ---- Pane layout: a GLOBAL editor preference, not per-document ----
        // The pane split is one editor-wide setting shared by every open
        // shader document (last drag wins), so a layout set once is the layout
        // every material opens with. Deliberate: the alternative -- per-document
        // ratios -- makes the user re-drag the same split for each asset, and
        // there is no per-asset reason for the two to differ.
        //
        // `previewSplit` is the fraction of the Material panel's height the
        // PREVIEW takes, against the params editor below it, dragged
        // vertically. It is the only split left: the document's center tab is
        // now a single column (the graph/snippet for a base, the preview for an
        // instance) since preview+params moved out to the Material panel, so
        // the old horizontal `mainSplit` had nothing left to divide.
        //
        // The default is measured from the layout the user settled on at the
        // desk: the preview takes a little over half the panel. Double-clicking
        // the divider restores it.
        static constexpr float kPreviewSplitDefault = 0.55f;

        struct LayoutPrefs
        {
            float previewSplit = kPreviewSplitDefault;
        };
        // The one instance (process-wide). Draw reads and the dividers write it.
        static LayoutPrefs& Layout();

        // Persistence: an ImGuiSettingsHandler registered on the CURRENT ImGui
        // context, so the ratios ride the editor's existing imgui.ini next to
        // ImGui's own window/dock state -- the editor has no settings store of
        // its own, and this is ImGui's supported extension point for exactly
        // this (imgui_internal.h:2212-2225 declares the handler struct,
        // imgui.cpp:4498-4505 registers the stock "Window" one the same way).
        //
        // ORDERING: must run after ImGui::CreateContext and BEFORE the first
        // NewFrame -- NewFrame is where ImGui loads the ini file, and handlers
        // registered after that load see nothing. EditorApp::Init calls it.
        // Idempotent; a no-op with no current context (the headless tests that
        // never make one).
        static void RegisterLayoutSettings();

        // True when the result belonged to this document's in-flight compiles.
        bool ConsumeResult(const Arcane::ShaderCompileResult& result);

        bool IsInstance() const { return m_data.IsInstance(); }
        // Graph-owned (Slice 9): the node canvas is the editing surface and the
        // snippet buffer holds GENERATED text (shown read-only). Doc-level =
        // the BASE; every pass carries its own optional graph (per-pass graphs).
        bool IsGraphOwned() const { return m_data.graph.has_value(); }

        // Undo plumbing for graph edits (same doc-identity anchor pattern as
        // ApplyParamEdit): swap in a whole graph state for ONE pass (0 = base)
        // and regenerate/recompile.
        void ApplyGraphState(std::size_t pass, std::optional<Arcane::MaterialGraph> state);

        // Undo plumbing for pass-canvas STRUCTURAL edits (add/remove/rewire/
        // reorder/rename): whole pass-list before/after, one step per gesture.
        // The list is small (a handful of passes, graphs of tens of nodes), so
        // the whole-state snapshot carries the same justification as
        // GraphEditCommand's. active/view ride along so undo lands the user
        // back on the pass they were editing.
        struct PassListState
        {
            std::vector<Arcane::MaterialPass> passes;
            std::vector<std::uint32_t> baseInputs;   // scene wires on the base
            int activePass = 0;
            int viewPass = -1;
        };
        [[nodiscard]] PassListState CapturePassListState() const;
        void ApplyPassListState(PassListState state);

        // Parse/chain-resolution errors (published to the Console ahead of the
        // compile diags). Exposed for the headless tests.
        const std::vector<std::string>& ParseErrors() const { return m_parseErrors; }

        // Undo plumbing (doc-identity commands, review M3): apply a param
        // override edit to the CURRENT instance. Undo steps hold the document
        // through m_anchor -- recompiles swap m_instance underneath them, so
        // forwarding by name hash here is what lets history survive rebinds.
        void ApplyParamEdit(std::uint32_t nameHash, bool hasValue,
                            const Arcane::MatParamValue& value);

        // Assisted param rename (design 2026-07-24): the BASE document's
        // propagation rewrote this INSTANCE's file on disk -- keep this open
        // document in step (saved-params re-key + a pending rename that
        // migrates the live override when the renamed base finally rebinds).
        void PatchParamRename(const std::string& oldName, const std::string& newName);

        // External-change hooks (the app's material file watcher):
        // Reload from disk, DISCARDING the working copy -- callers gate on
        // Dirty(). Re-seeds every canvas, re-resolves chains, recompiles.
        void ReloadFromDisk();
        // True when this document's resolved parent chain contains `id`.
        bool DependsOn(const Arcane::Guid& id) const;
        // A parent's FILE changed: re-resolve the chain + recompile, keeping
        // this document's own working copy and live overrides (they migrate
        // by hash at the rebind, rename-translated).
        void RefreshParentChain();

    private:
        double Now() const { return m_services.clock ? *m_services.clock : 0.0; }
        void   Rebuild();          // parse + stitch + submit both stages (structural edit)
        void   BindIfComplete();   // both stages landed -> createShader + SetMaterial
        // Pass chains (queue item 4): a fullscreen BASE material with extra
        // passes compiles/binds through the chain path -- one merged template,
        // per-pass stages, atomic SetChain (chain-level last-good).
        // Chain mode also covers the base-only POST material (scene inputs,
        // no extra passes) -- one uniform build/run path for anything that
        // reads InputTexture slots.
        bool   ChainMode() const
        {
            return m_surface == 0 && !IsInstance() &&
                   (!m_data.passes.empty() || !m_data.baseInputs.empty());
        }
        void   BindChainIfComplete();
        // Promote pending template -> bound + rebuild the instance over it
        // (parent-chain layering, override migration, dirty re-baseline).
        void   PromotePendingInstance();
        // The snippet buffer the text editor edits: pass 0 = m_snippet, else
        // the extra pass's text. Reference is only stable within the frame.
        std::string& ActiveSnippet();
        // "base" / the extra pass's display name.
        std::string PassLabel(std::size_t pass) const;
        // Descend into a pass: `chainIndex` becomes the active pass and the
        // canvas area swaps from the chain overview to that pass's editing
        // view. Clamped, because callers read ids straight off the canvas.
        void EnterPass(int chainIndex);
        // Record the view the document is CURRENTLY showing as a history entry,
        // truncating any forward branch. Called after an EXPLICIT navigation
        // (enter, crumb click) and never by back/forward itself.
        void NavRecord();
        // Walk the history by `dir` (-1 back, +1 forward). Entries whose pass
        // no longer exists are SKIPPED rather than landed on, so a deleted pass
        // cannot strand the user on a stale index. False when the walk runs off
        // the end (nothing to go back/forward to).
        bool NavStep(int dir);
        // The one-line breadcrumb above the canvas. Root crumb = the material
        // (the chain overview), second crumb = the pass being edited. Drawn in
        // BOTH views, so the overview is always one click away -- which is what
        // keeps Add Pass reachable on a material that opened straight into its
        // graph. Returns the height it consumed.
        float DrawBreadcrumbBar();
        // The pass CANVAS: every pass is a node with a live thumbnail; wires
        // are the DAG (a wire into slot pin k IS inputs[k]). DOUBLE-CLICK a
        // pass = descend into it (selection is pure selection and no longer
        // re-aims the editor); context menu adds/removes and owns the preview
        // truncation; drag wires to rewire. Structural edits recompile.
        // Fills the canvas region -- it is one of the two mutually exclusive
        // views, not a strip above another one.
        void   DrawPassCanvas();
        // Keep execution order == array order after a rewire: stable topo sort
        // (positions/active/view indices ride along). False on a cycle.
        bool   TopoSortPasses();
        // Would wiring `source` into `consumer` (chain indices) close a cycle?
        bool   PassWireWouldCycle(std::uint32_t source, std::uint32_t consumer) const;
        bool   HasErrors() const;
        bool   ParamsDirty() const;   // EffectiveSerial vs the saved baseline
        // Instance mode: walk parent -> ... -> base through the project registry
        // (cycle-guarded). Fills m_parentChain ([immediate parent, ..., base]);
        // false (with a parse error) when a hop cannot resolve or load.
        bool ResolveParentChain();
        // The snippet the compile sees: my own (base) or the chain's base's.
        const std::string& SnippetSource() const;

        void DrawToolbar();
        // What Ctrl+S runs. Carries the error guard the toolbar's Save button
        // used to own: writing a material that does not compile is allowed, but
        // only through an explicit confirm (UE's pre-apply guard shape). Save()
        // itself stays unguarded -- the close flow's save-then-close needs it,
        // and the confirm modal's "Save Anyway" is the deliberate way past.
        void RequestSave();
        void DrawSnippetEditor();
        // ---- Diagnostics -> editor Console (no in-document panel) ----
        // THE formatting seam. One traversal turns every diagnostic this
        // document holds -- graph codegen errors (per pass), parse/stitch
        // errors, vertex-body rows, and compile diags (per pass in chain mode,
        // m_diags otherwise) -- into presentable rows, in that order. It is the
        // ONLY place that ordering and that wording exist.
        //
        // PROBLEMS-PANEL SEAM: a future Problems panel plugs in HERE. It will
        // want structured source info (origin, chain index, node id, snippet
        // line) rather than a flattened string, so widen the callback's payload
        // in this one function instead of re-deriving the traversal beside it --
        // the flattening is deliberately the last step. Non-const only because
        // GraphOptAt (the node lookup for graph-error rows) is non-const.
        void ForEachDiagnosticRow(Arcane::FunctionRef<void(bool isError,
                                                           const std::string& row)> fn);
        // Per-frame (from Tick), gated on the CONTENT of the diagnostic set.
        void PublishDiagnostics();
        // Graph mode (Slice 9, imgui-node-editor canvas). The canvas edits the
        // ACTIVE pass's graph; these resolve which optional that is.
        std::optional<Arcane::MaterialGraph>& GraphOptAt(std::size_t pass);
        std::optional<Arcane::MaterialGraph>& ActiveGraphOpt();
        bool ActiveGraphOwned() { return ActiveGraphOpt().has_value(); }
        // Regenerate EVERY graph-owned pass's snippet, then Rebuild. Safe to
        // call on text-only docs (the loop no-ops); any codegen error keeps
        // last-good bound and fills that pass's badge list instead.
        void RegenerateFromGraph();
        // One pass-canvas wire: transparent ed::Link for interaction + our own
        // curve on top, same two-layer shape the material graph's links use.
        // Raw ids rather than ed:: handles, for the same reason
        // DrawGradientWire takes them: only EditorContext is forward-declared
        // here, so the node-editor types are not nameable in this header.
        void DrawPassWire(std::uint64_t linkId, std::uint64_t fromPinId,
                          std::uint64_t toPinId);
        // Blit one canvas's shader grid backdrop. MUST be called before that
        // canvas's ed::Begin (layering + ScreenToCanvas both require it); the
        // instance is a parameter because the grid's phase is per-canvas state.
        void DrawCanvasBackdrop(std::unique_ptr<GraphGridPass>& grid);
        void DrawGraphPanel();
        // `lod` is the canvas tier for THIS frame, computed once by
        // DrawGraphPanel before the node loop and branched on at the draw sites
        // inside. Passed rather than stored so there is exactly one read of the
        // zoom per frame and no way for two nodes to disagree.
        void DrawGraphNode(Arcane::GraphNode& node, NodeLOD lod);
        // Anchor the CURRENT pin's wire endpoint at `p` (canvas space) and
        // remember it for the frame's gradient wires. Must be called between
        // ed::BeginPin and ed::EndPin. Takes ImVec2 by value; the id is the
        // raw ed::PinId payload, so the header does not need the editor's types.
        void SetPinPivot(std::uint64_t pinId, ImVec2 p);
        // One link wire, source-pin colour at the tail blending to
        // destination-pin colour at the head. Draws into the library's own link
        // layer; the ed::Link submission that owns interaction is separate.
        void DrawGradientWire(std::uint64_t fromPinId, std::uint64_t toPinId,
                              const ImVec4& fromColor, const ImVec4& toColor,
                              bool emphasize) const;
        void HandleGraphEdits();             // link create/delete queries (inside Begin/End)
        // Copy/paste: the clip is GraphToJson of the selected subgraph on the
        // SYSTEM clipboard -- cross-document paste falls out for free, and
        // pasted Param nodes merge into same-name decls by construction.
        // Both run inside the canvas Begin/End (they use ed:: selection and
        // canvas-space coordinates).
        [[nodiscard]] std::string BuildGraphClipJson();   // "" = nothing copyable
        void PasteGraphClipText(const char* text);              // ignores foreign clips
        // One undo step per completed graph gesture: `before` was captured at
        // the gesture start; `after` is read from the graph at push time. The
        // live edit already happened (ICommand contract).
        //
        // The pass-taking overload is for DEFERRED pushes -- a gesture whose
        // command builds at CLOSE rather than at the edit. Such a push can land
        // after the ACTIVE pass has moved (the pass canvas is submitted before
        // the graph panel, and an abandoned gesture closes later still, at the
        // document's ScopeGuard), so BOTH the pass index and the `after` it
        // reads must be the ones pinned when `before` was captured. Pairing
        // pass B's index with pass A's `before` would make Undo overwrite pass
        // B's graph with pass A's.
        void PushGraphUndo(const char* label, std::optional<Arcane::MaterialGraph> before);
        void PushGraphUndo(const char* label, std::optional<Arcane::MaterialGraph> before,
                           std::size_t pass);
        // One undo step per completed pass-canvas gesture (after = current).
        void PushPassUndo(const char* label, PassListState before);
        bool NodeBadged(std::uint32_t nodeId) const;
        void RebuildDiagBadges();            // compile diags -> line map -> node ids
        void DrawPreviewPanel(float height);
        // ---- Per-node preview thumbnails (SG parity) ----
        // Each previewable node of the ACTIVE graph compiles a truncated clone
        // (GenerateNodePreviewSnippet) through the NORMAL build path into its
        // own tiny RGBA16F target, hash-gated so only nodes whose upstream
        // actually changed recompile. One passthrough VS (fullscreen template,
        // empty snippet) serves every thumbnail; params sync from the doc's
        // instance every frame (redundant Sets don't bump serials); pass-graph
        // thumbnails bind the chain's live intermediates as InputTexture(N).
        void RefreshNodePreviews();          // (re)submit stale per-node compiles
        void RenderNodePreviews(double dt);  // record all ready thumbnails (own CL)
        // `width` is the node's measured content width (SG parity: the preview
        // spans the node). Zero on a node's first frame -- no width has been
        // measured yet -- which falls back to the minimum thumbnail size.
        void DrawNodePreviewImage(const Arcane::GraphNode& node, float width);
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
        // Latched each Draw; read by DocumentHost::FocusedDoc to route Ctrl+S.
        bool                            m_windowFocused = false;

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
        // Content signature of the diagnostic set at the LAST console emission.
        // 0 means "nothing outstanding" -- both the initial state and the state
        // after a clean build, so opening a clean material says nothing.
        std::uint64_t                              m_emittedDiagSig = 0;

        std::unique_ptr<Arcane::OffscreenCanvas>         m_preview;
        std::unique_ptr<Arcane::FullscreenMaterialPass>  m_pass;
        std::unique_ptr<Arcane::FullscreenMaterialChain> m_chain;   // chain mode (lazy)

        // Chain mode in-flight compile state, parallel to [m_snippet, passes...].
        struct PassJobs
        {
            std::uint64_t vsJob = 0, psJob = 0;
            std::vector<std::uint8_t> vsBytes, psBytes;
            std::vector<Arcane::ShaderDiag> diags;   // ps stage, per pass
        };
        std::vector<PassJobs> m_passJobs;
        std::vector<int> m_passLineOffsets;   // per-pass snippet offset in its hlsl
        // Vertex stage: REPAIR MODE ONLY (graphless base) -- graph-owned
        // materials author it with the Vertex Output node and view it inside
        // the read-only HLSL view (UE's one-code-viewer shape).
        bool m_editVertex = false;
        // The combined read-only HLSL view buffer (pixel body + vertex body);
        // rebuilt each frame it is shown.
        std::string m_generatedView;
        std::vector<Arcane::ShaderDiag> m_vsDiags;
        int  m_vsLineOffset = 0;
        // Validated DAG wiring from the last chain build (what SetChain binds;
        // captured at Rebuild so async binds never race pass-list edits).
        std::vector<std::vector<std::uint32_t>> m_passInputs;
        std::uint32_t m_chainInputSlots = 1;
        // Pass-canvas state (a SECOND node-editor context; node ids are chain
        // index + 1, the Output node is kPassOutputNodeId).
        ax::NodeEditor::EditorContext* m_passCanvasCtx = nullptr;
        bool  m_passCanvasSeeded = false;   // re-seed positions after list edits
        std::uint32_t m_passCtxNode = 0;    // node the context menu opened on
        float m_passPopupX = 0.0f, m_passPopupY = 0.0f;
        int m_activePass = 0;   // which snippet the text editor shows (0 = base)
        int m_viewPass = -1;    // preview truncation; -1 = the full chain
        // Which of the two mutually exclusive views owns the canvas area: the
        // chain overview, or m_activePass's editing view. Seeded in the ctor
        // from whether there IS a chain worth surveying, then driven by the
        // breadcrumb (up) and double-click-to-enter (down).
        //
        // m_activePass keeps its value while the overview is up rather than
        // being cleared: the preview, the params panel and the side Material
        // tab all key off it, and blanking it on every trip to the overview
        // would make them flicker back to the base and lose the user's place.
        // The overview is a NAVIGATION layer over the document, not a different
        // document -- so "which pass am I editing" survives a look at the map.
        bool m_inChainView = true;

        // ---- Back/forward navigation (mouse4/mouse5) ----
        // Browser semantics over the document's two-level view space. One
        // visited view; `pass` is meaningful only when chainView is false.
        struct ViewEntry
        {
            bool chainView = true;
            int  pass = 0;
            bool operator==(const ViewEntry&) const = default;
        };
        // Visited views, oldest first, with m_navIndex pointing at the CURRENT
        // one -- so back/forward is index arithmetic and nothing else. Explicit
        // navigation truncates everything after the index before appending,
        // which is what makes a new jump abandon the forward branch.
        //
        // IN-MEMORY AND PER-DOCUMENT, deliberately: this is a record of a
        // reading session, not of the asset, so it neither persists nor rides
        // undo. Closing the document forgets it, which is what a browser tab
        // does too.
        std::vector<ViewEntry> m_navHistory;
        int m_navIndex = -1;
        // Modest cap; the oldest entry drops when it is hit. Nobody walks back
        // 32 view changes, and an uncapped vector on a long session is a leak
        // with extra steps.
        static constexpr int kNavHistoryMax = 32;

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
        // 1-based; 0 = no pending jump. DORMANT for the same reason as
        // m_focusNode -- the errors-panel rows were the only writer.
        int    m_jumpToLine = 0;
        bool   m_focusSnippet = false;               // focus the input so the jump lands
        // Armed jump consumed by the input callback. Per-document (review m2): a
        // shared static could deliver one doc's jump to another on a same-frame
        // focus race.
        int    m_callbackJumpLine = 0;

        // Latched by Draw from ImGui::IsWindowAppearing -- see TabBecameVisible.
        bool   m_tabBecameVisible = false;

        // The document's ONE edit-gesture bracket (EditGesture). TWO draw
        // scopes open gestures against it now -- Draw (the graph's value/pin
        // drags) and DrawMaterialWindow (the param rows) -- so BOTH declare a
        // ScopeGuard as their first local. The host draws the Material panel
        // BEFORE the documents (EditorApp::DrawEditorUi), which puts Draw's
        // guard last in the frame: the guaranteed close still runs after every
        // widget that can open a gesture has been submitted.
        // Param-panel drags carry
        // the override value, graph value drags the WHOLE graph (small graphs --
        // the SG full-snapshot-undo pathology was per-edit reserialization plus
        // full preview regeneration, neither of which applies here). Before-
        // state on activation, one undo step at close. Per-document (review m3):
        // same-frame keyboard-nav active-ID transfer between documents could
        // cross shared statics.
        EditGesture::GestureState m_gesture;

        // ---- Graph mode (Slice 9; per-pass graphs) ----
        ax::NodeEditor::EditorContext* m_graphCtx = nullptr;   // lazy; dtor destroys
        // Shader-rendered canvas backdrop, blitted under the canvas content.
        // Lazy and optional: device-less services (the headless tests) and a
        // missing shader artifact both leave it null and simply draw no grid.
        std::unique_ptr<GraphGridPass> m_grid;
        // The pass canvas's own backdrop instance. Separate because the grid's
        // phase is per-canvas state (DrawCanvasBackdrop says why), and the two
        // views alternate.
        std::unique_ptr<GraphGridPass> m_passGrid;
        // Pass-canvas node widths, keyed by pass-canvas node id. Its own map:
        // chain-index-derived ids and graph node ids are unrelated counters.
        std::unordered_map<std::uint32_t, float> m_passNodeWidths;
        // Each node's measured width from the LAST frame it drew, keyed by node
        // id. Right-aligned output rows and full-width previews both need a
        // width that only exists after the node has been laid out, so they use
        // the previous frame's -- which is stable, because aligning to width W
        // produces rows of exactly W. Cleared with the canvas on a pass switch.
        std::unordered_map<std::uint32_t, float> m_nodeWidths;
        // THIS FRAME's wire anchor for every pin submitted, keyed by ed::PinId.
        // Written at BeginPin/EndPin time by SetPinPivot, which hands the SAME
        // point to ed::PinPivotRect -- so the gradient wires drawn afterwards
        // start from the library's own endpoints instead of re-deriving them.
        // Rebuilt every frame (positions move with the node and the view).
        std::unordered_map<std::uint64_t, ImVec2> m_pinPivots;
        // Per-pass codegen state, indexed by CHAIN index (0 = base). Sized by
        // RegenerateFromGraph; empty entries = text-owned or clean.
        std::vector<std::vector<Arcane::GraphError>> m_passGraphErrors;
        std::vector<std::vector<std::uint32_t>> m_passLineNodeIds;   // GOOD line maps
        std::vector<std::uint32_t> m_diagBadgeNodes;     // ACTIVE pass's diag nodes
        int m_graphShownPass = -1;   // canvas re-seeds + reselects on pass switch
        // First snippet line's 0-based offset inside the stitched HLSL -- maps
        // compiler diag lines back into snippet space (jump + badges).
        int  m_snippetLineOffset = 0;
        bool m_graphPositionsApplied = false;   // canvas seeded from stored node positions
        bool m_showGeneratedText = false;       // toolbar toggle: canvas <-> read-only HLSL
        // Select + navigate the canvas to one node. DORMANT: the errors panel's
        // rows were the only writer, and console lines are not clickable -- the
        // Problems panel is what re-drives it (DrawGraphPanel still consumes it).
        std::uint32_t m_focusNode = 0;
        float m_graphPopupX = 0.0f, m_graphPopupY = 0.0f;   // create-menu screen pos
        // Drag-wire searcher (SG's signature interaction): releasing a new wire
        // over empty canvas opens the create menu filtered to types with a pin
        // on the wire's far side, and the created node auto-connects. The
        // request flag carries the accept from HandleGraphEdits (canvas space)
        // into the Suspend'ed popup block; m_wireActive spans the popup.
        bool          m_wireCreateRequest = false;
        bool          m_wireActive = false;
        bool          m_wireIsInput = false;    // dragged pin is an input pin
        std::uint32_t m_wireNode = 0, m_wirePin = 0;
        char          m_createSearch[64] = {};  // create-menu search filter
        // In-progress inline text edits (pass name, comment title, param/
        // texture name, swizzle mask) all share ONE StableTextEdit buffer --
        // only one InputText is active at a time. The key is namespaced by SITE
        // KIND because a pass's CHAIN INDEX and a node's ID are unrelated
        // counters that would otherwise collide on the same small number (the
        // pre-widget-layer code kept two separate members for exactly that
        // reason); the kind tag restores the separation inside one slot.
        enum class TextEditKind : std::uint64_t { PassName = 1, NodeName, Swizzle, Comment };
        static constexpr std::uint64_t TextKey(TextEditKind k, std::uint64_t id) noexcept
        { return (static_cast<std::uint64_t>(k) << 56) | id; }
        TextCommitState m_textEdit;
        // Custom-node HLSL body editing: the node shows a plain-text preview
        // (child-window widgets drift inside the canvas); the body edits in a
        // Suspend'ed MODAL. Request set by the node's button, consumed in
        // DrawGraphPanel's popup block; the buffer holds the working copy
        // until Apply commits it as one undo step.
        std::uint32_t m_bodyEditRequest = 0;
        std::uint32_t m_bodyEditNode = 0;
        char          m_bodyBuf[4096] = {};

        // ---- Assisted param rename (graph tier only; text //@param edits are
        // not reliably detectable as renames) ----
        struct RenameTarget
        {
            Arcane::Guid id;
            std::filesystem::path path;
            std::string name;
        };
        // Rename-commit hook: sole-declarer guard, local fix, registry walk
        // for dependent instance files; a nonempty hit list arms the modal.
        void BeginParamRename(const std::string& oldName, const std::string& newName);
        // Pending renames awaiting MATERIALIZATION: override-hash migration at
        // PromotePendingInstance (conditional on the target template, so an
        // undo rolling the template back translates in reverse) and saved-name
        // translation at instance Save.
        [[nodiscard]] std::uint32_t TranslateOverrideHash(
            std::uint32_t hash, const Arcane::MaterialTemplate& templ) const;
        std::vector<std::pair<std::string, std::string>> m_paramRenames;
        std::vector<RenameTarget> m_renameTargets;   // the modal's hit list
        std::string m_renameOld, m_renameNew;
        bool m_renameRequest = false;   // open the modal (Suspend space)

        // ---- Node preview thumbnails ----
        struct NodePreview
        {
            std::uint64_t psJob = 0;         // in-flight compile (0 = none)
            std::uint64_t snippetHash = 0;   // last generated preview source
            std::vector<std::uint8_t> psBytes;   // landed before the shared VS
            std::shared_ptr<Arcane::MaterialTemplate> pendingTempl;
            std::uint32_t pendingInputs = 0; // slot count the source stitched
            std::vector<std::uint32_t> pendingSources;   // wired chain indices
            // Bound state (last-good: a failed recompile leaves these showing).
            std::unique_ptr<Arcane::FullscreenMaterialPass> pass;
            std::shared_ptr<Arcane::MaterialInstance> inst;
            std::uint32_t boundInputs = 0;
            std::vector<std::uint32_t> boundSources;
            nvrhi::TextureHandle tex;
            nvrhi::FramebufferHandle fb;
            bool ready = false;
        };
        std::unordered_map<std::uint32_t, NodePreview> m_nodePreviews;   // ACTIVE graph
        int  m_nodePreviewsPass = -1;   // which pass the map belongs to
        bool m_showNodePreviews = true; // toolbar toggle
        // Zoom-tier gate on the same thumbnails: DrawGraphPanel clears this
        // when the canvas LOD has stopped DRAWING them, so RenderNodePreviews
        // stops PAYING for them (one offscreen material pass per node per
        // frame -- the graph canvas's dominant GPU cost, and the only one that
        // scales with how many nodes are on screen).
        //
        // Reads one frame late by construction: Tick renders before the panel
        // draws, so a tier transition costs one extra frame of rendering (or
        // one frame of a stale thumbnail on the way back in). Invisible, and
        // the alternative -- driving the render off the panel -- would put a
        // command-list submit inside the ImGui pass.
        bool m_nodePreviewsInLod = true;
        std::uint64_t       m_nodePreviewVsJob = 0;
        nvrhi::ShaderHandle m_nodePreviewVs;    // shared passthrough VS
        nvrhi::CommandListHandle m_nodePreviewCl;
        // The Scene stand-in (post materials): a checkerboard bound wherever
        // kSceneInput slots appear in the PREVIEW -- the real scene color
        // only exists at runtime. Lazy; null device-less.
        nvrhi::TextureHandle m_sceneStandIn;
        nvrhi::ITexture* SceneStandIn();
        // Displaced thumbnail textures parked one frame: an ImGui::Image
        // submitted earlier in the SAME frame still holds the raw pointer
        // until the backend records it, so release happens next Tick.
        std::vector<nvrhi::TextureHandle> m_nodePreviewRetired;
        void BindNodePreview(NodePreview& np, nvrhi::ShaderHandle ps);

        friend struct SnippetCallbackForwarder;
    };

    // The dockable "Material" panel. Docked as a TAB beside the Inspector by
    // default (EditorPanels.cpp's BuildDefaultLayout); the user may re-dock it
    // freely and imgui.ini remembers where.
    //
    // `active` is the material document whose content to show -- the one whose
    // center tab is active, or the last one that was (EditorApp resolves it).
    // Null submits NOTHING: with no material open the tab is ABSENT, not empty.
    // Hiding it does not cost its dock slot -- cites at the definition.
    //
    // SEAM: this panel is the intended home of selected-NODE properties too
    // (Unity Shader Graph's Graph Inspector shape -- click a node in the
    // canvas, edit its properties here). Nothing is built for that yet; when it
    // lands it becomes a second section of this window, above or beside the
    // params, and the graph canvas will publish its node selection to it.
    void DrawMaterialPanel(ShaderEditorDocument* active);
}
