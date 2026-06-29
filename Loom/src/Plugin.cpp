#include "Plugin.hpp"

#include <cstdint>
#include <utility>

namespace
{
    template<typename T>
    T Resolve(Module& module, const char* name) noexcept
    {
        return reinterpret_cast<T>(module.Symbol(name));
    }

    bool ResolveGamePluginAbi(Module& module, Arcane::PluginVTable& out) noexcept
    {
        out.ABIVersion = Resolve<std::uint32_t(*)()>(module, Arcane::PluginEntry::kABIVersion);
        out.Init = Resolve<bool(*)(Arcane::EngineContext*)>(module, Arcane::PluginEntry::kInit);
        out.Shutdown = Resolve<void(*)()>(module, Arcane::PluginEntry::kShutdown);
        out.FixedUpdate = Resolve<void(*)(double)>(module, Arcane::PluginEntry::kFixedUpdate);
        out.Update = Resolve<void(*)(double, double)>(module, Arcane::PluginEntry::kUpdate);
        out.SaveState = Resolve<void(*)(Astra::BinaryWriter&)>(module, Arcane::PluginEntry::kSaveState);
        out.LoadState = Resolve<bool(*)(Astra::BinaryReader&)>(module, Arcane::PluginEntry::kLoadState);

        if (!out.ABIVersion || !out.Init || !out.Shutdown || !out.FixedUpdate ||
            !out.Update || !out.SaveState || !out.LoadState)
        {
            return false;
        }

        out.DrawUI = Resolve<void(*)()>(module, Arcane::PluginEntry::kDrawUI);
        return out.ABIVersion() == Arcane::kGamePluginABIVersion;
    }
}

Plugin::Plugin(Module module, Arcane::PluginVTable vtable) noexcept
    : m_module(std::move(module)), m_vtable(vtable)
{
}

std::optional<Plugin> Plugin::Load(std::filesystem::path path)
{
    std::optional<Module> module = Module::Load(std::move(path));
    if (!module)
        return std::nullopt;

    Arcane::PluginVTable vtable{};
    if (!ResolveGamePluginAbi(*module, vtable))
        return std::nullopt;

    return Plugin(std::move(*module), vtable);
}
