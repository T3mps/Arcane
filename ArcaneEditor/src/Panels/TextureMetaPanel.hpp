#pragma once

// TextureMetaPanel: the PURE half of the Inspector's texture-asset settings
// block (F2b Task 13) -- reading/merge-writing the ".meta" sidecar's "texture"
// block, separated from the ImGui widgets that edit it
// (EditorPanels.cpp's DrawTextureMetaSettingsBlock/DrawTextureAssetPanel).
// Pure by construction (no ImGui) because the test gate does not compile
// EditorPanels.cpp -- the same InspectorMeta/ConsoleModel split applied to
// this problem, so the [editor] units can drive the merge-preserving write
// directly.

#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>

#include <filesystem>

namespace Arcane::Editor
{
    // Read the `.meta` sidecar's "texture" block, tolerant of everything
    // CookSession.cpp's own ReadSourceMeta already is: a missing/unparsable
    // file, or a missing/malformed "texture" key within it, just means
    // "defaults" -- never throws. A DISPLAY read only: the cook pipeline
    // re-reads the file itself at cook time, so this never becomes a second
    // authority on the settings.
    [[nodiscard]] Arcane::AssetPipeline::TextureMetaSettings ReadTextureMetaSettingsDisplay(
        const std::filesystem::path& metaPath);

    // MERGE-PRESERVING write: re-reads the CURRENT file on disk (never trusts
    // a cached copy -- a hand-edit or the watcher's own recook could have
    // touched it since the caller last read it), replaces ONLY the "texture"
    // key, and writes the whole document back. `guid`/`version`/anything else
    // present in the file survives untouched -- AssetRegistry.cpp's
    // ResolveSidecarId is the sidecar shape this preserves against
    // (`{"guid":..., "version":1}` at minimum; `guid` is load-bearing --
    // losing it would mint a NEW identity for the asset on the next registry
    // scan). A file that fails to read or fails to parse as a JSON object is
    // left ALONE -- never overwritten with a bare `{"texture":...}` that
    // would drop its guid -- this call silently no-ops (a WARN is logged)
    // until the file is fixed by hand. Once written, Task 12's watcher
    // (already wired, no further plumbing needed here) treats the mtime
    // change as a cook trigger exactly like a source edit -- the contract's
    // ergonomic #2.
    void WriteTextureMetaSettingsMerged(const std::filesystem::path& metaPath,
                                        const Arcane::AssetPipeline::TextureMetaSettings& settings);
}
