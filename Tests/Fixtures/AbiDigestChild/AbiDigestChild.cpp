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

#include <Arcane/Plugin/Plugin.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
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
