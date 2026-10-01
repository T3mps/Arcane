#include "Documents/SpriteDocument.hpp"

#include "Panels/AssetPanelModel.hpp"   // AssetKind (the Texture row's kind)
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Assets/Assets.hpp>   // TextureInfoFor (the sprite rect crop, the Whole texture untick)
#include <Arcane/Edit/Command.hpp>

#include <Astra/Reflection/Attribute.hpp>   // Astra::Range (the ranged rows)

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        // One completed field gesture as an undo step. The live edit already
        // happened (the ICommand contract); Undo restores the BEFORE data,
        // Redo re-applies the AFTER. Whole-data steps rather than per-field
        // ones because the payload is five small fields -- the same
        // whole-state justification GraphEditCommand carries, at a fraction of
        // the size.
        //
        // Doc-identity: the step holds the DOCUMENT weakly through an anchor
        // and forwards to whatever it currently points at -- exactly
        // ParamEditCommand's mechanism (ShaderEditorDocument.cpp:54-87, resolve
        // at :66-72), because a raw SpriteDocument* dangles the moment the
        // document closes with steps still on the shared stack, and the stack
        // outlives every document (EditorApp owns it; DocumentHost::Close
        // erases the document synchronously, DocumentHost.cpp:135-141).
        class SpriteDataEditCommand final : public Arcane::ICommand
        {
        public:
            SpriteDataEditCommand(std::weak_ptr<SpriteDocument*> anchor, std::string label,
                                  Arcane::SpriteAssetData before,
                                  Arcane::SpriteAssetData after)
                : m_anchor(std::move(anchor)), m_label(std::move(label)),
                  m_before(std::move(before)), m_after(std::move(after))
            {
            }

            void Undo() override { Apply(m_before); }
            void Redo() override { Apply(m_after); }
            const char* Label() const override { return m_label.c_str(); }
            // Spec s3.3: a document step never dirties the scene, and it is
            // EXPIRED once its document closed (the anchor died).
            bool AffectsScene() const override { return false; }
            bool IsExpired() const override
            {
                const auto doc = m_anchor.lock();
                return !doc || !*doc;
            }

        private:
            void Apply(const Arcane::SpriteAssetData& data)
            {
                auto doc = m_anchor.lock();
                if (!doc || !*doc)
                    return;   // document closed -- the step is inert
                (*doc)->ApplySpriteData(data);
            }

            std::weak_ptr<SpriteDocument*> m_anchor;
            std::string                    m_label;
            Arcane::SpriteAssetData        m_before;
            Arcane::SpriteAssetData        m_after;
        };
    }

    SpriteDocument::SpriteDocument(Services services, std::filesystem::path path,
                                   Arcane::SpriteAssetData data)
        : m_services(std::move(services)), m_path(std::move(path)), m_data(std::move(data))
    {
        // Same name-fallback rule as ShaderEditorDocument (ShaderEditorDocument.cpp:950):
        // an empty asset name (hand-authored file, or a pre-name-field asset)
        // falls back to the file stem rather than showing a blank title.
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + " (Sprite)###spritedoc_" + m_data.id.ToString();
        // The anchor every undo step routes through; it dies with the document
        // (ShaderEditorDocument.cpp:955 mints its own the same way).
        m_anchor = std::make_shared<SpriteDocument*>(this);
    }

    SpriteDocument::~SpriteDocument()
    {
        // Teardown close, same shape and rationale as ShaderEditorDocument's
        // (ShaderEditorDocument.cpp:1000-1021). Documents are destroyed
        // synchronously on close and there is no on-close hook: the X-button
        // path is already safe (requestClose is raised INSIDE Draw and acted on
        // after the loop, so Draw's ScopeGuard has run), but a close that
        // destroys the document between a gesture parking and its next Draw --
        // the project-switch CloseAll (DocumentHost.cpp:128-133) -- would
        // strand the transaction open, and one stranded transaction leaves
        // InTransaction() true editor-wide: structural edits refused
        // (CanEditStructure) AND Ctrl+Z/Ctrl+Y dead (EditorAppFrame.cpp:424-426).
        //
        // It is not free: a close that LANDS a step also clears the REDO stack
        // (CommandStack.cpp:70 -- reached only for a non-empty transaction,
        // since :61-62 returns first when nothing changed). So closing a
        // document mid-gesture discards redo history. That is the accepted cost
        // of ClosePending's commit-not-cancel rule, which exists because Cancel
        // would discard the transaction WITHOUT reverting the edits the user
        // already watched happen.
        //
        // Order matters and holds: this body runs BEFORE the members are
        // destroyed, so the pendingCommit builder ClosePending fires still sees
        // a live m_data and a live m_anchor. Every step it just pushed goes
        // inert an instant later, when m_anchor's control block drops.
        if (UndoStack())
            EditGesture::ClosePending(*UndoStack(), m_gesture);
    }

    void SpriteDocument::FlushGesture()
    {
        if (Arcane::CommandStack* s = UndoStack())
            EditGesture::ClosePending(*s, m_gesture);
    }

    void SpriteDocument::ApplySpriteData(const Arcane::SpriteAssetData& data)
    {
        m_data = data;
        // Dirty is a COARSE ledger here: undoing all the way back to the saved
        // bytes still reads dirty, because this document tracks a bool rather
        // than a save-point state id (SceneSession rides CommandStack::SceneStateId
        // for that; a five-field asset does not earn it). That errs toward
        // offering a redundant save, never toward silently dropping one.
        m_dirty = true;
        // Same republish Save does, and for the same reason: SpriteCache's
        // resolve is a once-per-Guid cache (Render/SpriteCache.cpp:37), so without
        // this the viewport would keep drawing the PRE-undo geometry. An undo
        // the user cannot see in the scene is indistinguishable from an undo
        // that did not happen.
        if (m_services.invalidateSprite)
            m_services.invalidateSprite(m_data.id);
    }

    void SpriteDocument::PushDataEdit(std::string label, const Arcane::SpriteAssetData& before)
    {
        // No stack (Play mode / an unwired document) or nothing actually moved
        // -> no step. The second guard is what keeps a bare click on a drag
        // out of the history; the stack drops empty TRANSACTIONS on its own
        // (CommandStack.cpp:61-62) but a generic Push is unconditional
        // (:84-102), so the compare has to happen here.
        if (!UndoStack() || before == m_data)
            return;
        UndoStack()->Push(std::make_unique<SpriteDataEditCommand>(
            m_anchor, std::move(label), before, m_data));
    }

    bool SpriteDocument::Save()
    {
        if (!Arcane::SaveSpriteAsset(m_path, m_data))
            return false;   // failed save: m_dirty stays set, edits are not silently lost
        m_dirty = false;
        // The SpriteCache resolve this replaces is otherwise permanent (once-per-Guid,
        // Render/SpriteCache.cpp:37) -- without this call the viewport keeps showing the
        // PRE-edit geometry/texture until something else evicts the entry.
        if (m_services.invalidateSprite)
            m_services.invalidateSprite(m_data.id);
        return true;
    }

    void SpriteDocument::Draw(bool& requestClose)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard
        // (EditGesture.hpp:205-227). It covers the early return below (Begin
        // refused: collapsed window or a background tab, where no widget inside
        // can report its own deactivation), which for a DOCUMENT window is the
        // routine case, not an edge one: any other tab in the same dock node
        // being in front puts this document exactly there.
        const EditGesture::ScopeGuard gestureGuard{ UndoStack(), m_gesture };

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_FirstUseEver);
        const ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, flags))
        {
            // Collapsed (not closed): ShaderEditorDocument's same early-return
            // shape (ShaderEditorDocument.cpp:1731-1740) -- `open` only goes
            // false when the titlebar X was clicked, so a merely-collapsed
            // window still reports requestClose=false here.
            m_windowFocused = false;   // see the member: a stale true misroutes Ctrl+S
            ImGui::End();
            requestClose = !open;
            return;
        }
        // Answers DocumentHost::FocusedDoc, which is how the APP's scene-level
        // Ctrl+S learns to stand down while the user is inside a document. The
        // Shortcut below routes itself and does not need this.
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        // Ctrl+S saves. Shortcut() (not IsKeyChordPressed) so it ROUTES to
        // whichever window/document currently owns focus (imgui.h:1106-1114,
        // default ImGuiInputFlags_RouteFocused) -- with several sprite/
        // material documents open at once, each one's Ctrl+S only fires for
        // the one on top.
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
            Save();

        if (ImGui::Button("Save"))
            Save();
        ImGui::SameLine();
        ImGui::TextDisabled(Dirty() ? "(unsaved)" : "(saved)");

        // The form (the read-only Texture row, the four drags and "Whole
        // texture") is the Inspector's sprite page now (DrawFormBody, drawn
        // by whichever Inspector instance shows this document -- inspector
        // filters s6a).
        // What stays is the toolbar above, a pointer to the page, the texture
        // line and the sprite itself (final fix D).
        ImGui::Separator();
        ImGui::TextDisabled("Sprite properties are in the Inspector");
        const std::string texName = m_services.assetName ? m_services.assetName(m_data.texture) : std::string{};
        ImGui::TextDisabled("Texture: %s", texName.empty() ? m_data.texture.ToString().c_str() : texName.c_str());
        // The sprite through the chrome's thumbnail seam (the Asset Browser
        // rows' resolveAssetThumb): the texture cache OWNS the ImGui texture
        // and its invalidation, so this window uploads nothing. Cropped to
        // the sprite's rect with ComputeSpriteGeom when the texture's true
        // dims are known (a header read, memoized); the thumbnail is a
        // downscale of the whole texture, so the UVs hold.
        if (m_services.resolveThumb && m_data.texture.IsValid())
            if (const std::uint64_t thumb = m_services.resolveThumb(m_data.texture); thumb != 0)
            {
                std::uint32_t texW = 0, texH = 0;
                if (m_services.assets)
                    if (const Arcane::TextureInfo* info = m_services.assets->TextureInfoFor(m_data.texture))
                    { texW = info->width; texH = info->height; }
                const Arcane::ResolvedSpriteGeom geom = Arcane::ComputeSpriteGeom(m_data, texW, texH);
                // The rect's pixel aspect when known, else square.
                const bool rect = texW != 0 && texH != 0 && m_data.sourceSize.x > 0.0f && m_data.sourceSize.y > 0.0f;
                const float aspect = rect ? m_data.sourceSize.x / m_data.sourceSize.y
                                   : (texW != 0 && texH != 0 ? static_cast<float>(texW) / static_cast<float>(texH) : 1.0f);
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float side = std::max(std::min(avail.x, avail.y), 16.0f);
                const ImVec2 size = aspect >= 1.0f ? ImVec2(side, side / aspect) : ImVec2(side * aspect, side);
                ImGui::Image(static_cast<ImTextureID>(thumb), size,
                             ImVec2(geom.uvMin.x, geom.uvMin.y), ImVec2(geom.uvMax.x, geom.uvMax.y));
            }

        // Opened = selected; a click anywhere in the content (the toolbar,
        // the hint, the sprite) re-selects the sprite page (spec s3's one
        // selection rule). AFTER the content, and only on this non-collapsed
        // path: a collapsed/background tab has no content to click.
        m_pageSel.NoteContentClick();

        ImGui::End();
        requestClose = !open;
    }

    bool SpriteDocument::SetWholeTexture(Arcane::SpriteAssetData& data, bool whole, std::uint32_t texW, std::uint32_t texH)
    {
        if (whole) { data.sourcePos = { 0.0f, 0.0f }; data.sourceSize = { 0.0f, 0.0f }; return true; }
        if (texW == 0 || texH == 0) return false;
        data.sourcePos = { 0.0f, 0.0f };
        data.sourceSize = { static_cast<float>(texW), static_cast<float>(texH) };
        return true;
    }

    AssetRefArgs SpriteDocument::TextureRefArgs(const Arcane::SpriteAssetData& data)
    {
        // v1 is read-only by design: reassigning the source texture goes
        // through "Create Sprite" on a DIFFERENT texture (mints a new sibling
        // .arcsprite, EditorAppProject.cpp MintOrReuseSpriteForTexture), not an
        // in-place swap of this asset's `texture` field. readOnly hides the
        // picker and the clear and refuses drops (AssetReferenceField.hpp).
        AssetRefArgs args;
        args.guid = data.texture;
        args.kindFilter = static_cast<int>(AssetKind::Texture);
        args.readOnly = true;
        return args;
    }

    void SpriteDocument::DrawFormBody(PropertyGrid& grid)
    {
        // FIRST local, so it destructs LAST -- see EditGesture::ScopeGuard.
        // The rows below open gestures against m_gesture. The body draws
        // inside an Inspector instance window, AFTER the documents, and on
        // collapsed/background-tab frames too (InspectorWindows calls
        // page->Draw even when Begin returns false) -- where no widget inside
        // can report its own deactivation, which is exactly what this guard
        // covers. Draw keeps its own guard for the document window's refused-
        // Begin path (ShaderEditorDocument's Draw + DrawMaterialPageBody are
        // the precedent for two guards on one gesture).
        Arcane::CommandStack* const undo = UndoStack();
        const EditGesture::ScopeGuard gestureGuard{ undo, m_gesture };

        // Field clamp policy: ClampOnInput, not AlwaysClamp. The ranged
        // FloatRow/VecRow route through RangedDragFloat / AxisDragFloatN with
        // ImGuiSliderFlags_ClampOnInput (PropertyGrid.hpp): Ctrl+Click on a
        // Drag widget opens a text box whose typed value ImGui does NOT clamp
        // to v_min/v_max unless ClampOnInput is set -- confirmed at the call
        // site (imgui_widgets.cpp:2781-2784 routes into TempInputScalar only
        // when TempInputIsClampEnabled agrees) and in the header's own doc
        // comment (imgui.h:676,701). AlwaysClamp (imgui.h:2030-2034) is
        // ClampOnInput | ClampZeroRange; ClampZeroRange only changes behavior
        // for a DEGENERATE v_min==v_max==0.0f range (DragBehaviorT's
        // is_bounded test, imgui_widgets.cpp:2540, and
        // TempInputIsClampEnabled's own zero-range branch, :2711-2712) -- none
        // of the ranges below are degenerate (ppu/pivot have a real max;
        // sourcePos/sourceSize use FLT_MAX, not 0.0f, as their "no real
        // upper bound" sentinel specifically so min==0/max==0 never happens,
        // which would otherwise make BOTH mouse-drag and keyboard entry fully
        // unbounded, not just keyboard entry -- is_bounded already requires
        // v_min<v_max at :2540).

        // Undo bracket for the numeric rows below (widget-layer Task 7; the
        // close is EndAfterRow, s4.1). Call it IMMEDIATELY after each row:
        // both halves read ImGui's LastItemData (the row's value widget,
        // s4.1(f)), so anything submitted in between would move the id out
        // from under them; EndAfterRow's `cancelled` is the row's Escape, so
        // an Esc on a grouped VecRow closes at the row.
        //
        // Deferred/builder style: `before` is pinned when the widget ACTIVATES,
        // the command is built at CLOSE from that plus whatever m_data holds
        // then -- which is what makes an ABANDONED drag (window collapsed
        // mid-drag, tab sent to the background) land a step instead of
        // vanishing.
        //
        // Close-time re-read hazard (the cross-pass class the shader editor hit
        // at 6c997412): `after` is read at close, so anything that could
        // replace m_data between activation and close would pair a stale
        // `before` with an unrelated `after`. Enumerated, this document has no
        // such path. There is ONE m_data and no pass/target selector to drift.
        // Nothing outside this class holds a SpriteDocument (EditorApp.cpp:351-
        // 389 constructs one and hands it straight to DocumentHost), there is
        // no ReloadFromDisk hook on it (ShaderEditorDocument has one,
        // ShaderEditorDocument.hpp:226-230; the sprite watcher path does not
        // exist), and re-opening the same asset focuses this document via the
        // registered peek instead of building a second one (EditorApp.cpp:390-
        // 396). The only other writer is ApplySpriteData, i.e. an undo/redo --
        // and Ctrl+Z/Ctrl+Y are refused while any transaction is open
        // (EditorAppFrame.cpp:424-426), which covers every gesture that owns
        // one. A gesture that JOINED someone else's transaction (Begin returned
        // None) does open a window after that owner commits, but the worst it
        // yields is a step whose `before` is this drag's activation state and
        // whose `after` is what the document actually shows -- one object,
        // self-consistent, never another target's data. The "Whole texture"
        // flip is not a gesture: it pushes its own step on the frame it lands.
        const auto bracket = [&](const char* label)
        {
            EditGesture::BeginOnActivate(undo, m_gesture,
                [&] { return std::string(label); },
                [&]
                {
                    return std::function<void()>(
                        [this, label = std::string(label), before = m_data] { PushDataEdit(label, before); });
                });
            EditGesture::EndAfterRow(undo, m_gesture, grid.LastRowEvents().cancelled);
        };

        if (!grid.Section("Sprite"))
            return;
        PropertyGrid::Rows rows(grid, "##sprite");
        if (!rows)
            return;

        static const AssetRefServices kNoRefs{};   // the null services: kind glyph + raw guid
        (void)AssetRefRow(grid, "Texture", TextureRefArgs(m_data), m_services.assetRefs ? *m_services.assetRefs : kNoRefs);

        // m_dirty and the undo history are SEPARATE ledgers: Save clears dirty
        // and never touches history, undo pushes history and never clears
        // dirty. m_data still mutates live (the document's crop follows a
        // drag), so dirt is "m_data moved this frame", compared at the end.
        const Arcane::SpriteAssetData shown = m_data;
        (void)grid.FloatRow("Pixels Per Meter", m_data.ppu, 0.5f, Astra::Range(1.0, 4096.0), "%g");
        bracket("Edit Pixels Per Meter");

        // "Whole texture": a UI view over sourceSize == (0,0) (s5.4). Unticking
        // needs the texture's true size (an artifact HEADER read, memoized);
        // with it unknown the ticked box is disabled -- typing a non-zero
        // Source Size below unticks it by construction.
        std::uint32_t texW = 0, texH = 0;
        if (m_services.assets)
            if (const Arcane::TextureInfo* info = m_services.assets->TextureInfoFor(m_data.texture))
            { texW = info->width; texH = info->height; }
        const bool whole = m_data.sourceSize.x == 0.0f && m_data.sourceSize.y == 0.0f;
        const bool locked = whole && (texW == 0 || texH == 0);
        bool wholeBox = whole;
        ImGui::BeginDisabled(locked);
        const bool flipped = grid.CheckboxRow("Whole texture", wholeBox);
        ImGui::EndDisabled();
        if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Texture size unknown -- type a Source Size to use a sub-rect");
        if (flipped)
        {
            const Arcane::SpriteAssetData before = m_data;
            if (SetWholeTexture(m_data, wholeBox, texW, texH))
            {
                m_dirty = true;
                PushDataEdit("Whole Texture", before);
            }
        }

        // "%.0f": integer pixel fields -- a drag rounds to the format (Mesh & Sprite #10).
        (void)grid.VecRow("Source Pos", &m_data.sourcePos.x, 2, 1.0f, Astra::Range(0.0, FLT_MAX), "%.0f");
        bracket("Edit Source Pos");
        (void)grid.VecRow("Source Size", &m_data.sourceSize.x, 2, 1.0f, Astra::Range(0.0, FLT_MAX), "%.0f");
        bracket("Edit Source Size");
        (void)grid.VecRow("Pivot", &m_data.pivot.x, 2, 0.005f, Astra::Range(0.0, 1.0), "%.3f");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Normalized: (0, 0) = bottom-left, (1, 1) = top-right (+Y up).\n"
                              "Sprites authored before F4 used y = 0 = top; an off-centre pivot\n"
                              "from then now anchors on the other side -- re-author it here.");
        bracket("Edit Pivot");
        if (!(m_data == shown))
            m_dirty = true;
    }
}
