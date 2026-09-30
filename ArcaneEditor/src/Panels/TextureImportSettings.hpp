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
    // Draws the ".png only" note, or the four knobs for a .png source, and
    // merge-writes the `.meta` sidecar (`sourcePath` + ".meta") on an edit.
    // `sourcePath` is the asset's resolved SOURCE file (Project::ResolveAsset).
    void DrawTextureImportSettings(const std::filesystem::path& sourcePath);
}
