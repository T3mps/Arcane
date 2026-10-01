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
#include "Scene/UndoGate.hpp"
#include "Documents/DocumentPageSelection.hpp"   // the page's one key + open/click epoch
#include "Documents/EditorDocument.hpp"

#include <Arcane/Guid.hpp>
#include <Arcane/Sprite/SpriteAsset.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// CommandStack arrives complete via EditGesture.hpp (which includes
// <Arcane/Edit/CommandStack.hpp>) -- Services holds a pointer to one.
namespace Arcane { class Assets; }

namespace Arcane::Editor
{
    struct AssetRefServices;   // Panels/AssetReferenceField.hpp (Services::assetRefs)

    class SpriteDocument final : public EditorDocument
    {
    public:
        // Everything the document borrows from the app (outlives the host's
        // document list -- same "services struct" shape as
        // ShaderEditorDocument's DocServices, ShaderEditorDocument.hpp:85-104,
        // just with far less in it: a sprite has no compiler and no clock,
        // because it has nothing async to drive. `undo` resolves to the same one shared
        // editor CommandStack every other surface pushes to (the app hands the
        // same resolver to DocServices::undo, EditorApp::DocumentUndo), so a
        // sprite field edit is one step in the ONE global history -- Ctrl+Z
        // walks back through it exactly like an Inspector or graph edit.
        struct Services
        {
            // The texture's true dims (TextureInfoFor: an artifact HEADER read,
            // memoized) so Draw can crop the thumbnail to the sprite's rect.
            // Null = the whole texture is shown.
            Arcane::Assets* assets = nullptr;
            // The editor chrome's thumbnail seam (the Asset Browser rows'
            // resolveAssetThumb): the sprite's TEXTURE guid -> an ImGui
            // texture id owned by the chrome texture cache, 0 = not yet
            // available. Null (every headless test) = no image, hint only.
            std::function<std::uint64_t(const Arcane::Guid&)> resolveThumb;
            // An asset guid -> its display name (the texture line). Null or
            // "" = the guid is printed.
            std::function<std::string(const Arcane::Guid&)> assetName;
            // Asked per edit (UndoStack()). Unset, or returning null (Play), =
            // no undo coverage (the EditGesture bracket then no-ops whole) --
            // the document still edits and saves, so a missing stack degrades
            // to "no history", never to a lost edit.
            UndoResolver undo;   // the ONE history, resolved per edit; returns null in Play (s3.3b)
            // Fired after a successful Save with the asset's Guid -- lets the
            // app's SpriteCache drop its cached resolve
            // (Render/SpriteCache.hpp:69-76 Invalidate), so the NEXT Request()
            // re-reads the saved file. This is the whole mechanism that makes
            // an edit show up in the viewport (SpriteCache::Request is
            // otherwise a once-per-Guid cache, Render/SpriteCache.cpp:37).
            std::function<void(const Arcane::Guid&)> invalidateSprite;

            // The shared asset-reference cell's services (spec 2026-09-30 s4.2):
            // EditorApp::m_assetRefServices, app-lifetime; its callables read state
            // at call time, so a document made during a boot stage is not stale.
            // Null in the headless tests (the cell's null services). T3's ports read it.
            const AssetRefServices* assetRefs = nullptr;
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

        // ---- Inspector source (inspector filters spec s6a) ----------------
        // Kind "sprite". ONE page, the whole sprite form (the four drags and
        // the read-only Texture line), under ONE key, "sprite": opening the
        // document selects it (m_pageSel starts at epoch 1) and a click in the
        // document's content re-selects it (Draw's NoteContentClick). Tab
        // switches and focus never do (the spec's one selection rule). The
        // document window keeps its toolbar, a "Sprite properties are in the
        // Inspector" hint, the texture line and the sprite image (final fix D).
        std::string_view Kind() const override { return "sprite"; }
        InspectorPage* Page() override { return &m_page; }
        InspectorPage* PageFor(std::string_view key) override { return m_pageSel.Resolves(key) ? &m_page : nullptr; }
        std::string SelectionKey() const override { return m_pageSel.SelectionKey(); }
        // True when it resolves; NO epoch bump -- a restore is not a click.
        bool RestoreSelection(std::string_view key) override { return m_pageSel.Resolves(key); }
        bool Resolves(std::string_view key) const override { return m_pageSel.Resolves(key); }
        std::uint64_t SelectionEpoch() const override { return m_pageSel.epoch; }
        void NoteReopened() override { m_pageSel.NoteReopened(); }
        void FlushGesture() override;

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
        // the undo units never draw (Draw and the page's DrawFormBody are the
        // only ImGui methods), so this is how they observe what a command did.
        const Arcane::SpriteAssetData& Data() const noexcept { return m_data; }

    private:
        // The sprite page: the form, drawn by the Inspector instance showing
        // it. Carries its own EditGesture::ScopeGuard (the drags that open
        // gestures are submitted inside it) and no Begin/End -- the Inspector
        // window is its window.
        void DrawFormBody();

        // The one page this document contributes (kind "sprite", key
        // "sprite"). The base MUST be public: Page() hands &m_page out as
        // InspectorPage*, and a private base makes that conversion
        // inaccessible (MSVC C2243).
        class SpriteInspectorPage final : public InspectorPage
        {
        public:
            explicit SpriteInspectorPage(SpriteDocument& doc) : m_doc(doc) {}
            std::vector<InspectorCrumb> Breadcrumb() const override
            {
                // One crumb; `select` is a no-op (the page IS the only level).
                return { InspectorCrumb{ m_doc.m_title, [] {}, std::string{ "sprite" } } };
            }
            void Draw(PropertyGrid&) override { m_doc.DrawFormBody(); }

        private:
            SpriteDocument& m_doc;
        };

        [[nodiscard]] Arcane::CommandStack* UndoStack() const { return m_services.undo ? m_services.undo() : nullptr; }
        Services                 m_services;
        std::filesystem::path    m_path;
        Arcane::SpriteAssetData  m_data;
        std::string              m_title;         // display name (Title())
        std::string              m_windowLabel;    // "name###spritedoc_<guid>" (stable across rename)
        bool                     m_dirty = false;  // set by any field edit; cleared only by a SUCCESSFUL Save
        // Latched each Draw; read by DocumentHost::FocusedDoc so the app's
        // scene-level Ctrl+S stands down while this document is focused.
        bool                     m_windowFocused = false;

        // The document's ONE edit-gesture bracket. All four field drags share
        // it: only one can hold ActiveId at a time, and EditGesture's
        // ownership guard is what keeps the other three from closing it. TWO
        // draw scopes hold a ScopeGuard on it as their first local -- Draw
        // (the document window, whose Begin can refuse on a background tab)
        // and DrawFormBody (the Inspector page, where the drags now live and
        // which draws AFTER the documents, inside an Inspector window's
        // Begin/End) -- the same two-guard shape ShaderEditorDocument's Draw +
        // DrawMaterialPageBody hold on its own gesture.
        EditGesture::GestureState m_gesture;

        // Doc-identity handle for undo steps, mirroring ShaderEditorDocument's
        // m_anchor (ShaderEditorDocument.hpp:446-449, minted
        // ShaderEditorDocument.cpp:955): commands hold this WEAKLY and forward
        // through the pointee, so steps left on the shared stack after this
        // document closes go inert instead of dereferencing a dead `this`.
        std::shared_ptr<SpriteDocument*> m_anchor;

        // The Inspector page's selection (key "sprite", epoch 1 = selected at
        // open) and the page itself. m_page holds a reference to *this;
        // documents live behind unique_ptr in DocumentHost and never move.
        DocumentPageSelection  m_pageSel{ "sprite" };
        SpriteInspectorPage    m_page{ *this };
    };
}
