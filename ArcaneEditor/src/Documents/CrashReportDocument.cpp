#include "Documents/CrashReportDocument.hpp"

#include "Widgets/EditorTheme.hpp"

#include <Arcane/Render/IGpuCrashBackend.hpp>   // Diag::ReadGpuDump / ParseGpuDump

#include <imgui.h>

#include <system_error>

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

        // Resolved ONCE here, never in Draw() (post-review fix): a moved or
        // copied reports folder leaves the envelope's ABSOLUTE paths stale,
        // so every sibling is resolved against disk exactly once, at load,
        // and the verdict (existing path or empty = missing) is what both
        // Draw() and any headless test observe. See the class header
        // comment and ResolveSibling's own doc comment.
        m_siblingTxtResolved     = ResolveSibling(m_envelope.siblingTxt, m_path);
        m_siblingDmpResolved     = ResolveSibling(m_envelope.siblingDmp, m_path);
        m_siblingGpuDumpResolved = ResolveSibling(m_envelope.siblingGpuDump, m_path);

        // Parsed ONCE here, never in Draw(): the model half a headless test
        // observes (GpuDumpSectionTags) is exactly what gets rendered. Reads
        // the RESOLVED path (which may be the beside-.arcdiag fallback, not
        // the envelope's raw/possibly-stale one) -- a moved report folder
        // still yields its real section inventory.
        if (!m_siblingGpuDumpResolved.empty())
        {
            if (const auto dump = Arcane::Diag::ReadGpuDump(m_siblingGpuDumpResolved))
                for (const auto& section : dump->sections)
                    m_gpuDumpTags.push_back(section.tag);
        }
    }

    std::filesystem::path CrashReportDocument::ResolveSibling(const std::string& recorded,
                                                               const std::filesystem::path& docPath)
    {
        if (recorded.empty())
            return {};   // never produced this run -- not "missing", just absent

        const std::filesystem::path stored(recorded);
        std::error_code ec;
        if (std::filesystem::exists(stored, ec) && !ec)
            return stored;

        // Siblings are minted beside each other at the same stem
        // (Diagnostics.cpp:335-339 mints base = "<appName>-<stamp>-pid<N>"
        // and every sibling as `dir / (base + ext)`), so a whole reports
        // folder moved or copied together still has this file sitting right
        // beside the .arcdiag this document was opened from.
        const std::filesystem::path fallback = docPath.parent_path() / stored.filename();
        ec.clear();
        if (std::filesystem::exists(fallback, ec) && !ec)
            return fallback;

        return {};   // recorded, but neither location panned out
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

        // ---- sibling files: shell-open buttons, truthful presence + resolved location --
        // A button appears ONLY when its envelope field is non-empty:
        // WriteReportImpl records a sibling path only when it actually
        // wrote that file (Diagnostics.cpp:581-582 for txt/dmp;
        // GpuCrashReport.hpp's EmitGpuDumpSibling doc comment for the
        // gpudump), so an empty field means "not produced this run". The
        // path a visible button OPENS is the one resolved at construction
        // (ResolveSibling) -- when neither the stored path nor the
        // beside-.arcdiag fallback exists, the button renders DISABLED with
        // a "(missing)" hint instead of silently shelling a dead path.
        ImGui::Separator();
        bool anySibling = false;
        const auto siblingButton = [&](const char* label, const std::string& recorded,
                                       const std::filesystem::path& resolved)
        {
            if (recorded.empty())
                return;
            if (anySibling)
                ImGui::SameLine();
            anySibling = true;
            if (resolved.empty())
            {
                ImGui::BeginDisabled();
                ImGui::Button(label);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(missing)");
            }
            else if (ImGui::Button(label))
            {
                ShowInExplorer(resolved.string());
            }
        };
        siblingButton("Show .txt", m_envelope.siblingTxt, m_siblingTxtResolved);
        siblingButton("Show .dmp", m_envelope.siblingDmp, m_siblingDmpResolved);
        siblingButton("Show .gpudump", m_envelope.siblingGpuDump, m_siblingGpuDumpResolved);

        // The container's section inventory, rendered inline -- parsed ONCE
        // at construction from the RESOLVED path (GpuDumpSectionTags),
        // never re-read here. Empty whenever the gpudump sibling wasn't
        // recorded, didn't resolve, or didn't parse.
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

        if (!anySibling)
            ImGui::TextDisabled("(no sibling files recorded)");

        ImGui::End();
        requestClose = !open;
    }
}
