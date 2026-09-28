#include <Arcane/Input/InputActionAsset.hpp>

#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace Arcane
{
    namespace
    {
        using Json = nlohmann::json;

        bool Reject(std::string& error, std::string_view where, std::string_view why)
        {
            error = std::string(where) + ": " + std::string(why);
            return false;
        }

        bool RequiredString(const Json& node, const char* key, std::string& value,
                            std::string& error, const std::string& where)
        {
            const auto it = node.find(key);
            if (it == node.end() || !it->is_string() || it->get_ref<const std::string&>().empty())
                return Reject(error, where, std::string(key) + " must be a nonempty string");
            value = it->get<std::string>();
            return true;
        }

        bool RequiredArray(const Json& node, const char* key, const Json*& value,
                           std::string& error, const std::string& where)
        {
            const auto it = node.find(key);
            if (it == node.end() || !it->is_array())
                return Reject(error, where, std::string(key) + " must be an array");
            value = &*it;
            return true;
        }

        bool Identity(const Json& node, Guid& value, std::unordered_set<Guid>& seen,
                      std::string& error, const std::string& where)
        {
            std::string idText;
            if (!RequiredString(node, "id", idText, error, where))
                return false;
            const auto parsed = Guid::FromString(idText);
            if (!parsed || parsed->IsNil())
                return Reject(error, where, "id must be a non-nil GUID");
            if (!seen.insert(*parsed).second)
                return Reject(error, where, "duplicate id " + idText);
            value = *parsed;
            return true;
        }

        bool OptionalStrings(const Json& node, const char* key, std::vector<std::string>& values,
                             std::string& error, const std::string& where)
        {
            values.clear();
            const auto it = node.find(key);
            if (it == node.end())
                return true;
            if (!it->is_array())
                return Reject(error, where, std::string(key) + " must be an array");
            for (const Json& item : *it)
            {
                if (!item.is_string() || item.get_ref<const std::string&>().empty())
                    return Reject(error, where, std::string(key) + " entries must be nonempty strings");
                values.push_back(item.get<std::string>());
            }
            return true;
        }

        bool Groups(const Json& node, std::vector<std::string>& groups,
                    const std::unordered_set<std::string>& knownGroups,
                    std::string& error, const std::string& where)
        {
            if (!OptionalStrings(node, "groups", groups, error, where))
                return false;
            std::unordered_set<std::string> unique;
            for (const std::string& group : groups)
            {
                if (!knownGroups.contains(group))
                    return Reject(error, where, "unknown control scheme group " + group);
                if (!unique.insert(group).second)
                    return Reject(error, where, "duplicate control scheme group " + group);
            }
            return true;
        }

        bool ValidPartRole(std::string_view composite, std::string_view role)
        {
            if (composite == "1DAxis")
                return role == "negative" || role == "positive";
            if (composite == "2DVector")
                return role == "up" || role == "down" || role == "left" || role == "right";
            return false;
        }

        bool ParsePart(const Json& node, InputBindingPartDefinition& part,
                       std::string_view composite, std::unordered_set<Guid>& seen,
                       const std::unordered_set<std::string>& knownGroups,
                       std::string& error, const std::string& where)
        {
            if (!node.is_object())
                return Reject(error, where, "part must be an object");
            if (!Identity(node, part.id, seen, error, where) ||
                !RequiredString(node, "name", part.name, error, where) ||
                !RequiredString(node, "path", part.path, error, where))
                return false;
            if (!ValidPartRole(composite, part.name))
                return Reject(error, where, "invalid composite part role " + part.name);
            if (!OptionalStrings(node, "processors", part.processors, error, where) ||
                !OptionalStrings(node, "interactions", part.interactions, error, where) ||
                !Groups(node, part.groups, knownGroups, error, where))
                return false;
            part.sourceJson = node;
            return true;
        }

        bool ParseBinding(const Json& node, InputBindingDefinition& binding,
                          std::unordered_set<Guid>& seen,
                          const std::unordered_set<std::string>& knownGroups,
                          std::string& error, const std::string& where)
        {
            if (!node.is_object())
                return Reject(error, where, "binding must be an object");
            if (!Identity(node, binding.id, seen, error, where) ||
                !OptionalStrings(node, "processors", binding.processors, error, where) ||
                !OptionalStrings(node, "interactions", binding.interactions, error, where) ||
                !Groups(node, binding.groups, knownGroups, error, where))
                return false;

            const auto composite = node.find("composite");
            if (composite == node.end())
            {
                if (!RequiredString(node, "path", binding.path, error, where))
                    return false;
                if (node.contains("parts"))
                    return Reject(error, where, "simple binding cannot contain composite parts");
            }
            else
            {
                if (!composite->is_string())
                    return Reject(error, where, "composite must be a string");
                binding.composite = composite->get<std::string>();
                if (binding.composite != "1DAxis" && binding.composite != "2DVector")
                    return Reject(error, where, "unsupported composite " + binding.composite);
                if (node.contains("path"))
                    return Reject(error, where, "composite binding cannot have a path");
                const Json* parts = nullptr;
                if (!RequiredArray(node, "parts", parts, error, where))
                    return false;
                if (parts->empty())
                    return Reject(error, where, "composite must contain parts");
                for (std::size_t i = 0; i < parts->size(); ++i)
                {
                    InputBindingPartDefinition part;
                    const std::string partWhere = where + " part[" + std::to_string(i) + "]";
                    if (!ParsePart((*parts)[i], part, binding.composite, seen, knownGroups,
                                   error, partWhere))
                        return false;
                    binding.parts.push_back(std::move(part));
                }
            }
            binding.sourceJson = node;
            return true;
        }

        bool ParseAction(const Json& node, InputActionDefinition& action,
                         std::unordered_set<Guid>& seen,
                         const std::unordered_set<std::string>& knownGroups,
                         std::string& error, const std::string& where)
        {
            if (!node.is_object())
                return Reject(error, where, "action must be an object");
            if (!RequiredString(node, "name", action.name, error, where))
                return false;
            const std::string actionWhere = where + " action \"" + action.name + "\"";
            if (!Identity(node, action.id, seen, error, actionWhere))
                return false;
            std::string type;
            if (!RequiredString(node, "type", type, error, actionWhere))
                return false;
            if (type == "Button") action.type = InputActionType::Button;
            else if (type == "Axis1D") action.type = InputActionType::Axis1D;
            else if (type == "Axis2D") action.type = InputActionType::Axis2D;
            else return Reject(error, actionWhere, "unsupported action type " + type);
            if (!OptionalStrings(node, "processors", action.processors, error, actionWhere) ||
                !OptionalStrings(node, "interactions", action.interactions, error, actionWhere))
                return false;
            const Json* bindings = nullptr;
            if (!RequiredArray(node, "bindings", bindings, error, actionWhere))
                return false;
            for (std::size_t i = 0; i < bindings->size(); ++i)
            {
                InputBindingDefinition binding;
                const std::string bindingWhere = actionWhere + " binding[" + std::to_string(i) + "]";
                if (!ParseBinding((*bindings)[i], binding, seen, knownGroups,
                                  error, bindingWhere))
                    return false;
                action.bindings.push_back(std::move(binding));
            }
            action.sourceJson = node;
            return true;
        }

        bool ParseMap(const Json& node, InputActionMapDefinition& map,
                      std::unordered_set<Guid>& seen,
                      const std::unordered_set<std::string>& knownGroups,
                      std::string& error, const std::string& where)
        {
            if (!node.is_object())
                return Reject(error, where, "map must be an object");
            if (!RequiredString(node, "name", map.name, error, where))
                return false;
            const std::string mapWhere = where + " map \"" + map.name + "\"";
            if (!Identity(node, map.id, seen, error, mapWhere))
                return false;
            if (const auto it = node.find("blocking"); it != node.end())
            {
                if (!it->is_boolean())
                    return Reject(error, mapWhere, "blocking must be a boolean");
                map.blocking = it->get<bool>();
            }
            if (const auto it = node.find("priority"); it != node.end())
            {
                if (!it->is_number_integer())
                    return Reject(error, mapWhere, "priority must be an integer");
                const auto value = it->get<std::int64_t>();
                if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
                    return Reject(error, mapWhere, "priority is out of range");
                map.priority = static_cast<int>(value);
            }
            const Json* actions = nullptr;
            if (!RequiredArray(node, "actions", actions, error, mapWhere))
                return false;
            for (std::size_t i = 0; i < actions->size(); ++i)
            {
                InputActionDefinition action;
                const std::string actionWhere = mapWhere + " action[" + std::to_string(i) + "]";
                if (!ParseAction((*actions)[i], action, seen, knownGroups,
                                 error, actionWhere))
                    return false;
                map.actions.push_back(std::move(action));
            }
            map.sourceJson = node;
            return true;
        }

        Json Base(const Json& source)
        {
            return source.is_object() ? source : Json::object();
        }

        void OptionalArray(Json& target, const char* key, const std::vector<std::string>& values)
        {
            if (target.contains(key) || !values.empty())
                target[key] = values;
        }

        Json PartJson(const InputBindingPartDefinition& part)
        {
            Json result = Base(part.sourceJson);
            result["id"] = part.id.ToString();
            result["name"] = part.name;
            result["path"] = part.path;
            OptionalArray(result, "processors", part.processors);
            OptionalArray(result, "interactions", part.interactions);
            OptionalArray(result, "groups", part.groups);
            return result;
        }

        Json BindingJson(const InputBindingDefinition& binding)
        {
            Json result = Base(binding.sourceJson);
            result["id"] = binding.id.ToString();
            if (binding.composite.empty())
            {
                result["path"] = binding.path;
                result.erase("composite");
                result.erase("parts");
            }
            else
            {
                result["composite"] = binding.composite;
                result.erase("path");
                result["parts"] = Json::array();
                for (const auto& part : binding.parts)
                    result["parts"].push_back(PartJson(part));
            }
            OptionalArray(result, "processors", binding.processors);
            OptionalArray(result, "interactions", binding.interactions);
            OptionalArray(result, "groups", binding.groups);
            return result;
        }

        Json ActionJson(const InputActionDefinition& action)
        {
            Json result = Base(action.sourceJson);
            result["id"] = action.id.ToString();
            result["name"] = action.name;
            switch (action.type)
            {
            case InputActionType::Button: result["type"] = "Button"; break;
            case InputActionType::Axis1D: result["type"] = "Axis1D"; break;
            case InputActionType::Axis2D: result["type"] = "Axis2D"; break;
            }
            result["bindings"] = Json::array();
            for (const auto& binding : action.bindings)
                result["bindings"].push_back(BindingJson(binding));
            OptionalArray(result, "processors", action.processors);
            OptionalArray(result, "interactions", action.interactions);
            return result;
        }

        Json MapJson(const InputActionMapDefinition& map)
        {
            Json result = Base(map.sourceJson);
            result["id"] = map.id.ToString();
            result["name"] = map.name;
            if (result.contains("blocking") || map.blocking)
                result["blocking"] = map.blocking;
            if (result.contains("priority") || map.priority != 0)
                result["priority"] = map.priority;
            result["actions"] = Json::array();
            for (const auto& action : map.actions)
                result["actions"].push_back(ActionJson(action));
            return result;
        }

        Json SchemeJson(const InputControlSchemeDefinition& scheme)
        {
            Json result = Base(scheme.sourceJson);
            result["id"] = scheme.id.ToString();
            result["name"] = scheme.name;
            result["bindingGroup"] = scheme.bindingGroup;
            return result;
        }
    }

    std::optional<InputActionAsset> InputActionAsset::FromJson(
        const nlohmann::json& document, std::string* error)
    {
        std::string why;
        if (error) error->clear();
        auto fail = [&]() -> std::optional<InputActionAsset>
        {
            if (error) *error = why;
            return std::nullopt;
        };
        if (!document.is_object())
        {
            Reject(why, "asset", "document must be an object");
            return fail();
        }
        const auto version = document.find("version");
        if (version == document.end() || !version->is_number_integer() || *version != 1)
        {
            Reject(why, "asset", "unsupported or missing version (expected 1)");
            return fail();
        }

        InputActionAsset asset;
        std::unordered_set<Guid> seen;
        if (!Identity(document, asset.id, seen, why, "asset"))
            return fail();

        const Json* schemes = nullptr;
        if (!RequiredArray(document, "controlSchemes", schemes, why, "asset"))
            return fail();
        std::unordered_set<std::string> knownGroups;
        std::unordered_set<std::string> schemeNames;
        for (std::size_t i = 0; i < schemes->size(); ++i)
        {
            const Json& node = (*schemes)[i];
            const std::string where = "asset controlSchemes[" + std::to_string(i) + "]";
            if (!node.is_object())
            {
                Reject(why, where, "scheme must be an object");
                return fail();
            }
            InputControlSchemeDefinition scheme;
            if (!RequiredString(node, "name", scheme.name, why, where) ||
                !Identity(node, scheme.id, seen, why, where + " \"" + scheme.name + "\"") ||
                !RequiredString(node, "bindingGroup", scheme.bindingGroup, why, where))
                return fail();
            if (!schemeNames.insert(scheme.name).second ||
                !knownGroups.insert(scheme.bindingGroup).second)
            {
                Reject(why, where, "duplicate scheme name or bindingGroup");
                return fail();
            }
            scheme.sourceJson = node;
            asset.controlSchemes.push_back(std::move(scheme));
        }

        const Json* maps = nullptr;
        if (!RequiredArray(document, "actionMaps", maps, why, "asset"))
            return fail();
        std::unordered_set<Guid> mapIds;
        for (std::size_t i = 0; i < maps->size(); ++i)
        {
            InputActionMapDefinition map;
            const std::string where = "asset actionMaps[" + std::to_string(i) + "]";
            if (!ParseMap((*maps)[i], map, seen, knownGroups, why, where))
                return fail();
            mapIds.insert(map.id);
            asset.actionMaps.push_back(std::move(map));
        }

        if (const auto reference = document.find("defaultMap"); reference != document.end())
        {
            if (!reference->is_string())
            {
                Reject(why, "asset", "defaultMap must be a map GUID");
                return fail();
            }
            const auto parsed = Guid::FromString(reference->get<std::string>());
            if (!parsed || parsed->IsNil() || !mapIds.contains(*parsed))
            {
                Reject(why, "asset", "defaultMap does not reference an action map");
                return fail();
            }
            asset.defaultMap = *parsed;
        }
        else if (!asset.actionMaps.empty())
        {
            Reject(why, "asset", "defaultMap is required when actionMaps is nonempty");
            return fail();
        }

        asset.sourceJson_ = document;
        return asset;
    }

    nlohmann::json InputActionAsset::ToJson() const
    {
        Json result = Base(sourceJson_);
        result["version"] = 1;
        result["id"] = id.ToString();
        if (defaultMap)
            result["defaultMap"] = defaultMap->ToString();
        else
            result.erase("defaultMap");
        result["actionMaps"] = Json::array();
        for (const auto& map : actionMaps)
            result["actionMaps"].push_back(MapJson(map));
        result["controlSchemes"] = Json::array();
        for (const auto& scheme : controlSchemes)
            result["controlSchemes"].push_back(SchemeJson(scheme));
        return result;
    }

    InputActionAsset InputActionAsset::CreateDefault()
    {
        InputActionAsset asset;
        asset.id = Guid::Generate();
        return asset;
    }
}
