#include <Arcane/Host/ExclusionList.hpp>

#include <Json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace Arcane
{
    namespace
    {
        std::string Lower(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // "d3d12" and "dx12" name the same backend; the CLI says dx12, the
        // report says D3D12. Normalise so neither spelling is a silent miss.
        std::string NormaliseBackend(std::string_view s)
        {
            std::string v = Lower(s);
            if (v == "d3d12") return "dx12";
            return v;
        }

        bool IsIsoDate(std::string_view s)
        {
            if (s.size() != 10) return false;
            if (s[4] != '-' || s[7] != '-') return false;
            for (const std::size_t i : { 0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u })
                if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            // Bounded range checks only -- month 1-12, day 1-31. No calendar or
            // leap-year awareness (e.g. "2026-02-30" is intentionally accepted):
            // the goal is refusing a value that cannot sort meaningfully as a
            // date at all (IsExpired's lexicographic compare would otherwise
            // place an out-of-range month, e.g. a transposed "13", silently
            // between real 2026 and 2027 dates), not full calendar validation.
            const int month = (s[5] - '0') * 10 + (s[6] - '0');
            const int day   = (s[8] - '0') * 10 + (s[9] - '0');
            if (month < 1 || month > 12) return false;
            if (day < 1 || day > 31) return false;
            return true;
        }

        // Case-insensitivity is deliberately applied to ALL THREE axes, not just
        // backend (the only one the brief called out by name) -- host and
        // configuration spellings are equally free-text in the config, and a
        // "debug" vs "Debug" mismatch would be the same silent-miss failure
        // class the case-insensitive backend matching exists to prevent.
        bool AxisMatches(const std::vector<std::string>& allowed, std::string_view value, bool backend)
        {
            if (allowed.empty()) return true;   // omitted means ALL
            const std::string v = backend ? NormaliseBackend(value) : Lower(value);
            for (const std::string& a : allowed)
                if ((backend ? NormaliseBackend(a) : Lower(a)) == v) return true;
            return false;
        }

        bool ReadStringArray(const nlohmann::json& obj, const char* key,
                             std::vector<std::string>& out, std::string& error)
        {
            if (!obj.contains(key)) return true;
            if (!obj.at(key).is_array())
            {
                error = std::string("\"") + key + "\" must be an array of strings";
                return false;
            }
            for (const auto& v : obj.at(key))
            {
                if (!v.is_string())
                {
                    error = std::string("\"") + key + "\" must contain only strings";
                    return false;
                }
                out.push_back(v.get<std::string>());
            }
            return true;
        }
    }

    std::optional<std::vector<ExclusionEntry>>
        ParseExclusions(std::string_view json, std::string& error)
    {
        error.clear();
        nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
        if (doc.is_discarded())
        {
            error = "not valid JSON";
            return std::nullopt;
        }
        if (!doc.is_array())
        {
            error = "the exclusion file must be a JSON array of entries";
            return std::nullopt;
        }

        std::vector<ExclusionEntry> out;
        std::size_t index = 0;
        for (const auto& item : doc)
        {
            const std::string where = "entry " + std::to_string(index++) + ": ";
            if (!item.is_object())
            {
                error = where + "must be an object";
                return std::nullopt;
            }
            ExclusionEntry e;
            for (const char* key : { "target", "reason", "expires" })
            {
                if (!item.contains(key) || !item.at(key).is_string() ||
                    item.at(key).get<std::string>().empty())
                {
                    error = where + "\"" + key + "\" is required and must be a non-empty string";
                    return std::nullopt;
                }
            }
            e.target  = item.at("target").get<std::string>();
            e.reason  = item.at("reason").get<std::string>();
            e.expires = item.at("expires").get<std::string>();
            if (!IsIsoDate(e.expires))
            {
                error = where + "\"expires\" must be an ISO date, YYYY-MM-DD";
                return std::nullopt;
            }
            std::string axisError;
            if (!ReadStringArray(item, "backends", e.backends, axisError) ||
                !ReadStringArray(item, "hosts", e.hosts, axisError) ||
                !ReadStringArray(item, "configurations", e.configurations, axisError))
            {
                error = where + axisError;
                return std::nullopt;
            }
            out.push_back(std::move(e));
        }
        return out;
    }

    const ExclusionEntry* MatchExclusion(const std::vector<ExclusionEntry>& entries,
                                         const ExclusionQuery& q)
    {
        for (const ExclusionEntry& e : entries)
        {
            if (e.target != q.target) continue;
            if (!AxisMatches(e.backends,       q.backend,       /*backend*/ true))  continue;
            if (!AxisMatches(e.hosts,          q.host,          /*backend*/ false)) continue;
            if (!AxisMatches(e.configurations, q.configuration, /*backend*/ false)) continue;
            return &e;
        }
        return nullptr;
    }

    bool IsExpired(const ExclusionEntry& entry, std::string_view today)
    {
        // ISO YYYY-MM-DD sorts lexicographically. Strictly-after, so an entry
        // expiring today is live for the whole of today.
        return today > entry.expires;
    }

    std::string TodayIso()
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
    #if defined(_WIN32)
        localtime_s(&tm, &now);
    #else
        localtime_r(&now, &tm);
    #endif
        char buf[11] = {};
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return std::string(buf);
    }
}
