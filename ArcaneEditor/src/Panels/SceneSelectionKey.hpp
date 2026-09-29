#pragma once

// The scene's Inspector selection key (inspector-ownership T3): the WHOLE
// ordered selection set, primary first -- "<primary>;<e1>,<e2>,...,<eN>" --
// so a pin or a history entry names what the user saw (the body fans out
// over every member) and a member's death does not kill the page while
// others live (UE's locked Details view purges dead members and empties
// only when none remain). Pure and header-only so ArcaneTests can drive it
// without SceneInspectorSource.cpp (which needs EditorPanels.cpp).

#include "Scene/SelectionContext.hpp"
#include <Astra/Entity/Entity.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor::SceneSelectionKey
{
    struct KeySet
    {
        Astra::Entity primary = Astra::Entity::Invalid();
        std::vector<Astra::Entity> members;
    };

    [[nodiscard]] inline std::string Encode(const SelectionContext& sel)
    {
        if (!sel.HasSelection()) return {};
        std::string out = std::to_string(static_cast<std::uint64_t>(sel.Primary().GetValue())) + ";";
        bool first = true;
        for (const Astra::Entity e : sel.Entities())
        {
            if (!first) out += ',';
            first = false;
            out += std::to_string(static_cast<std::uint64_t>(e.GetValue()));
        }
        return out;
    }

    [[nodiscard]] inline Astra::Entity ParseOne(std::string_view tok)
    {
        std::uint64_t value = 0;
        const auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), value);
        if (ec != std::errc{} || p != tok.data() + tok.size()) return Astra::Entity::Invalid();
        return Astra::Entity(static_cast<Astra::Entity::StorageType>(value));
    }

    // Empty members on any malformed segment, or a primary absent from the list.
    [[nodiscard]] inline KeySet Decode(std::string_view key)
    {
        KeySet set;
        const std::size_t semi = key.find(';');
        if (semi == std::string_view::npos) return {};
        const Astra::Entity primary = ParseOne(key.substr(0, semi));
        if (!primary.IsValid()) return {};
        std::string_view rest = key.substr(semi + 1);
        while (!rest.empty())
        {
            const std::size_t comma = rest.find(',');
            const Astra::Entity e = ParseOne(rest.substr(0, comma));
            if (!e.IsValid()) return {};
            set.members.push_back(e);
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        if (std::find(set.members.begin(), set.members.end(), primary) == set.members.end()) return {};
        set.primary = primary;
        return set;
    }

    // Members filtered by `alive`; primary = the keyed primary when alive, else
    // the LAST alive member (SelectionContext::Prune's fallback), Invalid when none.
    template <typename IsAliveFn>
    [[nodiscard]] KeySet AliveSubset(const KeySet& set, IsAliveFn&& alive)
    {
        KeySet out;
        for (const Astra::Entity e : set.members)
            if (alive(e)) out.members.push_back(e);
        if (out.members.empty()) return out;
        out.primary = alive(set.primary) ? set.primary : out.members.back();
        return out;
    }
}
