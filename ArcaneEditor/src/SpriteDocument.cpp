#include "SpriteDocument.hpp"

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Project/AssetId.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>

#include <cfloat>
#include <utility>

namespace Arcane::Editor
{
    SpriteDocument::SpriteDocument(Services services, std::filesystem::path path,
                                   Arcane::SpriteAssetData data)
        : m_services(std::move(services)), m_path(std::move(path)), m_data(std::move(data))
    {
        // Same name-fallback rule as ShaderEditorDocument (ShaderEditorDocument.cpp:319):
        // an empty asset name (hand-authored file, or a pre-name-field asset)
        // falls back to the file stem rather than showing a blank title.
        m_title = m_data.name.empty() ? m_path.stem().string() : m_data.name;
        m_windowLabel = m_title + " (Sprite)###spritedoc_" + m_data.id.ToString();
    }

    bool SpriteDocument::Save()
    {
        if (!Arcane::SaveSpriteAsset(m_path, m_data))
            return false;   // failed save: m_dirty stays set, edits are not silently lost
        m_dirty = false;
        // The SpriteCache resolve this replaces is otherwise permanent (once-per-Guid,
        // SpriteCache.cpp:20) -- without this call the viewport keeps showing the
        // PRE-edit geometry/texture until something else evicts the entry.
        if (m_services.invalidateSprite)
            m_services.invalidateSprite(m_data.id);
        return true;
    }

    void SpriteDocument::Draw(bool& requestClose)
    {
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_FirstUseEver);
        const ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, flags))
        {
            // Collapsed (not closed): ShaderEditorDocument's same early-return
            // shape (ShaderEditorDocument.cpp:1006-1011) -- `open` only goes
            // false when the titlebar X was clicked, so a merely-collapsed
            // window still reports requestClose=false here.
            ImGui::End();
            requestClose = !open;
            return;
        }

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
        // drag's bound" concern (EditorPanels.cpp:1096-1103's RangedDragFloat).
        bool changed = false;
        changed |= ImGui::DragFloat("Pixels Per Meter", &m_data.ppu, 0.5f, 1.0f, 4096.0f,
                                    "%.3f", ImGuiSliderFlags_ClampOnInput);
        changed |= ImGui::DragFloat2("Source Pos", &m_data.sourcePos.x, 1.0f, 0.0f, FLT_MAX,
                                     "%.3f", ImGuiSliderFlags_ClampOnInput);
        changed |= ImGui::DragFloat2("Source Size", &m_data.sourceSize.x, 1.0f, 0.0f, FLT_MAX,
                                     "%.3f", ImGuiSliderFlags_ClampOnInput);
        ImGui::TextDisabled("(0, 0) = whole texture");
        changed |= ImGui::DragFloat2("Pivot", &m_data.pivot.x, 0.005f, 0.0f, 1.0f,
                                     "%.3f", ImGuiSliderFlags_ClampOnInput);
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
                // (ShaderEditorDocument.cpp:1486,1501; the backend keys its
                // per-frame SRV binding-set cache on that same pointer,
                // ImGuiNvrhi.cpp:246-252) -- any live ITexture* works, no
                // per-document binding setup needed.
                ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex.Get())),
                            ImVec2(drawW, drawH));

                // Outline the resolved sub-rect. Reuses ComputeSpriteGeom
                // (SpriteAsset.cpp:101-117) -- the SAME function
                // SpriteCache::Request calls at SpriteCache.cpp:71 to build
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
