#include "ComponentCatalog.hpp"

#include <Astra/Component/Component.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/TypeMeta.hpp>
#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        // Case-insensitive substring. Local twin of AssetBrowser.hpp's
        // MatchesFilter helper and EntityList.cpp's ContainsCI; each is an
        // anonymous-namespace local in its own TU, kept that way so no panel
        // header has to export a string utility.
        bool ContainsCI(std::string_view hay, std::string_view needle)
        {
            if (needle.empty())
                return true;
            if (needle.size() > hay.size())
                return false;
            auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
            {
                std::size_t j = 0;
                while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
                    ++j;
                if (j == needle.size())
                    return true;
            }
            return false;
        }
    }

    bool IsHiddenInInspector(std::string_view typeName)
    {
        // Derived per-frame caches: showing them is noise and editing them is
        // overwritten by the next propagation pass.
        //
        // Arcane::Hidden is a MECHANISM marker, not content (user call
        // 2026-07-29): the Outliner eye is its entire interface, and it must
        // not surface anywhere else -- an empty "Hidden" section on a hidden
        // entity is implementation leakage. Unity keeps its scene-visibility
        // state in the Hierarchy for the same reason.
        return typeName == "Arcane::WorldTransform"
            || typeName == "Arcane::PreviousTransform"
            || typeName == "Arcane::PhysicsBodyRef"
            || typeName == "Arcane::Hidden";
    }

    bool IsStructureLocked(std::string_view typeName)
    {
        // The invisible set, plus identity. Identity is VISIBLE (name edits,
        // id view-only) but never user-added or user-removed -- the ECS
        // equivalent of AActor's intrinsic ActorLabel/ActorGuid
        // (Actor.h:1055/:1188), which are not components at all. Hidden rides
        // in via IsHiddenInInspector, and locking it matters beyond tidiness:
        // a catalogue-added Hidden would bypass the eye's descendant
        // recursion (half-hiding a subtree), and a menu-remove would desync
        // the state the eye believes it owns.
        return IsHiddenInInspector(typeName) || typeName == "Arcane::Identity";
    }

    int InspectorSectionRank(std::string_view typeName)
    {
        if (typeName == "Arcane::Identity")
            return 0;
        if (typeName == "Arcane::Transform")
            return 1;
        return 2;
    }

    std::vector<ComponentCatalogEntry> BuildComponentCatalog(
        Astra::Registry& reg,
        std::span<const Astra::Entity> selection,
        std::string_view filter)
    {
        std::vector<ComponentCatalogEntry> out;

        const Astra::ComponentRegistry* creg = reg.GetComponentRegistry();
        if (!creg)
            return out;

        creg->ForEachComponent([&](Astra::ComponentID, const Astra::ComponentDescriptor& desc)
        {
            if (!desc.meta)
                return;   // unreflected: nothing the Inspector could ever show
            // TypeMeta::typeName is a std::string_view into a substring of a
            // larger compile-time literal (__FUNCSIG__ / __PRETTY_FUNCTION__)
            // and is NOT guaranteed NUL-terminated -- copy it before anything
            // treats it as a C string (the ImGui popup does).
            std::string typeName(desc.meta->typeName);
            // The catalog is the Add Component source, so it must exclude
            // Identity too (structure-locked), not just the hidden caches --
            // IsHiddenInInspector alone would offer a redundant "add Identity"
            // row that Edit::AddComponent would answer with a fresh NIL Guid.
            if (IsStructureLocked(typeName))
                return;
            if (!ContainsCI(typeName, filter))
                return;

            ComponentCatalogEntry e;
            e.desc = &desc;   // points into ComponentRegistry's fixed array: stable
            for (Astra::Entity ent : selection)
            {
                // HasComponentByHash, NOT GetComponentByHash: an empty (tag)
                // component has no storage array, so the getter returns null
                // even when the entity really carries it.
                if (reg.IsValid(ent) && !reg.HasComponentByHash(ent, desc.hash))
                    ++e.missingCount;
            }
            e.typeName = std::move(typeName);
            out.push_back(std::move(e));
        });

        // ForEachComponent walks ascending ComponentID = registration order,
        // which is meaningless to a user. Name order is the browsable one.
        std::sort(out.begin(), out.end(),
                  [](const ComponentCatalogEntry& a, const ComponentCatalogEntry& b)
                  { return a.typeName < b.typeName; });
        return out;
    }
}
