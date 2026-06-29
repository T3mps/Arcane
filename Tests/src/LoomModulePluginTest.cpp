#include <catch2/catch_test_macros.hpp>

#include <Module.hpp>
#include <Plugin.hpp>

#include <Arcane/Plugin/PluginABI.hpp>

#include <filesystem>

TEST_CASE("Loom Module loads a dynamic library and resolves symbols", "[loom][module]")
{
    auto module = Module::Load(std::filesystem::path("HotReloadPluginV1.dll"));

    REQUIRE(module.has_value());
    CHECK(module->IsLoaded());
    CHECK(module->Path().filename() == "HotReloadPluginV1.dll");
    CHECK(module->Symbol(Arcane::PluginEntry::kABIVersion) != nullptr);
    CHECK(module->Symbol("Definitely_Not_An_Exported_Symbol") == nullptr);
}

TEST_CASE("Loom Plugin resolves the current game plugin ABI", "[loom][plugin]")
{
    auto plugin = Plugin::Load(std::filesystem::path("HotReloadPluginV1.dll"));

    REQUIRE(plugin.has_value());
    CHECK(plugin->IsLoaded());
    REQUIRE(plugin->VTable().ABIVersion != nullptr);
    CHECK(plugin->VTable().ABIVersion() == Arcane::kGamePluginABIVersion);
    CHECK(plugin->VTable().Init != nullptr);
    CHECK(plugin->VTable().Shutdown != nullptr);
    CHECK(plugin->VTable().FixedUpdate != nullptr);
    CHECK(plugin->VTable().Update != nullptr);
    CHECK(plugin->VTable().SaveState != nullptr);
    CHECK(plugin->VTable().LoadState != nullptr);
}

TEST_CASE("Loom Plugin rejects modules that do not satisfy the game plugin ABI", "[loom][plugin]")
{
    auto plugin = Plugin::Load(std::filesystem::path("HotReloadPluginBad.dll"));

    CHECK_FALSE(plugin.has_value());
}
