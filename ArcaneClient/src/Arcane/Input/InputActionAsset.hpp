#pragma once

// Versioned project gameplay input definitions. These are authoring values;
// InputActions compiles them into its snapshot evaluator at runtime.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <Json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Arcane
{
    enum class InputActionType : std::uint8_t { Button, Axis1D, Axis2D };

    struct InputBindingPartDefinition
    {
        Guid id;
        std::string name;
        std::string path;
        std::vector<std::string> processors;
        std::vector<std::string> interactions;
        std::vector<std::string> groups;
        nlohmann::json sourceJson = nlohmann::json::object();
    };

    struct InputBindingDefinition
    {
        Guid id;
        std::string path;
        std::string composite;
        std::vector<InputBindingPartDefinition> parts;
        std::vector<std::string> processors;
        std::vector<std::string> interactions;
        std::vector<std::string> groups;
        nlohmann::json sourceJson = nlohmann::json::object();
    };

    struct InputActionDefinition
    {
        Guid id;
        std::string name;
        InputActionType type = InputActionType::Button;
        std::vector<InputBindingDefinition> bindings;
        std::vector<std::string> processors;
        std::vector<std::string> interactions;
        nlohmann::json sourceJson = nlohmann::json::object();
    };

    struct InputActionMapDefinition
    {
        Guid id;
        std::string name;
        bool blocking = false;
        int priority = 0;
        std::vector<InputActionDefinition> actions;
        nlohmann::json sourceJson = nlohmann::json::object();
    };

    struct InputControlSchemeDefinition
    {
        Guid id;
        std::string name;
        std::string bindingGroup;
        nlohmann::json sourceJson = nlohmann::json::object();
    };

    class ARC_API InputActionAsset
    {
    public:
        Guid id;
        std::optional<Guid> defaultMap;
        std::vector<InputActionMapDefinition> actionMaps;
        std::vector<InputControlSchemeDefinition> controlSchemes;

        [[nodiscard]] static std::optional<InputActionAsset> FromJson(
            const nlohmann::json& document, std::string* error = nullptr);
        [[nodiscard]] nlohmann::json ToJson() const;
        [[nodiscard]] static InputActionAsset CreateDefault();

    private:
        nlohmann::json sourceJson_ = nlohmann::json::object();
    };
}
