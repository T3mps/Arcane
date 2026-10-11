#include "Documents/InputPendingAdd.hpp"

#include "Documents/InputActionsEditorModel.hpp"
#include "Documents/InputActionsJson.hpp"

#include <algorithm>
#include <utility>

namespace Arcane::Editor
{
    std::vector<std::string> CompositeRoles(std::string_view composite)
    {
        if (composite == "1DAxis")   return { "negative", "positive" };
        if (composite == "2DVector") return { "up", "down", "left", "right" };
        return {};
    }

    std::vector<std::string> PrefillGroups(const nlohmann::json& draft, std::string_view schemeFilter)
    {
        if (!SchemeGroupExists(draft, schemeFilter)) return {};
        return { std::string(schemeFilter) };
    }

    PendingAdd MakeAddBinding(Guid map, Guid action, std::vector<std::string> groups)
    {
        PendingAdd p;
        p.kind = PendingAdd::Kind::Binding;
        p.map = map; p.action = action;
        p.roles = { "binding" };
        p.groups = std::move(groups);
        return p;
    }

    PendingAdd MakeAddComposite(Guid map, Guid action, std::string composite, std::vector<std::string> groups)
    {
        PendingAdd p;
        p.kind = PendingAdd::Kind::Composite;
        p.map = map; p.action = action;
        p.roles = CompositeRoles(composite);
        p.composite = std::move(composite);
        p.groups = std::move(groups);
        return p;
    }

    PendingAdd MakeAddPart(Guid binding, std::string role)
    {
        PendingAdd p;
        p.kind = PendingAdd::Kind::Part;
        p.binding = binding;
        p.roles = { std::move(role) };
        return p;
    }

    std::optional<PendingAdd> MakeRebindComposite(const nlohmann::json& draft, Guid binding)
    {
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return std::nullopt;
        for (const auto& m : draft["actionMaps"])
        {
            if (!m.is_object() || !m.contains("actions") || !m["actions"].is_array()) continue;
            for (const auto& a : m["actions"])
            {
                if (!a.is_object() || !a.contains("bindings") || !a["bindings"].is_array()) continue;
                for (const auto& b : a["bindings"])
                {
                    if (!IdIs(b, binding)) continue;
                    const std::string composite = Str(b, "composite");
                    if (CompositeRoles(composite).empty() || !b.contains("parts") || !b["parts"].is_array()) return std::nullopt;
                    PendingAdd p;
                    p.kind = PendingAdd::Kind::RebindComposite;
                    p.map = IdOf(m); p.action = IdOf(a); p.binding = binding; p.composite = composite;
                    for (const auto& part : b["parts"])
                        if (const Guid id = IdOf(part); id.IsValid()) { p.parts.push_back(id); p.roles.push_back(Str(part, "name")); }
                    if (p.parts.empty()) return std::nullopt;
                    return p;
                }
            }
        }
        return std::nullopt;
    }

    bool CommitPending(InputActionsEditorModel& model, const PendingAdd& add)
    {
        if (add.captured.empty()) return false;
        const std::size_t heard = std::min(add.captured.size(), add.roles.size());
        switch (add.kind)
        {
        case PendingAdd::Kind::Binding:
            return model.AddBinding(add.map, add.action, add.captured[0], add.groups);
        case PendingAdd::Kind::Composite:
        {
            std::vector<std::pair<std::string, std::string>> parts;
            for (std::size_t i = 0; i < heard; ++i) parts.emplace_back(add.roles[i], add.captured[i]);
            return model.AddComposite(add.map, add.action, add.composite, std::move(parts), add.groups);
        }
        case PendingAdd::Kind::Part:
            return model.AddPart(add.binding, add.roles.empty() ? std::string{} : add.roles[0], add.captured[0]);
        case PendingAdd::Kind::RebindComposite:
        {
            std::vector<std::pair<Guid, std::string>> paths;
            for (std::size_t i = 0; i < heard && i < add.parts.size(); ++i) paths.emplace_back(add.parts[i], add.captured[i]);
            return model.SetPartPaths(add.binding, std::move(paths));
        }
        }
        return false;
    }
}
