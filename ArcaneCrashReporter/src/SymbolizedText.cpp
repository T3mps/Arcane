#include "SymbolizedText.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>

namespace Arcane::Reporter
{
    std::string FormatFrame(const SymFrame& f)
    {
        char off[32];
        std::snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(f.displacement));
        std::string s = f.module;
        if (!f.function.empty()) s += "!" + f.function;
        s += off;
        if (!f.file.empty()) s += " [" + f.file + ":" + std::to_string(f.line) + "]";
        return s;
    }

    std::string FormatSymbolized(const Symbolized& s, std::string_view buildInfo, std::string_view portableFallback)
    {
        std::string out = "symbolized by ArcaneCrashReporter ";
        out += buildInfo;
        out += "\n";
        if (s.engineAvailable)
        {
            out += "engine      : dbgeng\nsymbol path : " + s.symbolPath + "\n\n";
            for (const SymThread& t : s.threads)
            {
                // R77: an id of 0 is "the engine would not name it", not a
                // thread. Printing "thread 0" would read as a real id and send
                // a reader looking for a thread that does not exist.
                out += "--- thread ";
                out += t.systemId != 0 ? std::to_string(t.systemId) : std::string("<unknown>");
                out += t.faulting ? " (faulting)\n" : "\n";

                char idx[8];
                for (std::size_t i = 0; i < t.frames.size(); ++i)
                {
                    std::snprintf(idx, sizeof(idx), "%02zu ", i);
                    out += idx;
                    out += FormatFrame(t.frames[i]);
                    out += "\n";
                }
                // A blank block is indistinguishable from a formatting bug.
                if (t.frames.empty()) out += "  <no frames recovered>\n";
                // R78: a capped walk and a complete one are otherwise the same
                // text, and "is this the whole stack?" is the question.
                if (t.framesTruncated)
                    out += "   ... (truncated at the reporter's " + std::to_string(t.frames.size()) + "-frame cap)\n";
                out += "\n";
            }
            if (s.threadsTruncated)
                out += "--- (truncated at the reporter's " + std::to_string(s.threads.size()) + "-thread cap)\n\n";
        }
        else
        {
            // The portable stack the host already wrote is the body: it is
            // module+offset, but it is the TRUE stack of the walked thread and
            // a human with the matching build can still resolve it by hand.
            out += "engine      : unavailable (" + s.engineError + ") -- module+offset from the portable stack\n\n";
            out += portableFallback;
            if (out.empty() || out.back() != '\n') out += "\n";
        }
        return out;
    }

    std::uint32_t ParseWalkedThreadId(std::string_view text)
    {
        constexpr std::string_view kPrefix = "--- thread ";
        const std::size_t at = text.find(kPrefix);
        if (at == std::string_view::npos) return 0;
        const char* b = text.data() + at + kPrefix.size();
        const char* e = text.data() + text.size();
        std::uint32_t id = 0;
        const auto r = std::from_chars(b, e, id);
        return r.ec == std::errc{} ? id : 0;
    }

    void PutFaultingFirst(Symbolized& s, std::uint32_t walkedThreadId)
    {
        // The engine's own verdict wins: a dump with a stored exception event
        // names the faulting thread directly. `walkedThreadId` is only the
        // FALLBACK, for a dump that carries no exception stream at all --
        // which, since this task's synthetic record (Diagnostics.cpp), means
        // a dump older than that change or a nested report raised on the
        // crash thread itself.
        auto it = std::find_if(s.threads.begin(), s.threads.end(), [](const SymThread& t) { return t.faulting; });
        if (it == s.threads.end() && walkedThreadId != 0)
        {
            it = std::find_if(s.threads.begin(), s.threads.end(),
                              [&](const SymThread& t) { return t.systemId == walkedThreadId; });
            if (it != s.threads.end()) it->faulting = true;
        }
        if (it != s.threads.end() && it != s.threads.begin())
            std::rotate(s.threads.begin(), it, it + 1);
    }
}
