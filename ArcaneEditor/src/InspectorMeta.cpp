#include "InspectorMeta.hpp"

#include <Astra/Reflection/Attribute.hpp>

#include <cctype>

namespace Arcane::Editor
{
    namespace
    {
        bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
        bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
        bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        // A word boundary falls BEFORE index i.
        bool BreaksBefore(std::string_view s, std::size_t i)
        {
            if (i == 0 || i >= s.size()) return false;
            if (!IsUpper(s[i])) return false;

            const char prev = s[i - 1];
            // lower|digit -> Upper: the ordinary camelCase boundary.
            if (IsLower(prev) || IsDigit(prev)) return true;
            // Upper -> Upper followed by lower: the END of an acronym run, so
            // "HTTPServer" splits once (HTTP | Server) rather than per letter.
            if (IsUpper(prev) && i + 1 < s.size() && IsLower(s[i + 1])) return true;
            return false;
        }

        // Case-insensitive substring. ASCII-only folding is sufficient: these
        // are C++ identifiers and author-written attribute strings.
        bool ContainsFold(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty()) return true;
            if (needle.size() > haystack.size()) return false;
            const auto lower = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            };
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
            {
                std::size_t j = 0;
                for (; j < needle.size(); ++j)
                    if (lower(haystack[i + j]) != lower(needle[j])) break;
                if (j == needle.size()) return true;
            }
            return false;
        }
    }

    std::string DeriveDisplayName(std::string_view identifier)
    {
        std::string spaced;
        spaced.reserve(identifier.size() + 8);
        for (std::size_t i = 0; i < identifier.size(); ++i)
        {
            const char c = identifier[i];
            if (c == '_') { spaced.push_back(' '); continue; }
            if (BreaksBefore(identifier, i)) spaced.push_back(' ');
            spaced.push_back(c);
        }

        // Split on spaces, capitalise each word, rejoin with single spaces --
        // which collapses runs and trims both ends in one pass.
        std::string out;
        out.reserve(spaced.size());
        bool wordStart = true;
        for (char c : spaced)
        {
            if (c == ' ') { wordStart = true; continue; }
            if (wordStart)
            {
                if (!out.empty()) out.push_back(' ');
                out.push_back(IsLower(c) ? static_cast<char>(c - 'a' + 'A') : c);
                wordStart = false;
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string DisplayNameForField(const Astra::FieldInfo& field)
    {
        if (const Astra::DisplayName* d = field.GetAttribute<Astra::DisplayName>())
            return std::string(d->name);
        return DeriveDisplayName(field.name);
    }

    std::string DisplayNameForComponent(std::string_view typeName)
    {
        const std::size_t sep = typeName.rfind("::");
        const std::string_view leaf =
            (sep == std::string_view::npos) ? typeName : typeName.substr(sep + 2);
        return DeriveDisplayName(leaf);
    }

    std::string_view CategoryOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Category* c = field.GetAttribute<Astra::Category>())
            return c->category;
        return {};
    }

    std::string_view TooltipOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Tooltip* t = field.GetAttribute<Astra::Tooltip>())
            return t->text;
        return {};
    }

    std::optional<Astra::Range> RangeOfField(const Astra::FieldInfo& field)
    {
        if (const Astra::Range* r = field.GetAttribute<Astra::Range>())
            return *r;
        return std::nullopt;
    }

    bool FieldIsReadOnly(const Astra::FieldInfo& field)
    {
        return field.GetAttribute<Astra::ReadOnly>() != nullptr;
    }

    bool FieldIsAttributeHidden(const Astra::FieldInfo& field)
    {
        return field.GetAttribute<Astra::Hidden>() != nullptr;
    }

    bool ComponentMatchesFilter(std::string_view componentDisplayName,
                                std::string_view query)
    {
        return ContainsFold(componentDisplayName, query);
    }

    bool MatchesInspectorFilter(std::string_view componentDisplayName,
                                std::string_view fieldDisplayName,
                                std::string_view rawFieldName,
                                std::string_view query)
    {
        if (query.empty()) return true;
        if (ContainsFold(componentDisplayName, query)) return true;
        return ContainsFold(fieldDisplayName, query) || ContainsFold(rawFieldName, query);
    }
}
