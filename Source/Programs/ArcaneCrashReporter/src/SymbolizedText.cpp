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

    namespace
    {
        // "NN <frame>": two or more digits, then exactly one space (%02zu).
        [[nodiscard]] std::size_t FrameBodyAt(std::string_view line) noexcept
        {
            std::size_t i = 0;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
            return (i >= 2 && i < line.size() && line[i] == ' ') ? i + 1 : 0;
        }

        // Parsed from the RIGHT, so "D:\..." paths and "operator!" survive:
        // trailing " [file:line]", then the last "+0x", then module!function
        // at the FIRST '!'.
        [[nodiscard]] SymFrame ParseFrame(std::string_view rest)
        {
            SymFrame f;
            if (rest.ends_with(']'))
                if (const std::size_t open = rest.rfind(" ["); open != std::string_view::npos)
                {
                    const std::string_view body = rest.substr(open + 2, rest.size() - open - 3);
                    const std::size_t colon = body.rfind(':');
                    std::uint32_t line = 0;
                    if (colon != std::string_view::npos)
                    {
                        const char* b = body.data() + colon + 1;
                        const char* e = body.data() + body.size();
                        if (const auto r = std::from_chars(b, e, line); r.ec == std::errc{} && r.ptr == e)
                        {
                            f.file = std::string(body.substr(0, colon));
                            f.line = line;
                            rest = rest.substr(0, open);
                        }
                    }
                }
            if (const std::size_t plus = rest.rfind("+0x"); plus != std::string_view::npos)
            {
                std::uint64_t d = 0;
                const char* b = rest.data() + plus + 3;
                const char* e = rest.data() + rest.size();
                if (const auto r = std::from_chars(b, e, d, 16); r.ec == std::errc{} && r.ptr == e)
                {
                    f.displacement = d;
                    rest = rest.substr(0, plus);
                }
            }
            const std::size_t bang = rest.find('!');
            f.module = std::string(rest.substr(0, bang));
            if (bang != std::string_view::npos) f.function = std::string(rest.substr(bang + 1));
            return f;
        }
    }

    std::optional<ParsedSymbolized> ParseSymbolized(std::string_view text)
    {
        constexpr std::string_view kHeader       = "symbolized by ArcaneCrashReporter ";
        constexpr std::string_view kEngineOk     = "engine      : dbgeng";
        constexpr std::string_view kEngineNo     = "engine      : unavailable (";
        constexpr std::string_view kEngineNoTail = ") -- module+offset from the portable stack";
        constexpr std::string_view kSymbolPath   = "symbol path : ";
        constexpr std::string_view kThread       = "--- thread ";
        constexpr std::string_view kThreadCap    = "--- (truncated at ";
        constexpr std::string_view kFrameCap     = "   ... (truncated at ";
        constexpr std::string_view kNoFrames     = "  <no frames recovered>";
        constexpr std::string_view kFaulting     = " (faulting)";

        std::size_t pos = 0;
        const auto nextLine = [&](std::string_view& line) -> bool
        {
            if (pos >= text.size()) return false;
            const std::size_t nl = text.find('\n', pos);
            const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
            line = text.substr(pos, end - pos);
            pos = nl == std::string_view::npos ? text.size() : nl + 1;
            return true;
        };

        std::string_view line;
        if (!nextLine(line) || !line.starts_with(kHeader)) return std::nullopt;
        ParsedSymbolized out;
        out.buildInfo = std::string(line.substr(kHeader.size()));

        while (nextLine(line))
        {
            if (line == kEngineOk) { out.sym.engineAvailable = true; continue; }
            if (line.starts_with(kSymbolPath)) { out.sym.symbolPath = std::string(line.substr(kSymbolPath.size())); continue; }
            if (line.starts_with(kEngineNo) && line.ends_with(kEngineNoTail)
                && line.size() >= kEngineNo.size() + kEngineNoTail.size())
            {
                out.sym.engineAvailable = false;
                out.sym.engineError = std::string(line.substr(kEngineNo.size(),
                                                              line.size() - kEngineNo.size() - kEngineNoTail.size()));
                const std::size_t bodyAt = pos;
                std::string_view blank;
                out.portableBody = std::string(nextLine(blank) && blank.empty() ? text.substr(pos) : text.substr(bodyAt));
                return out;
            }
            if (line.starts_with(kThread))
            {
                std::string_view rest = line.substr(kThread.size());
                SymThread t;
                if (rest.ends_with(kFaulting)) { t.faulting = true; rest.remove_suffix(kFaulting.size()); }
                if (rest != "<unknown>")
                {
                    std::uint32_t id = 0;
                    const auto r = std::from_chars(rest.data(), rest.data() + rest.size(), id);
                    t.systemId = r.ec == std::errc{} ? id : 0;
                }
                out.sym.threads.push_back(std::move(t));
                continue;
            }
            if (line.starts_with(kThreadCap)) { out.sym.threadsTruncated = true; continue; }
            if (out.sym.threads.empty() || line == kNoFrames) continue;
            if (line.starts_with(kFrameCap)) { out.sym.threads.back().framesTruncated = true; continue; }
            if (const std::size_t at = FrameBodyAt(line); at != 0)
                out.sym.threads.back().frames.push_back(ParseFrame(line.substr(at)));
        }
        return out;
    }
}
