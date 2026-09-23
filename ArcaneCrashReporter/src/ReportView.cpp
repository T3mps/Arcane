#include "ReportView.hpp"
#include <Arcane/Base/ForeignModules.hpp>
#include <cstdio>
#include <filesystem>

namespace Arcane::Reporter
{
    std::string PlainWordsKind(std::string_view kind)
    {
        if (kind == "crash")         return "crashed";
        if (kind == "hang")          return "stopped responding";
        if (kind == "gpu-stall")     return "the GPU stopped responding";
        if (kind == "gpu-crash")     return "the GPU device was lost";
        if (kind == "assert")        return "an assertion failed";
        if (kind == "terminate")     return "terminated";
        if (kind == "out-of-memory") return "ran out of memory";
        if (kind == "ensure")        return "hit a recoverable check";
        if (kind == "abnormal-exit") return "exited abnormally";
        return "stopped (" + std::string(kind) + ")";
    }

    std::string LastLines(std::string_view text, std::size_t n)
    {
        if (text.empty() || n == 0) return {};
        std::size_t end = text.size();
        if (text.back() == '\n') --end;             // a trailing newline does not count as a line
        std::size_t pos = end;
        for (std::size_t lines = 0; pos > 0; --pos)
        {
            if (text[pos - 1] == '\n' && ++lines == n) break;
        }
        return std::string(text.substr(pos));
    }

    namespace
    {
        std::string InjectedLines(const std::vector<std::string>& names)
        {
            std::string out;
            for (const std::string& name : names)
            {
                out += name;
                if (const auto m = ForeignModules::Classify(name))
                {
                    out += " -- " + m->product + " (tier " + std::to_string(m->tier) + ")";
                    if (!m->consequence.empty()) out += ": " + m->consequence;
                    if (!m->remedy.empty())      out += " -- " + m->remedy;
                }
                else
                {
                    out += " -- injected, uncatalogued";
                }
                out += "\n";
            }
            return out;
        }

        std::string GpuLines(const Diag::Envelope& e)
        {
            std::string out;
            for (const auto& q : e.queues)
            {
                out += q.name + ": last completed " + (q.lastCompleted.empty() ? "<none>" : q.lastCompleted);
                if (!q.inFlight.empty())
                {
                    out += ", in flight ";
                    for (std::size_t i = 0; i < q.inFlight.size(); ++i) out += (i ? ", " : "") + q.inFlight[i];
                }
                out += "\n";
            }
            if (!e.fault.type.empty())
                out += "fault: " + e.fault.type + " at " + e.fault.address + (e.fault.resource.empty() ? "" : " (" + e.fault.resource + ")") + "\n";
            if (!e.activeLayers.empty())
            {
                out += "layers: ";
                for (std::size_t i = 0; i < e.activeLayers.size(); ++i) out += (i ? ", " : "") + e.activeLayers[i];
                out += "\n";
            }
            return out;
        }

        // The portable stack's first line is "--- thread <id> (MAIN)"; the label is the rest of that line.
        ThreadView PortableThread(std::string_view summary)
        {
            ThreadView t;
            const std::size_t nl = summary.find('\n');
            const std::string_view first = summary.substr(0, nl);
            t.label = first.rfind("--- ", 0) == 0 ? std::string(first.substr(4)) : "thread";
            t.text  = nl == std::string_view::npos ? "" : std::string(summary.substr(nl + 1));
            return t;
        }
    }

    ReportView BuildReportView(const Diag::Envelope& e, const Args& a, const Symbolized* sym, std::string_view logTail)
    {
        ReportView v;
        const std::string product = a.product.empty() ? (e.appName.empty() ? "Arcane" : e.appName) : a.product;
        const std::string words = PlainWordsKind(e.kind);
        v.title     = product + " -- " + words;
        v.headline  = product + " " + words;
        v.whenLine  = e.timestampUtc + (e.phase.empty() ? "" : " | phase: " + e.phase) + (e.buildInfo.empty() ? "" : " | build: " + e.buildInfo);
        v.reasonText = e.reason.empty() ? "(no reason recorded)" : e.reason;
        v.injectedText = InjectedLines(e.foreignModules);
        v.gpuText   = GpuLines(e);
        v.logTail   = std::string(logTail);
        v.reportFolder = std::filesystem::path(a.envelopePath).parent_path().generic_string();
        v.relaunchLine = a.relaunch.empty() ? e.commandLine : a.relaunch;
        v.isHang    = (e.kind == "hang" || e.kind == "gpu-stall");
        v.isAbnormalExit = (e.kind == "abnormal-exit");
        v.canRelaunch = !v.relaunchLine.empty() && !v.isHang;

        if (sym && sym->engineAvailable && !sym->threads.empty())
        {
            for (const SymThread& t : sym->threads)
            {
                ThreadView tv;
                tv.label = "thread " + std::to_string(t.systemId) + (t.faulting ? " (faulting)" : "");
                char idx[8];
                for (std::size_t i = 0; i < t.frames.size(); ++i)
                {
                    std::snprintf(idx, sizeof(idx), "%02zu ", i);
                    tv.text += idx + FormatFrame(t.frames[i]) + "\n";
                }
                v.threads.push_back(std::move(tv));
            }
        }
        else if (!e.cpuThreadSummary.empty())
        {
            v.threads.push_back(PortableThread(e.cpuThreadSummary));
        }
        return v;
    }

    std::string DetailsText(const ReportView& v, std::size_t threadIndex)
    {
        std::string d = v.headline + "\n" + v.whenLine + "\n\nreason: " + v.reasonText + "\n";
        if (!v.injectedText.empty()) d += "\ninjected modules:\n" + v.injectedText;
        if (threadIndex < v.threads.size()) d += "\n--- " + v.threads[threadIndex].label + "\n" + v.threads[threadIndex].text;
        if (!v.gpuText.empty()) d += "\n=== GPU ===\n" + v.gpuText;
        if (!v.logTail.empty()) d += "\n=== log (tail) ===\n" + v.logTail;
        d += "\nreport folder: " + v.reportFolder + "\n";
        return d;
    }
}
