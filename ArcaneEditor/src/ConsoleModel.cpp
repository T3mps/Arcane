#include <ConsoleModel.hpp>

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
        };
    }

    std::string_view CategoryForMessage(std::string_view message) noexcept
    {
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
        // Pad to the panel's own 8-column category minimum ("%-8s") so a
        // multi-row paste stays column-aligned in a monospace target.
        for (std::size_t i = e.category.size(); i < 8; ++i)
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
}
