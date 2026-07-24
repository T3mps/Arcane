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

    void DocumentHost::RegisterFactory(std::string extension, OpenFactory factory)
    {
        m_factories.emplace_back(std::move(extension), std::move(factory));
    }

    EditorDocument* DocumentHost::OpenPath(const std::filesystem::path& path)
    {
        const std::string ext = LowerExt(path);
        for (const auto& [registered, factory] : m_factories)
        {
            if (registered != ext)
                continue;
            std::unique_ptr<EditorDocument> doc = factory(path);
            if (!doc)
                return nullptr;   // factory already logged the cause
            // Focus-not-reopen: an already-open document for the same asset wins.
            if (doc->AssetGuid().IsValid())
                if (EditorDocument* open = FindByGuid(doc->AssetGuid()))
                    return open;
            return Add(std::move(doc));
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

    void DocumentHost::Close(EditorDocument* doc)
    {
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(),
                                    [doc](const auto& d) { return d.get() == doc; }),
                     m_docs.end());
    }

    void DocumentHost::TickAll(double dt)
    {
        for (const auto& d : m_docs)
            d->Tick(dt);
    }

    void DocumentHost::DrawAll()
    {
        // Snapshot the pointers: a close request mutates m_docs after the loop.
        std::vector<EditorDocument*> toClose;
        for (const auto& d : m_docs)
        {
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
