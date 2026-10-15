// The game contract (engine modularization s3.1/s4, P4): the two copies of the
// default game-module set agree, and a static game's Plugin (the monolithic
// layout's PluginHost path, otherwise compiled only under ARC_MONOLITHIC) holds
// its table with no image.

#include "Helpers/ConstantScan.hpp"   // RepoRoot()

#include <Arcane/Plugin/Plugin.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Project/GameDescriptor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

TEST_CASE("kDefaultGameModules equals build/modules.lua's ARC_GAME_DEFAULT_MODULES", "[plugin][project]")
{
    std::ifstream in(Arcane::Test::RepoRoot() / "build" / "modules.lua", std::ios::binary);
    REQUIRE(in.good());
    std::stringstream text;
    text << in.rdbuf();

    std::smatch line;
    const std::string source = text.str();
    REQUIRE(std::regex_search(source, line, std::regex(R"(ARC_GAME_DEFAULT_MODULES\s*=\s*\{([^}]*)\})")));

    std::vector<std::string> lua;
    const std::string list = line[1].str();
    const std::regex quoted(R"x("([^"]+)")x");
    for (auto it = std::sregex_iterator(list.begin(), list.end(), quoted); it != std::sregex_iterator(); ++it)
        lua.push_back((*it)[1].str());

    std::vector<std::string> cpp(Arcane::kDefaultGameModules.begin(), Arcane::kDefaultGameModules.end());
    CHECK(lua == cpp);
}

namespace
{
    std::uint32_t StaticAbi() { return Arcane::kGamePluginABIVersion; }
}

TEST_CASE("Plugin::FromStatic holds the game's table with no image", "[plugin]")
{
    static const Arcane::PluginVTable table{ &StaticAbi };
    const Arcane::Plugin plugin = Arcane::Plugin::FromStatic(table, "StaticGameUnderTest");

    CHECK(plugin.IsStatic());
    CHECK(plugin.IsLoaded());
    CHECK_FALSE(plugin.LoadedModule().IsLoaded());
    CHECK(plugin.LoadedModule().Image().base == nullptr);
    // The system-factory owner key: the table's address, never a null base.
    CHECK(plugin.FactoryOwner() == static_cast<const void*>(&table));
    CHECK(plugin.CVarModule() == "StaticGameUnderTest");
    REQUIRE(plugin.VTable().ABIVersion != nullptr);
    CHECK(plugin.VTable().ABIVersion() == Arcane::kGamePluginABIVersion);
}
