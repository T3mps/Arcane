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
        Arcane::AssetPipeline::TextureMetaSettings settings = ReadTextureMetaSettingsDisplay(metaPath);
        bool changed = false;
        static constexpr const char* kFormats[] = { "Auto", "Bc7", "Rgba8" };
        if (const int f = grid.ComboRow("Format", kFormats, 3, static_cast<int>(settings.format)); f >= 0)
        {
            settings.format = static_cast<Arcane::AssetPipeline::TextureMetaSettings::Format>(f);
            changed = true;
        }
        changed |= grid.CheckboxRow("sRGB", settings.srgb);
        changed |= grid.CheckboxRow("Generate Mips", settings.generateMips);
        int maxSize = static_cast<int>(settings.maxSize);
        changed |= grid.IntRow("Max Size", maxSize, Astra::Range(0.0, 16384.0, 1.0));   // true once per gesture
        ImGui::SetItemTooltip("0 = unlimited");
        settings.maxSize = maxSize > 0 ? static_cast<std::uint32_t>(maxSize) : 0;
        if (changed)
            WriteTextureMetaSettingsMerged(metaPath, settings);   // one write per commit, as before
    }
}
