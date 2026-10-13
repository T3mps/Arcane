#pragma once

// The Input Actions document's Inspector selection key (spec A s3.1, spec B
// s2.4): "<map>/<action>/<binding>/<part>", THREE slashes ALWAYS, filled
// top-down with no gaps. One grammar for every producer (the model's
// SelectionKey, the page's crumbs) and every consumer (Resolves,
// RestoreSelection, RestoreSelectionOrAncestor, the document's PageFor). The
// asset root "" is each caller's own case. SceneSelectionKey.hpp is the
// scene's counterpart.

#include <Arcane/Guid.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // Every '/'-separated segment, empty ones kept: "" -> {""}, "a/" -> {"a",""}.
    [[nodiscard]] inline std::vector<std::string_view> SplitKey(std::string_view key)
    {
        std::vector<std::string_view> segs;
        for (std::size_t start = 0;;)
        {
            const std::size_t slash = key.find('/', start);
            segs.push_back(key.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
            if (slash == std::string_view::npos) return segs;
            start = slash + 1;
        }
    }

    // Syntax only, no existence check. nullopt unless: EXACTLY 4 segments;
    // segment 0 a valid Guid; every other segment empty or a valid Guid; no
    // non-empty segment after an empty one.
    [[nodiscard]] inline std::optional<std::array<Guid, 4>> ParseSelectionKey(std::string_view key)
    {
        const auto segs = SplitKey(key);
        if (segs.size() != 4) return std::nullopt;
        std::array<Guid, 4> ids{};
        bool ended = false;
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (segs[i].empty()) { if (i == 0) return std::nullopt; ended = true; continue; }
            if (ended) return std::nullopt;   // a gap
            const auto id = Guid::FromString(segs[i]);
            if (!id || !id->IsValid()) return std::nullopt;
            ids[i] = *id;
        }
        return ids;
    }

    // The inverse: "" when ids[0] is invalid, else four segments joined by '/'.
    [[nodiscard]] inline std::string EncodeSelectionKey(const std::array<Guid, 4>& ids)
    {
        if (!ids[0].IsValid()) return {};
        auto seg = [](const Guid& g) { return g.IsValid() ? g.ToString() : std::string{}; };
        return seg(ids[0]) + "/" + seg(ids[1]) + "/" + seg(ids[2]) + "/" + seg(ids[3]);
    }
}
