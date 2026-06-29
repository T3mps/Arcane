#pragma once

// Plugin: Loom's current game-runtime plugin protocol. Today there is one
// plugin kind, so this class owns the GamePlugin_* ABI. If editor/tool plugins
// appear later, split this into GamePlugin.

#include "Module.hpp"

#include <Arcane/Plugin/PluginABI.hpp>

#include <filesystem>
#include <optional>

class Plugin
{
public:
    Plugin(Plugin&&) noexcept = default;
    Plugin& operator=(Plugin&&) noexcept = default;

    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    static std::optional<Plugin> Load(std::filesystem::path path);

    [[nodiscard]] bool IsLoaded() const noexcept { return m_module.IsLoaded(); }
    [[nodiscard]] const Module& LoadedModule() const noexcept { return m_module; }
    [[nodiscard]] const Arcane::PluginVTable& VTable() const noexcept { return m_vtable; }

private:
    Plugin(Module module, Arcane::PluginVTable vtable) noexcept;

    Module m_module;
    Arcane::PluginVTable m_vtable{};
};
