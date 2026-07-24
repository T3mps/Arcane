#pragma once

// DocumentHost: the editor's open-document list + the unsaved-close confirm
// state machine + the asset-type -> open-document routing registry (shader-
// editor Slice 5, spec Fold 3). The close flow is PURE state (unit-tested
// headless, ConsoleBuffer pattern); DrawAll is the only ImGui-touching method
// (windows + the confirm modal). One pending close at a time -- a second
// request while one is pending is ignored (the modal is app-modal anyway).

#include "EditorDocument.hpp"

#include <Arcane/Guid.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    class DocumentHost
    {
    public:
        using OpenFactory =
            std::function<std::unique_ptr<EditorDocument>(const std::filesystem::path&)>;

        // ---- routing (Fold 3 skeleton) ----------------------------------
        // Register a factory for an asset extension (".armat"). Lowercase match.
        void RegisterFactory(std::string extension, OpenFactory factory);
        // Open a path through its extension's factory. Focus-not-reopen: when a
        // document with the same asset Guid is already open, returns it instead.
        // Null when no factory matches or the factory fails.
        EditorDocument* OpenPath(const std::filesystem::path& path);

        // ---- list --------------------------------------------------------
        EditorDocument* Add(std::unique_ptr<EditorDocument> doc);
        EditorDocument* FindByGuid(const Arcane::Guid& guid);
        std::size_t Count() const { return m_docs.size(); }
        bool AnyDirty() const;

        // ---- close flow (pure, testable) ----------------------------------
        // Ask to close `doc`: clean documents close immediately; dirty ones
        // park as the pending confirm (resolved by the Confirm*/CancelClose
        // calls -- DrawAll renders them as a modal).
        void RequestClose(EditorDocument* doc);
        bool HasPendingConfirm() const { return m_pendingClose != nullptr; }
        EditorDocument* PendingConfirmDoc() const { return m_pendingClose; }
        // Save-then-close; a FAILED save keeps the document open (and pending,
        // so the UI can surface the failure rather than dropping edits).
        void ConfirmSaveAndClose();
        void ConfirmDiscard();   // close without saving
        void CancelClose();      // keep the document open

        // ---- per-frame -----------------------------------------------------
        void TickAll(double dt);
        // Draw every document window + the pending-close confirm modal.
        void DrawAll();

        // Visit every open document (compile-result routing and the like).
        template <typename F>
        void ForEach(F&& fn)
        {
            for (const auto& d : m_docs)
                fn(*d);
        }

    private:
        void Close(EditorDocument* doc);   // erase from the list (destroys it)

        std::vector<std::unique_ptr<EditorDocument>> m_docs;
        std::vector<std::pair<std::string, OpenFactory>> m_factories;
        EditorDocument* m_pendingClose = nullptr;
    };
}
