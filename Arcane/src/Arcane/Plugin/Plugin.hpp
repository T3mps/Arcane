#pragma once

// Plugin: the engine's current game-runtime plugin protocol -- BOTH hosts consume
// it (ArcaneRuntime and the editor), which is why it lives here and not in either
// exe. Today there is one
// plugin kind, so this class owns the GamePlugin_* ABI. If editor/tool plugins
// appear later, split this into GamePlugin.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <filesystem>
#include <optional>

namespace Arcane
{
    class ARCANE_API Plugin
    {
    public:
        Plugin(Plugin&&) noexcept = default;
        Plugin& operator=(Plugin&&) noexcept = default;

        Plugin(const Plugin&) = delete;
        Plugin& operator=(const Plugin&) = delete;

        static std::optional<Plugin> Load(std::filesystem::path path);

        [[nodiscard]] bool IsLoaded() const noexcept { return m_module.IsLoaded(); }
        [[nodiscard]] const Module& LoadedModule() const noexcept { return m_module; }
        [[nodiscard]] const PluginVTable& VTable() const noexcept { return m_vtable; }

    private:
        Plugin(Module module, PluginVTable vtable) noexcept;

        Module m_module;
        PluginVTable m_vtable{};
    };
}
