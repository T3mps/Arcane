#pragma once

// SpriteDocument: the .arcsprite editor (sprite-asset arc, Task 5) -- the
// SECOND EditorDocument (ShaderEditorDocument, ~4k lines, is the first,
// Slice 5). Deliberately compact next to that one: a sprite asset is five
// scalar/vec2 fields (SpriteAssetData, SpriteAsset.hpp:30-39), so there is
// no compile pipeline, no async work at all -- just a form over
// SpriteAssetData plus a texture preview with the resolved sub-rect
// outlined. Its field drags DO ride the shared undo history (widget-layer
// Task 7): one EditGesture bracket, one whole-data step per gesture, held
// through the same doc-identity anchor ShaderEditorDocument's commands use.
// Implements EditorDocument's five pure virtuals
// (EditorDocument.hpp:15-44); DocumentHost owns the open-document list, the
// asset-type -> factory routing, and the unsaved-close confirm modal --
// this class only ever flips m_dirty and answers Save()/Draw() truthfully
// (same division of responsibility ShaderEditorDocument follows, see its
// Draw() at ShaderEditorDocument.cpp:1715-1847: requestClose mirrors `open`
// from ImGui::Begin, both on the collapsed-early-return path and the normal
// end-of-frame path -- DocumentHost.cpp:174-180 is what actually turns that
// into a close or a pending confirm).

#include "Scene/EditGesture.hpp"
#include "Documents/EditorDocument.hpp"

#include <Arcane/Guid.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// CommandStack arrives complete via EditGesture.hpp (which includes
// <Arcane/Edit/CommandStack.hpp>) -- Services holds a pointer to one.
namespace Arcane { class Assets; }

namespace Arcane::Editor
{
    class SpriteDocument final : public EditorDocument
    {
    public:
        // Everything the document borrows from the app (outlives the host's
        // document list -- same "services struct" shape as
        // ShaderEditorDocument's DocServices, ShaderEditorDocument.hpp:85-104,
        // just with far less in it: a sprite has no compiler and no clock,
        // because it has nothing async to drive. `undo` IS the same one shared
        // editor CommandStack every other surface pushes to (the app hands the
        // same pointer to DocServices::undo, EditorAppProject.cpp:39), so a
        // sprite field edit is one step in the ONE global history -- Ctrl+Z
        // walks back through it exactly like an Inspector or graph edit.
        struct Services
        {
            Arcane::Assets* assets = nullptr;   // GetTexture(AssetId) for the preview
            // Null = no undo coverage (the EditGesture bracket then no-ops
            // whole, EditGesture.hpp:145-146) -- the document still edits and
            // saves, so an unwired stack degrades to "no history", never to a
            // lost edit.
            Arcane::CommandStack* undo = nullptr;
            // Fired after a successful Save with the asset's Guid -- lets the
            // app's SpriteCache drop its cached resolve
            // (Render/SpriteCache.hpp:69-76 Invalidate), so the NEXT Request()
            // re-reads the saved file. This is the whole mechanism that makes
            // an edit show up in the viewport (SpriteCache::Request is
            // otherwise a once-per-Guid cache, Render/SpriteCache.cpp:37).
            std::function<void(const Arcane::Guid&)> invalidateSprite;
        };

        // `data` is already loaded (LoadSpriteAsset happens in the factory,
        // which returns null on failure -- same split as
        // ShaderEditorDocument(DocServices, path, MaterialAssetData),
        // ShaderEditorDocument.hpp:109-110 / EditorApp.cpp's materialFactory).
        SpriteDocument(Services services, std::filesystem::path path,
                       Arcane::SpriteAssetData data);
        ~SpriteDocument() override;   // closes a parked edit gesture

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_data.id; }
        bool Dirty() const override { return m_dirty; }
        bool Save() override;
        bool WindowFocused() const override { return m_windowFocused; }
        void Draw(bool& requestClose) override;

        // Undo plumbing (doc-identity commands, the same shape as
        // ShaderEditorDocument::ApplyParamEdit, ShaderEditorDocument.hpp:
        // 218-223): swap the whole authored data in and republish it exactly
        // the way a Save does, so an undo shows up in the viewport rather than
        // living only inside this window.
        void ApplySpriteData(const Arcane::SpriteAssetData& data);

        // One undo step for a COMPLETED field gesture: `before` is the copy
        // pinned when the widget activated, `after` is m_data as it stands
        // now (the live edit already happened -- the ICommand contract).
        // Pushes nothing when the two match, so a click that moved no value
        // leaves no step behind.
        void PushDataEdit(std::string label, const Arcane::SpriteAssetData& before);

        // The live authored data. Exposed for the headless [editor] units --
        // Draw is the only ImGui method and they never call it, so this is
        // how they observe what a command did.
        const Arcane::SpriteAssetData& Data() const noexcept { return m_data; }

    private:
        Services                 m_services;
        std::filesystem::path    m_path;
        Arcane::SpriteAssetData  m_data;
        std::string              m_title;         // display name (Title())
        std::string              m_windowLabel;    // "name###spritedoc_<guid>" (stable across rename)
        bool                     m_dirty = false;  // set by any field edit; cleared only by a SUCCESSFUL Save
        // Latched each Draw; read by DocumentHost::FocusedDoc so the app's
        // scene-level Ctrl+S stands down while this document is focused.
        bool                     m_windowFocused = false;

        // The document's ONE edit-gesture bracket (the ScopeGuard at the top
        // of Draw is its guaranteed close). All four field drags share it:
        // only one can hold ActiveId at a time, and EditGesture's ownership
        // guard is what keeps the other three from closing it.
        EditGesture::GestureState m_gesture;

        // Doc-identity handle for undo steps, mirroring ShaderEditorDocument's
        // m_anchor (ShaderEditorDocument.hpp:446-449, minted
        // ShaderEditorDocument.cpp:955): commands hold this WEAKLY and forward
        // through the pointee, so steps left on the shared stack after this
        // document closes go inert instead of dereferencing a dead `this`.
        std::shared_ptr<SpriteDocument*> m_anchor;
    };
}
