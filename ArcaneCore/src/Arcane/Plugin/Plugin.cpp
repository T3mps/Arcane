#include <Arcane/Plugin/Plugin.hpp>

#include <Arcane/Config/CVarRegistry.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace
{
    template<typename T>
    T Resolve(Arcane::Module& module, const char* name) noexcept
    {
        return reinterpret_cast<T>(module.Symbol(name));
    }

    // WHICH of the seven required exports is missing (in declaration order), and
    // BOTH ABI version numbers on a mismatch, so a caller can tell "the DLL isn't
    // an Arcane plugin at all" from "it's an Arcane plugin, built against the
    // wrong engine" instead of one bare bool. `error` may be null (the plain
    // Plugin::Load(path) overload forwards nullptr) -- every write below is
    // guarded on that.
    bool ResolveGamePluginAbi(Arcane::Module& module, Arcane::PluginVTable& out,
                              Arcane::PluginResolveError* error) noexcept
    {
        const auto need = [&](auto& slot, const char* name) -> bool
        {
            if (slot) return true;
            if (error)
            {
                error->kind   = Arcane::PluginResolveError::Kind::MissingExport;
                error->symbol = name;
            }
            return false;
        };

        // Resolve ALL seven first, then check them in declaration order below --
        // so the FIRST missing symbol in declaration order is the one reported,
        // regardless of which symbols the DLL happens to export.
        out.ABIVersion  = Resolve<std::uint32_t(*)()>(module, Arcane::PluginEntry::kABIVersion);
        out.Init        = Resolve<bool(*)(Arcane::EngineContext*)>(module, Arcane::PluginEntry::kInit);
        out.Shutdown    = Resolve<void(*)()>(module, Arcane::PluginEntry::kShutdown);
        out.FixedUpdate = Resolve<void(*)(double)>(module, Arcane::PluginEntry::kFixedUpdate);
        out.Update      = Resolve<void(*)(double, double)>(module, Arcane::PluginEntry::kUpdate);
        out.SaveState   = Resolve<void(*)(Astra::BinaryWriter&)>(module, Arcane::PluginEntry::kSaveState);
        out.LoadState   = Resolve<bool(*)(Astra::BinaryReader&)>(module, Arcane::PluginEntry::kLoadState);

        if (!need(out.ABIVersion,  Arcane::PluginEntry::kABIVersion))  return false;
        if (!need(out.Init,        Arcane::PluginEntry::kInit))        return false;
        if (!need(out.Shutdown,    Arcane::PluginEntry::kShutdown))    return false;
        if (!need(out.FixedUpdate, Arcane::PluginEntry::kFixedUpdate)) return false;
        if (!need(out.Update,      Arcane::PluginEntry::kUpdate))      return false;
        if (!need(out.SaveState,   Arcane::PluginEntry::kSaveState))   return false;
        if (!need(out.LoadState,   Arcane::PluginEntry::kLoadState))   return false;

        // Optional: resolution is lenient, the host null-checks before calling.
        out.DrawUI = Resolve<void(*)()>(module, Arcane::PluginEntry::kDrawUI);

        const std::uint32_t pluginAbi = out.ABIVersion();
        if (pluginAbi != Arcane::kGamePluginABIVersion)
        {
            if (error)
            {
                error->kind      = Arcane::PluginResolveError::Kind::AbiMismatch;
                error->pluginAbi = pluginAbi;
                error->engineAbi = Arcane::kGamePluginABIVersion;
            }
            return false;
        }
        return true;
    }

    bool ResolveGamePluginAbi(Arcane::Module& module, Arcane::PluginVTable& out) noexcept
    {
        return ResolveGamePluginAbi(module, out, nullptr);
    }
}

namespace Arcane
{
    Plugin::Plugin(Module module, PluginVTable vtable, std::string cvarModule) noexcept
        : m_module(std::move(module)), m_vtable(vtable), m_cvars(std::move(cvarModule))
    {
    }

    std::optional<Plugin> Plugin::Load(std::filesystem::path path)
    {
        return Load(std::move(path), nullptr);
    }

    std::optional<Plugin> Plugin::Load(std::filesystem::path path, PluginResolveError* error)
    {
#if defined(_WIN32)
        // Build-flavor gate, from the FILE bytes and BEFORE Module::Load: a
        // config-mismatched DLL's static initializers run inside LoadLibrary
        // itself, so by the time an export could be checked the damage is done
        // (the desk signature was an AV inside VCRUNTIME140_1 at plugin_load).
        // Unknown fails OPEN -- only a positive cross-flavor verdict refuses.
        {
#if defined(_DEBUG)
            constexpr bool hostDebugCrt = true;
#else
            constexpr bool hostDebugCrt = false;
#endif
            std::string matched;
            const CrtFlavor flavor = Module::ScanFileCrtFlavor(path, &matched);
            if (flavor != CrtFlavor::Unknown && (flavor == CrtFlavor::Debug) != hostDebugCrt)
            {
                if (error)
                {
                    error->kind           = PluginResolveError::Kind::CrtFlavorMismatch;
                    error->symbol         = std::move(matched);
                    error->pluginDebugCrt = (flavor == CrtFlavor::Debug);
                    error->hostDebugCrt   = hostDebugCrt;
                }
                return std::nullopt;
            }
        }
#endif

        // The image's ARC_CVAR/ARC_COMMAND statics run INSIDE Module::Load, on
        // this thread (settings spec s4.3). Attribute them to the open
        // CVarModuleScope (PluginHost opens one naming the source dll), else to
        // this file's stem.
        const std::string cvarModule = CVarRegistry::ScopedModule().empty()
            ? path.stem().string()
            : std::string(CVarRegistry::ScopedModule());
        // An image this process has ALREADY mapped (an engine DLL named as a
        // plugin by mistake, say ArcaneClient.dll) runs no statics in this
        // Load: the loader only bumps its reference. Whatever is registered
        // under its name belongs to the live owner, so a refusal below must
        // not drop it. UnregisterModule is keyed by NAME, not by image; tagging
        // registrations by image is Review Focus 2 half (b), owed to S2-3.
        const bool alreadyMapped = Module::IsMapped(path);
        std::optional<Module> module;
        {
            const CVarModuleScope scope(cvarModule);
            module = Module::Load(std::move(path));
        }
        if (!module)
        {
            if (!alreadyMapped)
                CVarRegistry::Get().UnregisterModule(cvarModule);   // a failing DllMain may have run some statics
            return std::nullopt;   // Kind::None; caller reads Module::LastLoadError()
        }

        PluginVTable vtable{};
        if (!ResolveGamePluginAbi(*module, vtable, error))
        {
            // Refused AFTER its statics ran: drop their registrations before
            // `module` unmaps the code they point into.
            if (!alreadyMapped)
                CVarRegistry::Get().UnregisterModule(cvarModule);
            return std::nullopt;
        }

        return Plugin(std::move(*module), vtable, cvarModule);
    }
}
