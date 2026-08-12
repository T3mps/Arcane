#include "Documents/CrashReportDocument.hpp"

#include "Widgets/EditorTheme.hpp"

#include <Arcane/Render/IGpuCrashBackend.hpp>   // Diag::ReadGpuDump / ParseGpuDump

#include <imgui.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW -- mirrors EditorAppFrame.cpp's AssetPathAction show-in-explorer branch
#endif

namespace Arcane::Editor
{
    namespace
    {
        // Same recipe as EditorAppFrame.cpp's AssetPathAction show-in-
        // explorer branch (:144-150): "explorer /select" opens the
        // containing folder WITH the file focused. Duplicated in full
        // (rather than shared) because that helper is a file-local
        // (anonymous-namespace) function in a TU this task's binding file
        // list does not include -- EditorAppFrame.cpp is not among the
        // files this task modifies.
#ifdef _WIN32
        void ShowInExplorer(const std::string& path)
        {
            if (path.empty())
                return;
            const std::filesystem::path p(path);
            const std::wstring args = L"/select,\"" + p.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
#else
        void ShowInExplorer(const std::string&) {}
#endif
    }

    CrashReportDocument::CrashReportDocument(std::filesystem::path path, Arcane::Diag::Envelope envelope)
        : m_path(std::move(path)), m_envelope(std::move(envelope))
    {
        // Same name-fallback shape as ShaderEditorDocument/SpriteDocument,
        // but there is no authored "name" field on a crash report -- the
        // file stem (Diagnostics.cpp's WriteReportImpl: "<appName>-<stamp>-
        // pid<N>") is already unique per report, so it needs no fallback.
        m_title = m_path.stem().string();
        m_windowLabel = m_title + " (Crash Report)###crashdoc_" + m_envelope.guid.ToString();

        // Parsed ONCE here, never in Draw(): the model half a headless test
        // observes (GpuDumpSectionTags) is exactly what gets rendered.
        if (!m_envelope.siblingGpuDump.empty())
        {
            if (const auto dump = Arcane::Diag::ReadGpuDump(std::filesystem::path(m_envelope.siblingGpuDump)))
                for (const auto& section : dump->sections)
                    m_gpuDumpTags.push_back(section.tag);
        }
    }

    bool CrashReportDocument::IsEmptyQueueTimeline(const Arcane::Diag::Envelope::Queue& q) noexcept
    {
        return q.lastCompleted.empty() && q.inFlight.empty();
    }

    bool CrashReportDocument::IsNoiseFault(const Arcane::Diag::Envelope::Fault& f) noexcept
    {
        return f.type.empty() || f.type == "device-alive";
    }

    std::vector<const Arcane::Diag::Envelope::Queue*> CrashReportDocument::VisibleQueues() const
    {
        std::vector<const Arcane::Diag::Envelope::Queue*> out;
        for (const Arcane::Diag::Envelope::Queue& q : m_envelope.queues)
            if (!IsEmptyQueueTimeline(q))
                out.push_back(&q);
        return out;
    }

    void CrashReportDocument::Draw(bool& requestClose)
    {
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(520.0f, 640.0f), ImGuiCond_FirstUseEver);
        // Never dirty -> never ImGuiWindowFlags_UnsavedDocument (unlike
        // SpriteDocument/ShaderEditorDocument, which flip that flag on
        // Dirty()).
        if (!ImGui::Begin(m_windowLabel.c_str(), &open, 0))
        {
            // Collapsed (not closed): same early-return shape as
            // SpriteDocument::Draw (SpriteDocument.cpp:165-175).
            m_windowFocused = false;
            ImGui::End();
            requestClose = !open;
            return;
        }
        m_windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        // ---- header: kind / time / phase / build ---------------------------
        ImGui::TextUnformatted(m_envelope.kind.empty() ? "(unknown kind)" : m_envelope.kind.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_envelope.timestampUtc.c_str());
        if (!m_envelope.phase.empty())
            ImGui::Text("Phase: %s", m_envelope.phase.c_str());
        if (!m_envelope.buildInfo.empty())
            ImGui::Text("Build: %s", m_envelope.buildInfo.c_str());
        ImGui::Separator();

        // ---- activeLayers: one opaque summary line -------------------------
        // Rendered as a joined line, never a per-value switch: the
        // vocabulary (breadcrumbs:*, dred:*, dred-data:*, devicefault:*,
        // devicefault-data:*, ...) is documented to grow, and an exhaustive
        // switch would silently stop covering the day a new layer key ships.
        if (!m_envelope.activeLayers.empty())
        {
            std::string line;
            for (const std::string& layer : m_envelope.activeLayers)
            {
                if (!line.empty())
                    line += "   ";
                line += layer;
            }
            ImGui::TextWrapped("Layers: %s", line.c_str());
            ImGui::Separator();
        }

        // ---- per-queue timeline: lastCompleted then inFlight ----------------
        const std::vector<const Arcane::Diag::Envelope::Queue*> queues = VisibleQueues();
        if (ImGui::CollapsingHeader("GPU Queues", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (queues.empty())
                ImGui::TextDisabled("(no GPU queue activity captured)");
            for (const Arcane::Diag::Envelope::Queue* q : queues)
            {
                ImGui::TextUnformatted(q->name.empty() ? "(unnamed queue)" : q->name.c_str());
                if (!q->lastCompleted.empty())
                    ImGui::BulletText("last completed: %s", q->lastCompleted.c_str());
                for (const std::string& scope : q->inFlight)
                {
                    // In-flight = the pass the GPU entered and never left --
                    // highlighted with the theme's one non-gray accent
                    // (Theme::kAmber). EditorTheme.hpp's own doc comment
                    // names kAmber the editor's "this is the thing you are
                    // acting on" language (the drop-target frame, the
                    // selected shader-graph node border) -- there is no
                    // separate "kWarning" token in the monochrome theme, so
                    // this is its existing accessor for "pay attention
                    // here", never a hard-coded literal.
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::kAmber);
                    ImGui::BulletText("in flight: %s", scope.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }

        // ---- fault block: hidden for CPU-report noise ------------------------
        if (HasVisibleFault())
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Fault", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Type: %s", m_envelope.fault.type.c_str());
                if (!m_envelope.fault.address.empty())
                    ImGui::Text("Address: %s", m_envelope.fault.address.c_str());
                if (!m_envelope.fault.resource.empty())
                    ImGui::Text("Resource: %s", m_envelope.fault.resource.c_str());
            }
        }

        // ---- CPU thread summary --------------------------------------------
        if (!m_envelope.cpuThreadSummary.empty())
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("CPU Threads", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(m_envelope.cpuThreadSummary.c_str());
                ImGui::PopTextWrapPos();
            }
        }

        // ---- sibling files: shell-open buttons, truthful presence -------------
        // A button appears ONLY when its field is non-empty: WriteReportImpl
        // records a sibling path only when it actually wrote that file
        // (Diagnostics.cpp:581-582 for txt/dmp; GpuCrashReport.hpp's
        // EmitGpuDumpSibling doc comment for the gpudump), so an empty field
        // here means "not produced this run", never a stale/guessed path.
        ImGui::Separator();
        bool anySibling = false;
        if (!m_envelope.siblingTxt.empty())
        {
            if (ImGui::Button("Show .txt"))
                ShowInExplorer(m_envelope.siblingTxt);
            anySibling = true;
        }
        if (!m_envelope.siblingDmp.empty())
        {
            if (anySibling)
                ImGui::SameLine();
            if (ImGui::Button("Show .dmp"))
                ShowInExplorer(m_envelope.siblingDmp);
            anySibling = true;
        }
        if (!m_envelope.siblingGpuDump.empty())
        {
            if (anySibling)
                ImGui::SameLine();
            if (ImGui::Button("Show .gpudump"))
                ShowInExplorer(m_envelope.siblingGpuDump);
            anySibling = true;

            // The container's section inventory, rendered inline -- parsed
            // ONCE at construction (GpuDumpSectionTags), never re-read here.
            if (!m_gpuDumpTags.empty())
            {
                std::string inventory;
                for (const std::string& tag : m_gpuDumpTags)
                {
                    if (!inventory.empty())
                        inventory += ", ";
                    inventory += tag;
                }
                ImGui::TextDisabled("Sections: %s", inventory.c_str());
            }
        }
        if (!anySibling)
            ImGui::TextDisabled("(no sibling files recorded)");

        ImGui::End();
        requestClose = !open;
    }
}
