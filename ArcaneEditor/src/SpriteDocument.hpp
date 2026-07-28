#pragma once

// SpriteDocument: the .arcsprite editor (sprite-asset arc, Task 5) -- the
// SECOND EditorDocument (ShaderEditorDocument, ~4k lines, is the first,
// Slice 5). Deliberately compact next to that one: a sprite asset is five
// scalar/vec2 fields (SpriteAssetData, SpriteAsset.hpp:30-39), so there is
// no compile pipeline, no undo-anchored param graph, no async work at all --
// just a form over SpriteAssetData plus a texture preview with the resolved
// sub-rect outlined. Implements EditorDocument's five pure virtuals
// (EditorDocument.hpp:15-32); DocumentHost owns the open-document list, the
// asset-type -> factory routing, and the unsaved-close confirm modal --
// this class only ever flips m_dirty and answers Save()/Draw() truthfully
// (same division of responsibility ShaderEditorDocument follows, see its
// Draw() at ShaderEditorDocument.cpp:1001-1066: requestClose mirrors `open`
// from ImGui::Begin, both on the collapsed-early-return path and the normal
// end-of-frame path -- DocumentHost.cpp:151-157 is what actually turns that
// into a close or a pending confirm).

#include "EditorDocument.hpp"

#include <Arcane/Guid.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace Arcane { class Assets; }

namespace Arcane::Editor
{
    class SpriteDocument final : public EditorDocument
    {
    public:
        // Everything the document borrows from the app (outlives the host's
        // document list -- same "services struct" shape as
        // ShaderEditorDocument's DocServices, ShaderEditorDocument.hpp:52-71,
        // just with far less in it: a sprite has no compiler/undo/clock).
        struct Services
        {
            Arcane::Assets* assets = nullptr;   // GetTexture(AssetId) for the preview
            // Fired after a successful Save with the asset's Guid -- lets the
            // app's SpriteCache drop its cached resolve (SpriteCache.hpp:
            // 60-67 Invalidate), so the NEXT Request() re-reads the saved
            // file. This is the whole mechanism that makes an edit show up
            // in the viewport (SpriteCache::Request is otherwise a
            // once-per-Guid cache, SpriteCache.cpp:20).
            std::function<void(const Arcane::Guid&)> invalidateSprite;
        };

        // `data` is already loaded (LoadSpriteAsset happens in the factory,
        // which returns null on failure -- same split as
        // ShaderEditorDocument(DocServices, path, MaterialAssetData),
        // ShaderEditorDocument.hpp:76-77 / EditorApp.cpp's materialFactory).
        SpriteDocument(Services services, std::filesystem::path path,
                       Arcane::SpriteAssetData data);

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        bool Dirty() const override { return m_dirty; }
        bool Save() override;
        void Draw(bool& requestClose) override;

    private:
        Services                 m_services;
        std::filesystem::path    m_path;
        Arcane::SpriteAssetData  m_data;
        std::string              m_title;         // display name (Title())
        std::string              m_windowLabel;    // "name###spritedoc_<guid>" (stable across rename)
        bool                     m_dirty = false;  // set by any field edit; cleared only by a SUCCESSFUL Save
    };
}
