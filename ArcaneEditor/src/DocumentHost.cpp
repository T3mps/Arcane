#include "DocumentHost.hpp"

#include <Arcane/Base/Log.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace Arcane::Editor
{
    namespace
    {
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }
    }

    void DocumentHost::RegisterFactory(std::string extension, OpenFactory factory,
                                       PeekGuid peek)
    {
        m_factories.push_back(Route{ std::move(extension), std::move(factory),
                                     std::move(peek) });
    }

    EditorDocument* DocumentHost::OpenPath(const std::filesystem::path& path)
    {
        const std::string ext = LowerExt(path);
        for (const Route& route : m_factories)
        {
            if (route.ext != ext)
                continue;
            // Focus-not-reopen, cheap form first (review m4): resolve the asset
            // identity WITHOUT constructing a document -- a discarded construct
            // is not free (compile submits on the live doc's coalesce keys).
            // Every path below arms m_focusRequest: "open this asset" means the
            // user wants to be LOOKING AT it, whether that opens a window or
            // re-surfaces one already open behind another tab.
            if (route.peek)
                if (const Arcane::Guid peeked = route.peek(path); peeked.IsValid())
                    if (EditorDocument* open = FindByGuid(peeked))
                        return m_focusRequest = open;
            std::unique_ptr<EditorDocument> doc = route.factory(path);
            if (!doc)
                return nullptr;   // factory already logged the cause
            // Fallback dedup for peek-less routes.
            if (doc->AssetGuid().IsValid())
                if (EditorDocument* open = FindByGuid(doc->AssetGuid()))
                    return m_focusRequest = open;
            return m_focusRequest = Add(std::move(doc));
        }
        ARC_WARN("DocumentHost: no editor registered for '{}'", ext);
        return nullptr;
    }

    EditorDocument* DocumentHost::Add(std::unique_ptr<EditorDocument> doc)
    {
        m_docs.push_back(std::move(doc));
        return m_docs.back().get();
    }

    EditorDocument* DocumentHost::FindByGuid(const Arcane::Guid& guid)
    {
        for (const auto& d : m_docs)
            if (d->AssetGuid() == guid)
                return d.get();
        return nullptr;
    }

    bool DocumentHost::AnyDirty() const
    {
        for (const auto& d : m_docs)
            if (d->Dirty())
                return true;
        return false;
    }

    void DocumentHost::RequestClose(EditorDocument* doc)
    {
        if (!doc || m_pendingClose)
            return;   // one confirm at a time
        if (!doc->Dirty())
        {
            Close(doc);
            return;
        }
        m_pendingClose = doc;
    }

    void DocumentHost::ConfirmSaveAndClose()
    {
        if (!m_pendingClose)
            return;
        if (!m_pendingClose->Save())
            return;   // failed/refused save keeps it open AND pending
        EditorDocument* doc = m_pendingClose;
        m_pendingClose = nullptr;
        Close(doc);
    }

    void DocumentHost::ConfirmDiscard()
    {
        if (!m_pendingClose)
            return;
        EditorDocument* doc = m_pendingClose;
        m_pendingClose = nullptr;
        Close(doc);
    }

    void DocumentHost::CancelClose()
    {
        m_pendingClose = nullptr;
    }

    void DocumentHost::CloseAll()
    {
        m_pendingClose = nullptr;
        m_docs.clear();
        m_dockPlaced.clear();
    }

    void DocumentHost::Close(EditorDocument* doc)
    {
        m_dockPlaced.erase(doc);   // a reopen docks fresh again
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(),
                                    [doc](const auto& d) { return d.get() == doc; }),
                     m_docs.end());
    }

    void DocumentHost::TickAll(double dt)
    {
        for (const auto& d : m_docs)
            d->Tick(dt);
    }

    void DocumentHost::DrawAll(unsigned int dockId)
    {
        // Snapshot the pointers: a close request mutates m_docs after the loop.
        std::vector<EditorDocument*> toClose;
        for (const auto& d : m_docs)
        {
            // First draw after OPENING forces the document into the target
            // node (Cond_Always -- the imgui.ini remembers pre-docking
            // floating placements for reopened assets, so FirstUseEver would
            // silently lose). One frame only: afterwards the user's drags own
            // the window for the document's lifetime.
            if (dockId != 0 && m_dockPlaced.insert(d.get()).second)
                ImGui::SetNextWindowDockID(static_cast<ImGuiID>(dockId),
                                           ImGuiCond_Always);
            // The CENTER document is the thing the user types into, so it gets
            // real window focus -- which is also what makes its dock node
            // select its tab (imgui.cpp:19611-19613 applies g.NavWindow back as
            // the node's selection). Side panels must NOT use this; they switch
            // by tab selection alone (EditorPanels' SelectDockTab), or the last
            // focus call in the frame steals the center selection from here.
            if (d.get() == m_focusRequest)
            {
                ImGui::SetNextWindowFocus();
                m_focusRequest = nullptr;
            }
            bool requestClose = false;
            d->Draw(requestClose);
            if (requestClose)
                toClose.push_back(d.get());
        }
        for (EditorDocument* d : toClose)
            RequestClose(d);

        if (m_pendingClose)
        {
            ImGui::OpenPopup("Unsaved Changes##dochost");
            if (ImGui::BeginPopupModal("Unsaved Changes##dochost", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Save changes to '%s' before closing?",
                            m_pendingClose->Title().c_str());
                ImGui::Separator();
                if (ImGui::Button("Save"))
                {
                    ImGui::CloseCurrentPopup();
                    ConfirmSaveAndClose();
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard"))
                {
                    ImGui::CloseCurrentPopup();
                    ConfirmDiscard();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    ImGui::CloseCurrentPopup();
                    CancelClose();
                }
                ImGui::EndPopup();
            }
        }
    }
}
