#include "Project/SourceIncludes.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        bool IsSourceMount(std::string_view mount)
        {
            return mount.size() >= 9 && mount.substr(0, 9) == "source://";
        }

        std::string_view SourceRel(std::string_view mount)
        {
            return IsSourceMount(mount) ? mount.substr(9) : mount;
        }

        std::string_view DirOf(std::string_view rel)
        {
            const auto slash = rel.rfind('/');
            if (slash == std::string_view::npos)
                return {};
            return rel.substr(0, slash + 1);
        }

        std::string NormalizeSlashes(std::string_view p)
        {
            std::string out(p);
            std::replace(out.begin(), out.end(), '\\', '/');
            return out;
        }
    }

    std::vector<std::string> ParseIncludeDirectives(std::string_view text)
    {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < text.size())
        {
            const std::size_t lineEnd = text.find('\n', i);
            const std::size_t end = lineEnd == std::string_view::npos ? text.size() : lineEnd;
            std::string_view line = text.substr(i, end - i);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            const auto hash = line.find_first_not_of(" \t");
            if (hash != std::string_view::npos && line[hash] == '#')
            {
                std::string_view rest = line.substr(hash + 1);
                const auto tok = rest.find_first_not_of(" \t");
                if (tok != std::string_view::npos && rest.substr(tok).starts_with("include"))
                {
                    rest.remove_prefix(tok + 7);
                    const auto q = rest.find_first_not_of(" \t");
                    if (q != std::string_view::npos)
                    {
                        const char open = rest[q];
                        const char close = open == '"' ? '"' : (open == '<' ? '>' : '\0');
                        if (close != '\0')
                        {
                            const auto closeAt = rest.find(close, q + 1);
                            if (closeAt != std::string_view::npos && closeAt > q + 1)
                                out.emplace_back(NormalizeSlashes(rest.substr(q + 1, closeAt - q - 1)));
                        }
                    }
                }
            }
            if (lineEnd == std::string_view::npos)
                break;
            i = lineEnd + 1;
        }
        return out;
    }

    std::optional<Arcane::Guid> ResolveSourceInclude(
        std::string_view fromMount,
        std::string_view include,
        const std::vector<std::pair<Arcane::Guid, std::string>>& all)
    {
        if (include.empty())
            return std::nullopt;
        const std::string want = NormalizeSlashes(include);

        const std::string sibling = [&]()
        {
            std::string s(DirOf(SourceRel(fromMount)));
            s += want;
            return s;
        }();

        const Arcane::Guid* uniqueSuffix = nullptr;
        int suffixHits = 0;

        for (const auto& [guid, mount] : all)
        {
            if (!IsSourceMount(mount))
                continue;
            const std::string_view rel = SourceRel(mount);
            if (rel == sibling || rel == want)
                return guid;
            if (rel.size() >= want.size() &&
                rel.substr(rel.size() - want.size()) == want &&
                (rel.size() == want.size() || rel[rel.size() - want.size() - 1] == '/'))
            {
                uniqueSuffix = &guid;
                ++suffixHits;
            }
        }
        if (suffixHits == 1)
            return *uniqueSuffix;
        return std::nullopt;
    }
}
