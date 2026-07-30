#include "SpriteDocument.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Edit/Command.hpp>
#include <Arcane/Project/AssetId.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

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
        if (m_services.undo)
            EditGesture::ClosePending(*m_services.undo, m_gesture);
    }

    void SpriteDocument::ApplySpriteData(const Arcane::SpriteAssetData& data)
    {
        m_data = data;
        // Dirty is a COARSE ledger here: undoing all the way back to the saved
        // bytes still reads dirty, because this document tracks a bool rather
        // than a save-point state id (SceneSession rides CommandStack::StateId
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
        if (!m_services.undo || before == m_data)
            return;
        m_services.undo->Push(std::make_unique<SpriteDataEditCommand>(
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
        const EditGesture::ScopeGuard gestureGuard{ m_services.undo, m_gesture };

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
        ImGui::Separator();

        // Field clamp policy: ClampOnInput, not AlwaysClamp. Ctrl+Click on a
        // Drag widget opens a text box whose typed value ImGui does NOT clamp
        // to v_min/v_max unless ImGuiSliderFlags_ClampOnInput is set --
        // confirmed at the call site (imgui_widgets.cpp:2781-2784 routes into
        // TempInputScalar only when TempInputIsClampEnabled agrees) and in
        // the header's own doc comment (imgui.h:676,701). AlwaysClamp
        // (imgui.h:2030-2034) is ClampOnInput | ClampZeroRange; ClampZeroRange
        // only changes behavior for a DEGENERATE v_min==v_max==0.0f range
        // (DragBehaviorT's is_bounded test, imgui_widgets.cpp:2540, and
        // TempInputIsClampEnabled's own zero-range branch, :2711-2712) -- none
        // of the ranges below are degenerate (ppu/pivot have a real max;
        // sourcePos/sourceSize use FLT_MAX, not 0.0f, as their "no real
        // upper bound" sentinel specifically so min==0/max==0 never happens,
        // which would otherwise make BOTH mouse-drag and keyboard entry fully
        // unbounded, not just keyboard entry -- is_bounded already requires
        // v_min<v_max at :2540). ClampOnInput is also the established local
        // convention for exactly this "keyboard entry must not defeat a
        // drag's bound" concern (EditorWidgets.hpp:43-49's RangedDragFloat).

        // Undo bracket for the four drags below (widget-layer Task 7). Call it
        // IMMEDIATELY after each widget: both halves read ImGui's LastItemData,
        // so anything submitted in between (the TextDisabled hint) would move
        // the id out from under them.
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
        // self-consistent, never another target's data.
        const auto bracket = [&](const char* label)
        {
            EditGesture::BeginOnActivate(m_services.undo, m_gesture,
                [&] { return std::string(label); },
                [&]
                {
                    return std::function<void()>(
                        [this, label = std::string(label), before = m_data]
                        { PushDataEdit(label, before); });
                });
            EditGesture::EndOnDeactivate(m_services.undo, m_gesture);
        };

        // m_dirty and the undo history are SEPARATE ledgers: Save clears dirty
        // and never touches history, undo pushes history and never clears
        // dirty. `changed` keeps driving dirty exactly as before.
        bool changed = false;
        changed |= ImGui::DragFloat("Pixels Per Meter", &m_data.ppu, 0.5f, 1.0f, 4096.0f,
                                    "%.3f", ImGuiSliderFlags_ClampOnInput);
        bracket("Edit Pixels Per Meter");
        changed |= ImGui::DragFloat2("Source Pos", &m_data.sourcePos.x, 1.0f, 0.0f, FLT_MAX,
                                     "%.3f", ImGuiSliderFlags_ClampOnInput);
        bracket("Edit Source Pos");
        changed |= ImGui::DragFloat2("Source Size", &m_data.sourceSize.x, 1.0f, 0.0f, FLT_MAX,
                                     "%.3f", ImGuiSliderFlags_ClampOnInput);
        bracket("Edit Source Size");
        ImGui::TextDisabled("(0, 0) = whole texture");
        changed |= ImGui::DragFloat2("Pivot", &m_data.pivot.x, 0.005f, 0.0f, 1.0f,
                                     "%.3f", ImGuiSliderFlags_ClampOnInput);
        bracket("Edit Pivot");
        if (changed)
            m_dirty = true;

        ImGui::Separator();
        ImGui::TextUnformatted("Texture");
        ImGui::SameLine();
        // v1 is read-only by design: reassigning the source texture goes
        // through "Create Sprite" on a DIFFERENT texture (mints a new sibling
        // .arcsprite, EditorAppProject.cpp:191-251
        // MintOrReuseSpriteForTexture / EditorAppFrame.cpp:1139-1153), not an
        // in-place swap of this asset's `texture` field.
        ImGui::TextDisabled("%s", m_data.texture.IsValid()
                                       ? m_data.texture.ToString().c_str()
                                       : "(none)");

        ImGui::Separator();
        nvrhi::TextureHandle tex;
        if (m_services.assets && m_data.texture.IsValid())
            tex = m_services.assets->GetTexture(Arcane::AssetId::FromGuid(m_data.texture));
        if (tex)
        {
            const auto& desc = tex->getDesc();
            if (desc.width > 0 && desc.height > 0)
            {
                // Scale to fit the available width, preserving aspect.
                const float availW = ImGui::GetContentRegionAvail().x;
                const float drawW = availW;
                const float drawH = availW * (static_cast<float>(desc.height) /
                                              static_cast<float>(desc.width));
                const ImVec2 imgPos = ImGui::GetCursorScreenPos();
                // ImTextureID convention: the raw ITexture* cast to uintptr_t
                // (ShaderEditorDocument.cpp:2523,2570; the backend keys its
                // per-frame SRV binding-set cache on that same pointer,
                // ImGuiNvrhi.cpp:246-252) -- any live ITexture* works, no
                // per-document binding setup needed.
                ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex.Get())),
                            ImVec2(drawW, drawH));

                // Outline the resolved sub-rect. Reuses ComputeSpriteGeom
                // (SpriteAsset.cpp:101-117) -- the SAME function
                // SpriteCache::Request calls at Render/SpriteCache.cpp:88 to build
                // what actually renders -- rather than re-deriving the
                // fullRect/pos/size branch here, so this outline can never
                // drift out of sync with the real geometry (including its
                // fullRect rule, sourceSize.x<=0.0f || sourceSize.y<=0.0f at
                // SpriteAsset.cpp:109, which is slightly wider than the
                // doc's literal "(0,0) = whole texture" -- e.g. a negative
                // sourceSize also reads as full-rect, and this outline will
                // draw it that way too since it shares the same call).
                // uvMin/uvMax are normalized [0,1] texture-space fractions,
                // so multiplying them by the DRAWN width/height (not the
                // texture's pixel size) maps them straight into the image's
                // on-screen rect at whatever scale it was fit to.
                const Arcane::ResolvedSpriteGeom geom =
                    Arcane::ComputeSpriteGeom(m_data, desc.width, desc.height);
                const ImVec2 rectMin(imgPos.x + geom.uvMin.x * drawW,
                                     imgPos.y + geom.uvMin.y * drawH);
                const ImVec2 rectMax(imgPos.x + geom.uvMax.x * drawW,
                                     imgPos.y + geom.uvMax.y * drawH);
                // Positional order is (p_min, p_max, col, rounding, thickness,
                // flags) -- imgui.h:3467. 1.92.8 SWAPPED thickness and flags
                // from the older signature -- the obsolete forwarding overload
                // at imgui.h:3556 (live in this build; IMGUI_DISABLE_OBSOLETE_
                // FUNCTIONS is unset) documents the swap and correctly
                // re-forwards a full 6-arg old-order call; a partial-arg
                // old-order call (e.g. a bare 0 for rounding then a bare
                // thickness value) still silently binds to the wrong
                // parameter under this vendored version.
                ImGui::GetWindowDrawList()->AddRect(rectMin, rectMax,
                    IM_COL32(255, 210, 60, 255), 0.0f, 2.0f);
            }
        }
        else
        {
            ImGui::TextDisabled("(no texture)");
        }

        ImGui::End();
        requestClose = !open;
    }
}
