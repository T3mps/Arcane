#include "Panels/TextureImportSettings.hpp"

#include "Panels/TextureMetaPanel.hpp"   // the .meta "texture" block's PURE read/merge-write (F2b Task 13)

#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>   // the .meta "texture" block's four knobs (F2b Task 13)

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

// F2b Task 13's texture import settings, moved verbatim out of
// EditorPanels.cpp (inspector filters Task 5): the Inspector's texture-asset
// panel became the Asset page's import-settings block. Spec sec 4's four
// .meta knobs exactly, never UE's eighty.
namespace Arcane::Editor
{
    namespace
    {
        // Case-insensitive extension compare -- same rule AssetRegistry.cpp's
        // own LowerExt uses, duplicated rather than shared (that one is file-
        // local too; ArcaneEditor has at least two independent copies of this
        // exact idiom already, an acknowledged stylistic duplication).
        bool HasExtensionCI(const std::filesystem::path& p, std::string_view want)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == want;
        }

        // The four spec sec 4 knobs -- pinned, nothing more (the ceiling to
        // grow into is UE's eight-ish core, never the eighty). Returns true
        // on any edit THIS frame -- the caller merge-writes on that edge
        // only, not every frame the block happens to be drawn.
        bool DrawTextureMetaSettingsBlock(Arcane::AssetPipeline::TextureMetaSettings& settings)
        {
            bool changed = false;
            int format = static_cast<int>(settings.format);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("Format##texmeta", &format, "Auto\0Bc7\0Rgba8\0"))
            {
                settings.format =
                    static_cast<Arcane::AssetPipeline::TextureMetaSettings::Format>(format);
                changed = true;
            }
            if (ImGui::Checkbox("sRGB##texmeta", &settings.srgb))
                changed = true;
            if (ImGui::Checkbox("Generate Mips##texmeta", &settings.generateMips))
                changed = true;
            int maxSize = static_cast<int>(settings.maxSize);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragInt("Max Size (0 = unlimited)##texmeta", &maxSize, 1.0f, 0, 16384);
            // Minor fix (final-review wave, 2026-09-04): DragInt returns true on EVERY
            // frame the value changes WHILE the drag is active -- against this function's
            // OWN "true on any edit THIS frame" contract, a single drag gesture used to
            // report `changed` (and so trigger the caller's sidecar write, and so the
            // watcher's cook trigger) on every intermediate tick, not once per gesture.
            // Keep the LIVE value flowing into `settings` every frame regardless (ImGui's
            // own internal drag accumulator is what keeps the widget tracking the mouse
            // smoothly -- it does not depend on the caller persisting intermediate values),
            // but only report the edit -- the caller's actual write signal -- once the item
            // DEACTIVATES after an edit (mouse release / Enter): one write per gesture.
            settings.maxSize = maxSize > 0 ? static_cast<std::uint32_t>(maxSize) : 0;
            if (ImGui::IsItemDeactivatedAfterEdit())
                changed = true;
            return changed;
        }
    }

    void DrawTextureImportSettings(const std::filesystem::path& sourcePath)
    {
        // The four .meta knobs apply to a COOKABLE source only: ".png" is
        // the one extension CookSession.cpp's EnumerateTextureSources
        // actually cooks this slice, even though AssetKindOf classifies
        // several other extensions Texture too (.jpg/.tga/.bmp/.hdr).
        // Anything else shows no editing surface rather than a block
        // that silently writes settings nothing will ever read.
        if (!HasExtensionCI(sourcePath, ".png"))
        {
            ImGui::TextDisabled("import settings apply to .png sources only");
            return;
        }

        std::filesystem::path metaPath = sourcePath;
        metaPath += ".meta";
        Arcane::AssetPipeline::TextureMetaSettings settings =
            ReadTextureMetaSettingsDisplay(metaPath);
        if (DrawTextureMetaSettingsBlock(settings))
            WriteTextureMetaSettingsMerged(metaPath, settings);
    }
}
