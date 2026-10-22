// 3D-readiness Plan B Task 2: the disposable child for the plugin ABI-digest
// load gate. Usage: AbiDigestChild <module> <sentinel>
// Calls Plugin::Load on the module and prints the PluginResolveError as
// `key=value` lines (loaded, kind, symbol, pluginAbi, engineAbi, pluginDigest,
// engineDigest; digests as 16 hex digits), so the parent can tell a refusal at
// the digest compare (equal epochs, different digests) from one at the epoch.
// Exits 0 exactly when the load was refused with AbiMismatch. If the load
// SUCCEEDS, the child calls Init(nullptr): every fixture's Init writes the
// sentinel, so a wrong accept leaves the sentinel the parent checks for. The
// parent test spawns it once per fixture DLL; a module's header registrars may
// run inside this process at LoadLibrary, which is why the load never happens
// in ArcaneTests.
//
// Second mode (3D-readiness Plan A Task 8 fix round 1): AbiDigestChild
// --sim-hook <module>. This exe links ArcaneEngine only; ArcanePhysics2D is
// staged beside it, not linked, so a game module that imports it maps it at
// load, AFTER ProcessContext::Create sealed the sim hooks. The child loads the
// module through a real PluginHost, prints the hook count before the load,
// after it and after the unload, whether ArcanePhysics2D was mapped, then
// builds a fresh Runtime and runs ClearSystems, EnsurePhysics and one 1/60
// Advance. Exits 0 when the hook list never changed and nothing faulted.

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/Plugin.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Sim/SimHook.hpp>
#include <Astra/Core/TypeContext.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace
{
    bool Physics2DMapped()
    {
        for (const Arcane::Module::MappedModule& m : Arcane::Module::MappedModules())
            if (m.path.stem().string() == "ArcanePhysics2D")
                return true;
        return false;
    }

    int RunSimHookMode(const char* module)
    {
        auto process = Arcane::ProcessContext::Create({});
        if (!process)
            return 10;
        // This exe's own Astra slot, as every host sets its own (R13): the inline
        // RunLoop / Registry code below runs in this image.
        Astra::SetTypeContext(&process->TypeContext(), Astra::ModuleResidency::Resident);
        const std::size_t before = Arcane::SimHooks().size();
        const bool mappedBefore = Physics2DMapped();
        std::size_t afterLoad = 0, afterUnload = 0;
        bool loaded = false, mappedAtLoad = false;
        {
            Arcane::Runtime rt(*process);
            Arcane::PluginHost host(*process, std::filesystem::path(module));
            if (!host.AttachRuntime(rt))
                return 11;
            loaded = host.Load();
            mappedAtLoad = Physics2DMapped();
            afterLoad = Arcane::SimHooks().size();
            host.Unload();
            afterUnload = Arcane::SimHooks().size();
            host.DetachRuntime(rt);
        }
        std::printf("loaded=%d\nsealed=%d\nphysics2dMappedBefore=%d\nphysics2dMappedAtLoad=%d\n"
                    "hooksBefore=%zu\nhooksAfterLoad=%zu\nhooksAfterUnload=%zu\n",
                    loaded ? 1 : 0, Arcane::SimHooksSealed() ? 1 : 0, mappedBefore ? 1 : 0, mappedAtLoad ? 1 : 0,
                    before, afterLoad, afterUnload);
        std::fflush(stdout);
        // A fresh world after the unload: nothing the module left may run.
        {
            Arcane::Runtime rt(*process);
            rt.ClearSystems();
            rt.EnsurePhysics();
            (void)rt.Loop().Advance(1.0 / 60.0);
        }
        std::printf("advanced=1\n");
        std::fflush(stdout);
        const bool ok = loaded && before == afterLoad && before == afterUnload;
        return ok ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--sim-hook") == 0)
        return RunSimHookMode(argv[2]);
    if (argc < 3)
    {
        std::fputs("usage: AbiDigestChild <module> <sentinel>\n", stderr);
        return 2;
    }
#if defined(_WIN32)
    _putenv_s("ARC_ABI_DIGEST_SENTINEL", argv[2]);
#else
    setenv("ARC_ABI_DIGEST_SENTINEL", argv[2], 1);
#endif
    Arcane::PluginResolveError error{};
    auto plugin = Arcane::Plugin::Load(std::filesystem::path(argv[1]), &error);

    const char* kind = "None";
    switch (error.kind)
    {
        case Arcane::PluginResolveError::Kind::None:              kind = "None"; break;
        case Arcane::PluginResolveError::Kind::MissingExport:     kind = "MissingExport"; break;
        case Arcane::PluginResolveError::Kind::AbiMismatch:       kind = "AbiMismatch"; break;
        case Arcane::PluginResolveError::Kind::CrtFlavorMismatch: kind = "CrtFlavorMismatch"; break;
    }
    std::printf("loaded=%d\nkind=%s\nsymbol=%s\npluginAbi=%u\nengineAbi=%u\npluginDigest=%016llx\nengineDigest=%016llx\n",
                plugin.has_value() ? 1 : 0, kind, error.symbol.c_str(),
                static_cast<unsigned>(error.pluginAbi), static_cast<unsigned>(error.engineAbi),
                static_cast<unsigned long long>(error.pluginDigest),
                static_cast<unsigned long long>(error.engineDigest));
    std::fflush(stdout);
    // A wrong accept: run Init so the fixture writes the sentinel. The fixtures'
    // Init ignores the context.
    if (plugin.has_value() && plugin->VTable().Init)
        plugin->VTable().Init(nullptr);
    return (!plugin.has_value() && error.kind == Arcane::PluginResolveError::Kind::AbiMismatch) ? 0 : 1;
}
