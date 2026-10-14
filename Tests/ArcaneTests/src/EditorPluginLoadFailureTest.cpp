// DescribePluginLoadFailure (T6-GATE, the T4-GATE c10 observation): a bare
// --plugin launch whose module fails to load must not claim a project opened.
#include <catch2/catch_test_macros.hpp>
#include <App/PluginLoadFailure.hpp>

TEST_CASE("DescribePluginLoadFailure names the project only when one is open", "[editor]")
{
    SECTION("a project is open: the manifest's DLLs failed")
    {
        const auto t = Arcane::Editor::DescribePluginLoadFailure(true, "Binaries/ReferenceGame.dll", 50);
        CHECK(t.title == "Open Project Failed");
        CHECK(t.body.find("The project opened") != std::string::npos);
        CHECK(t.body.find("ABI 50") != std::string::npos);
    }
    SECTION("no project is open: the --plugin module failed, and no project is claimed")
    {
        const auto t = Arcane::Editor::DescribePluginLoadFailure(false, "ReferenceProject/Binaries/ReferenceGame.dll", 50);
        CHECK(t.title == "Game Module Failed to Load");
        CHECK(t.body.find("project opened") == std::string::npos);
        CHECK(t.body.find("'ReferenceProject/Binaries/ReferenceGame.dll'") != std::string::npos);
        CHECK(t.body.find("No project is open") != std::string::npos);
        CHECK(t.body.find("ABI 50") != std::string::npos);
    }
}
