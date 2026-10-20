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

        // The epoch FIRST: a module from an older epoch (HotReloadPluginBad, an
        // old game) fails here without being required to export the digest.
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

        // Epoch matches: now the digest export is required (a missing symbol is
        // the unresolved-symbol failure, not AbiMismatch), then compared.
        out.ABIDigest = Resolve<std::uint64_t(*)()>(module, Arcane::PluginEntry::kABIDigest);
        if (!need(out.ABIDigest, Arcane::PluginEntry::kABIDigest)) return false;
        return Arcane::AcceptPluginAbi(out, error);
    }

    bool ResolveGamePluginAbi(Arcane::Module& module, Arcane::PluginVTable& out) noexcept
    {
        return ResolveGamePluginAbi(module, out, nullptr);
    }
}

namespace Arcane
{
    Plugin::Plugin(Module module, PluginVTable vtable, std::string cvarModule) noexcept
        : m_module(std::move(module)), m_vtable(vtable)
    {
        const Module::ImageSpan image = m_module.Image();
        m_cvars = CVarOwner(std::move(cvarModule), image.base, image.size);
    }

    bool AcceptPluginAbi(const PluginVTable& table, PluginResolveError* error) noexcept
    {
        const auto refuse = [&](std::uint32_t pluginAbi, std::uint64_t pluginDigest) -> bool
        {
            if (error)
            {
                error->kind         = PluginResolveError::Kind::AbiMismatch;
                error->pluginAbi    = pluginAbi;
                error->engineAbi    = kGamePluginABIVersion;
                error->pluginDigest = pluginDigest;
                error->engineDigest = kGamePluginABIDigest;
            }
            return false;
        };
        if (!table.ABIVersion) return refuse(0, 0);
        const std::uint32_t pluginAbi = table.ABIVersion();
        if (pluginAbi != kGamePluginABIVersion) return refuse(pluginAbi, 0);
        if (!table.ABIDigest) return refuse(pluginAbi, 0);
        const std::uint64_t pluginDigest = table.ABIDigest();
        if (pluginDigest != kGamePluginABIDigest) return refuse(pluginAbi, pluginDigest);
        return true;
    }

    bool AdoptStaticGameModule(const PluginVTable& table, PluginResolveError* error,
                               std::optional<Plugin>* out, std::string cvarModule)
    {
        if (!AcceptPluginAbi(table, error)) return false;
        if (out) out->emplace(Plugin::FromStatic(table, std::move(cvarModule)));
        return true;
    }

    Plugin Plugin::FromStatic(const PluginVTable& table, std::string cvarModule)
    {
        // Module{} is the empty handle: Image() reports {}, so the ctor's
        // CVarOwner falls back to name-only ownership (UnregisterModule).
        Plugin plugin(Module{}, table, std::move(cvarModule));
        plugin.m_staticTable = &table;
        return plugin;
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
        // Load: the loader only bumps its reference. Compare the pre-load
        // image span to the post-load span: a match means this Load owns
        // none of the registrations in that range and must not drop them.
        const Module::ImageSpan already = Module::MappedImage(path);
        std::optional<Module> module;
        {
            const CVarModuleScope scope(cvarModule);
            module = Module::Load(std::move(path));
        }
        if (!module)
        {
            // Load failed with no handle: statics may have run then the OS
            // unmapped. Name-drop only when this path was not already mapped.
            if (!already.base)
                CVarRegistry::Get().UnregisterModule(cvarModule);
            return std::nullopt;   // Kind::None; caller reads Module::LastLoadError()
        }

        const Module::ImageSpan loaded = module->Image();
        const bool alreadyMapped = already.base && already.base == loaded.base
            && already.size == loaded.size;

        PluginVTable vtable{};
        if (!ResolveGamePluginAbi(*module, vtable, error))
        {
            // Refused AFTER its statics ran: drop THIS image's registrations
            // by range (not by stem) before `module` unmaps the code they
            // point into. An already-mapped engine DLL shares this span with
            // the live owner -- leave it.
            if (!alreadyMapped)
                CVarRegistry::Get().UnregisterModuleRange(loaded.base, loaded.size);
            return std::nullopt;
        }

        CVarRegistry::Get().RegisterModuleImage(cvarModule, loaded.base, loaded.size);
        return Plugin(std::move(*module), vtable, cvarModule);
    }
}
