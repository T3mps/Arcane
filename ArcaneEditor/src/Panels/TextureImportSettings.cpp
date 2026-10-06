#include "Panels/TextureImportSettings.hpp"

#include "Panels/TextureMetaPanel.hpp"   // the .meta "texture" block's PURE read/merge-write (F2b Task 13)
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>   // the .meta "texture" block's four knobs (F2b Task 13)

#include <Astra/Reflection/Attribute.hpp>   // Astra::Range (Max Size)
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

// F2b Task 13's texture import settings, moved out of
// EditorPanels.cpp (inspector filters Task 5): the Inspector's texture-asset
// panel became the Asset page's import-settings block. Spec sec 4's four
// .meta knobs exactly, never UE's eighty; PropertyGrid rows since node-page s5.6.
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
    }

    // The four spec sec 4 knobs -- pinned, nothing more (the ceiling to grow
    // into is UE's eight-ish core, never the eighty) -- as PropertyGrid rows
    // (node-page s5.6). Each row reports a COMMIT, not a live tick: the
    // checkboxes and the combo on the click, Max Size once per gesture
    // (IntRow's deactivate-after-edit contract) -- so the sidecar is
    // merge-written once per edit, never per drag frame.
    void DrawTextureImportSettings(PropertyGrid& grid, const std::filesystem::path& sourcePath)
    {
        PropertyGrid::Rows rows(grid, "##texmeta");
        if (!rows)
            return;
        // The four .meta knobs apply to a COOKABLE source only: ".png" is
        // the one extension CookSession.cpp's EnumerateTextureSources
        // actually cooks this slice, even though AssetKindOf classifies
        // several other extensions Texture too (.jpg/.tga/.bmp/.hdr).
        // Anything else shows no editing surface rather than a block
        // that silently writes settings nothing will ever read.
        if (!HasExtensionCI(sourcePath, ".png"))
        {
            grid.ReadOnlyRow("Import", "import settings apply to .png sources only");
            return;
        }

        std::filesystem::path metaPath = sourcePath;
        metaPath += ".meta";
        // Settings arc S6-6: a field the .meta leaves out shows the project's
        // assets.import.texture.* value (what the cook resolves it to) with a
        // "(project default)" tooltip; a field the .meta sets carries the reset
        // arrow, which removes it so it follows the project again. An edit writes
        // only the fields the .meta already set plus the edited one. No label
        // change and no extra row: a row's label is its ImGui id, and the page's
        // 1080p fit has no height to spare.
        using TextureMetaSettings = Arcane::AssetPipeline::TextureMetaSettings;
        TextureMetaSettings::FieldsSet set;
        TextureMetaSettings settings =
            ReadTextureMetaSettingsDisplay(metaPath, Arcane::Settings<TextureMetaSettings>(), &set);
        TextureMetaSettings::FieldsSet write = set;
        bool changed = false;
        const auto decorate = [&grid](bool isSet) {
            grid.SetNextRowDecor(RowDecor{ .reset = true, .resetActive = isSet });
        };
        // After the row: an edit pins the field, a reset unpins it, and an absent
        // field's value widget says where its value comes from.
        const auto settle = [&grid, &changed](bool edited, bool isSet, bool& field, const char* tip) {
            if (edited)
                field = true;
            else if (grid.LastRowEvents().resetClicked)
                field = false;
            changed |= edited || grid.LastRowEvents().resetClicked;
            if (!isSet)
                ImGui::SetItemTooltip("%s%s(project default: Project Settings > assets.import.texture)", tip,
                                      *tip ? "\n" : "");
            else if (*tip)
                ImGui::SetItemTooltip("%s", tip);
        };
        static constexpr const char* kFormats[] = { "Auto", "Bc7", "Rgba8" };
        decorate(set.format);
        const int f = grid.ComboRow("Format", kFormats, 3, static_cast<int>(settings.format));
        if (f >= 0)
            settings.format = static_cast<TextureMetaSettings::Format>(f);
        settle(f >= 0, set.format, write.format, "");
        decorate(set.srgb);
        settle(grid.CheckboxRow("sRGB", settings.srgb), set.srgb, write.srgb, "");
        decorate(set.generateMips);
        settle(grid.CheckboxRow("Generate Mips", settings.generateMips), set.generateMips, write.generateMips, "");
        int maxSize = static_cast<int>(settings.maxSize);
        decorate(set.maxSize);
        const bool maxSizeEdited = grid.IntRow("Max Size", maxSize, Astra::Range(0.0, 16384.0, 1.0));   // true once per gesture
        settle(maxSizeEdited, set.maxSize, write.maxSize, "0 = unlimited");
        settings.maxSize = maxSize > 0 ? static_cast<std::uint32_t>(maxSize) : 0;
        if (changed)
            WriteTextureMetaSettingsMerged(metaPath, settings, &write);   // one write per commit, as before
    }
}
