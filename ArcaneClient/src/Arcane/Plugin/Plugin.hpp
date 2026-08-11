#pragma once

// Plugin: the engine's current game-runtime plugin protocol -- BOTH hosts consume
// it (ArcaneRuntime and the editor), which is why it lives here and not in either
// exe. Today there is one
// plugin kind, so this class owns the GamePlugin_* ABI. If editor/tool plugins
// appear later, split this into GamePlugin.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Arcane
{
    // WHICH of the three plugin-resolve causes occurred, once the module itself
    // loaded (a Module::Load failure is reported separately via
    // Module::LastLoadError() -- this struct stays at Kind::None for that case,
    // since ResolveGamePluginAbi never runs). Populated by Plugin::Load's
    // PluginResolveError* overload; the plain single-argument overload is a
    // thin wrapper that discards this detail (existing callers keep compiling).
    struct PluginResolveError
    {
        enum class Kind : std::uint8_t { None, MissingExport, AbiMismatch };
        Kind          kind      = Kind::None;
        std::string   symbol;              // Kind::MissingExport: the first missing export, by declaration order
        std::uint32_t pluginAbi = 0;       // Kind::AbiMismatch: what the plugin was built against
        std::uint32_t engineAbi = 0;       // Kind::AbiMismatch: what this engine enforces
    };

    class ARCANE_API Plugin
    {
    public:
        Plugin(Plugin&&) noexcept = default;
        Plugin& operator=(Plugin&&) noexcept = default;

        Plugin(const Plugin&) = delete;
        Plugin& operator=(const Plugin&) = delete;

        static std::optional<Plugin> Load(std::filesystem::path path);

        // Same load, plus WHICH resolve cause failed when it returns nullopt
        // (left untouched -- Kind::None -- if `error` is null or the module
        // itself failed to load; check Module::LastLoadError() for that case).
        static std::optional<Plugin> Load(std::filesystem::path path, PluginResolveError* error);

        [[nodiscard]] bool IsLoaded() const noexcept { return m_module.IsLoaded(); }
        [[nodiscard]] const Module& LoadedModule() const noexcept { return m_module; }
        [[nodiscard]] const PluginVTable& VTable() const noexcept { return m_vtable; }

    private:
        Plugin(Module module, PluginVTable vtable) noexcept;

        Module m_module;
        PluginVTable m_vtable{};
    };
}
