#include "Panels/InspectorKinds.hpp"

#include <algorithm>

namespace Arcane::Editor
{
    const InspectorKind* FindInspectorKind(std::string_view id)
    {
        for (const InspectorKind& k : kInspectorKinds)
            if (k.id == id) return &k;
        return nullptr;
    }

    bool InspectorFilter::Admits(std::string_view kind) const
    {
        if (IsAll()) return true;
        if (kind.empty()) return false;
        return std::find(excluded.begin(), excluded.end(), kind) == excluded.end();
    }

    bool InspectorFilter::ExcludesEveryKind() const
    {
        return std::all_of(kInspectorKinds.begin(), kInspectorKinds.end(), [&](const InspectorKind& k)
        { return std::find(excluded.begin(), excluded.end(), k.id) != excluded.end(); });
    }

    InspectorFilter InspectorFilter::Sanitized() const
    {
        InspectorFilter out;
        for (const InspectorKind& k : kInspectorKinds)
            if (std::find(excluded.begin(), excluded.end(), k.id) != excluded.end())
                out.excluded.emplace_back(k.id);
        if (out.ExcludesEveryKind()) out.excluded.clear();
        return out;
    }

    InspectorFilter InspectorFilter::AllBut(std::string_view kind)
    {
        InspectorFilter f;
        f.excluded.emplace_back(kind);
        return f.Sanitized();
    }

    InspectorFilter InspectorFilter::Only(std::string_view kind)
    {
        InspectorFilter f;
        for (const InspectorKind& k : kInspectorKinds)
            if (k.id != kind) f.excluded.emplace_back(k.id);
        return f.Sanitized();
    }

    std::string InspectorFilterLabel(const InspectorFilter& filter)
    {
        if (filter.IsAll()) return "All";
        std::vector<std::string_view> ticked, unticked;
        for (const InspectorKind& k : kInspectorKinds)
            (filter.Admits(k.id) ? ticked : unticked).push_back(k.displayName);
        if (ticked.size() == 1) return std::string(ticked[0]);
        if (unticked.size() == 1) return "All but " + std::string(unticked[0]);
        std::string out;
        for (const std::string_view n : ticked) { if (!out.empty()) out += ", "; out += n; }
        return out;
    }

    FilterFace InspectorFilterFace(const InspectorFilter& filter, int maxIcons)
    {
        FilterFace face;
        if (filter.IsAll()) { face.icons.push_back(kInspectorAllIcon); return face; }
        const std::size_t cap = static_cast<std::size_t>(std::max(maxIcons, 1));
        for (const InspectorKind& k : kInspectorKinds)
        {
            if (!filter.Admits(k.id)) continue;
            if (face.icons.size() < cap) face.icons.push_back(k.icon);
            else ++face.overflow;
        }
        return face;
    }
}
