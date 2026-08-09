// Plugin load diagnoses WHICH of the three failure causes occurred. Before this,
// LoadLibrary failure, a missing export, and an ABI mismatch all collapsed into
// one bool and one generic log line. ([plugin])

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Plugin/Plugin.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <DiagnosticStore.hpp>

TEST_CASE("A DLL that cannot be loaded reports the OS error", "[plugin][diagnostics]")
{
    const std::optional<Arcane::Module> m =
        Arcane::Module::Load("this-path-does-not-exist-arcane.dll");
    CHECK_FALSE(m.has_value());
    CHECK_FALSE(Arcane::Module::LastLoadError().empty());   // the OS reason, captured
}

TEST_CASE("An ABI-mismatched plugin reports BOTH version numbers", "[plugin][diagnostics]")
{
    // HotReloadPluginBad exports ABI = kGamePluginABIVersion + 999 on purpose
    // (Tests/plugins/HotReloadPlugin.cpp:4,48), so this is unambiguously the
    // AbiMismatch cause -- not a missing export and not a load failure.
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path("HotReloadPluginBad.dll"), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(error.pluginAbi == Arcane::kGamePluginABIVersion + 999);
    CHECK(error.engineAbi == Arcane::kGamePluginABIVersion);
}

TEST_CASE("A missing required export is named", "[plugin][diagnostics]")
{
    // Arcane.dll itself loads fine as a module but exports none of the plugin
    // entry points -- the cheapest real MissingExport case available in-tree.
    Arcane::PluginResolveError error;
    auto plugin = Arcane::Plugin::Load(std::filesystem::path("Arcane.dll"), &error);

    CHECK_FALSE(plugin.has_value());
    CHECK(error.kind == Arcane::PluginResolveError::Kind::MissingExport);
    CHECK(error.symbol == std::string(Arcane::PluginEntry::kABIVersion));   // the first checked
}
