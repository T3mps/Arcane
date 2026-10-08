#include <Panels/ConsoleModel.hpp>

#include <Settings/EditorConsoleSettings.hpp>   // editor.console.categoryWidth (settings S6-41)
#include <Widgets/IconsLucide.h>

#include <Arcane/Config/Settings.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <unordered_map>

namespace Arcane::Editor
{
    namespace
    {
        struct PrefixRule { std::string_view prefix; std::string_view category; };

        // Longest-useful-prefix first is not required (no rule is a prefix of
        // another), but keep new rules grouped by category for readability.
        constexpr PrefixRule kPrefixRules[] = {
            { "AssetRegistry: ",     "Assets"   },
            { "Assets: ",            "Assets"   },
            { "plugin: ",            "Plugin"   },
            { "scene load: ",        "Scene"    },
            { "Save Scene: ",        "Scene"    },
            { "SpriteMaterialCache: ", "Material" },
            { "PostChainCache: ",    "Material" },
            { "LoadMaterialAsset: ", "Material" },
            { "Open Project: ",      "Project"  },
            { "bootScene: ",         "Project"  },
            { "RuntimeLaunch: ",     "Project"  },
            { "Build: ",             "Build"    },
            { "Arcane Editor: ",     "Editor"      },
            { "input: ",             "Input"       },
            { "IdeLaunch: ",         "IDE"         },
            { "IDE: ",               "IDE"         },
            { "Diagnostics: ",       "Diagnostics" },
            { "AudioDevice: ",       "Audio"       },
        };
    }

    std::string_view CategoryForMessage(std::string_view message) noexcept
    {
        // A leading "[tag]" (1-24 of [A-Za-z0-9_-], closed by ']') is the
        // category, verbatim (node-page phase s8.2): "[nri-graph]", "[thumbs]".
        // The message itself is untouched, so copy fidelity holds.
        if (message.size() >= 3 && message.front() == '[')
        {
            std::size_t i = 1;
            while (i < message.size() && i <= 25 &&
                   (std::isalnum(static_cast<unsigned char>(message[i])) || message[i] == '-' || message[i] == '_'))
                ++i;
            const std::size_t len = i - 1;
            if (len >= 1 && len <= 24 && i < message.size() && message[i] == ']')
                return message.substr(1, len);
        }
        for (const PrefixRule& rule : kPrefixRules)
            if (message.starts_with(rule.prefix))
                return rule.category;
        return "General";
    }

    std::vector<CollapsedRow> CollapseConsole(std::span<const ConsoleEntry> entries)
    {
        std::vector<CollapsedRow> rows;
        rows.reserve(entries.size());
        // key -> index into rows. Folds NON-adjacent duplicates too (Unity's
        // behavior): a warning that recurs after unrelated lines still folds.
        std::unordered_map<std::string, std::size_t> seen;

        for (const ConsoleEntry& e : entries)
        {
            std::string key;
            key.reserve(e.category.size() + e.message.size() + 4);
            key += static_cast<char>('0' + static_cast<int>(e.level));
            key += '\x1f';
            key += e.category;
            key += '\x1f';
            key += e.message;

            const auto it = seen.find(key);
            if (it == seen.end())
            {
                seen.emplace(std::move(key), rows.size());
                rows.push_back(CollapsedRow{ &e, 1 });
            }
            else
            {
                ++rows[it->second].count;
            }
        }
        return rows;
    }

    std::string ClockText(std::uint64_t timestampMs)
    {
        const std::time_t secs = static_cast<std::time_t>(timestampMs / 1000);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &secs);
#else
        localtime_r(&secs, &tm);
#endif
        char buf[16] = {};
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    }

    std::string FormatConsoleRow(const ConsoleEntry& e, std::size_t count)
    {
        std::string out = ClockText(e.timestampMs);
        out += "  ";
        out += e.category;
        // Pad to the panel's own category minimum (editor.console.categoryWidth,
        // 8 by default: "%-8s") so a multi-row paste stays column-aligned in a
        // monospace target.
        const std::size_t width = static_cast<std::size_t>(std::max(0, Settings<EditorConsoleSettings>().categoryWidth));
        for (std::size_t i = e.category.size(); i < width; ++i)
            out += ' ';
        out += "  ";
        out += e.message;
        if (count > 1)
        {
            out += "  (x";
            out += std::to_string(count);
            out += ')';
        }
        return out;
    }

    namespace
    {
        std::string BadgedTitle(std::string_view name, std::size_t n)
        {
            std::string t(name);
            if (n > 0) t += "  " + std::to_string(n);
            return t + "###" + std::string(name);
        }
    }

    std::string ProblemsTabTitle(std::size_t nErr, std::size_t nWarn) { return BadgedTitle("Problems", nErr + nWarn); }
    std::string ConsoleTabTitle(std::size_t unseen) { return BadgedTitle("Console", unseen); }

    std::size_t UnseenAlerts(std::span<const ConsoleEntry> entries, std::uint64_t lastSeenSeq) noexcept
    {
        std::size_t n = 0;
        for (const ConsoleEntry& e : entries)
            if (e.seq > lastSeenSeq && e.level != Arcane::DiagSeverity::Info) ++n;
        return n;
    }

    std::size_t UnseenErrors(std::span<const ConsoleEntry> entries, std::uint64_t lastSeenSeq) noexcept
    {
        std::size_t n = 0;
        for (const ConsoleEntry& e : entries)
            if (e.seq > lastSeenSeq && e.level == Arcane::DiagSeverity::Error) ++n;
        return n;
    }

    std::optional<ProblemsChipText> ProblemsChip(std::size_t nErr, std::size_t nWarn)
    {
        if (nErr + nWarn == 0) return std::nullopt;
        ProblemsChipText c;
        c.label = std::string(nErr > 0 ? ICON_LC_CIRCLE_X : ICON_LC_TRIANGLE_ALERT) + " " + std::to_string(nErr + nWarn);
        c.tooltip = std::to_string(nErr) + " errors, " + std::to_string(nWarn) + " warnings";
        return c;
    }
}
