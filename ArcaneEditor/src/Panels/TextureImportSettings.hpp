#pragma once

// The texture import settings block (inspector filters spec 2026-09-29 s6):
// the four .meta knobs the Inspector's texture-asset panel used to draw
// (F2b Task 13), moved out of EditorPanels.cpp so the Asset page
// (AssetInspectorSource.cpp's DrawAssetPage) draws it. The PURE read/
// merge-write of the sidecar stays in TextureMetaPanel.hpp; this is the ImGui
// half over it.

#include <filesystem>

namespace Arcane::Editor
{
    class PropertyGrid;

    // Draws the ".png only" note, or the four knobs as PropertyGrid rows
    // (s5.6); merge-written on commit, no undo, to the `.meta` sidecar
    // (`sourcePath` + ".meta"). `sourcePath` is the asset's resolved SOURCE
    // file (Project::ResolveAsset). Draw it OUTSIDE a Rows scope: it opens its
    // own ("##texmeta").
    void DrawTextureImportSettings(PropertyGrid& grid, const std::filesystem::path& sourcePath);
}
