#include <catch2/catch_test_macros.hpp>
#include "Helpers/ConstantScan.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
using namespace Arcane;
using namespace Arcane::Test;
TEST_CASE("sweep coverage: every frozen SETTING name is a registered cvar", "[sweep][coverage]")
{
    std::ifstream in(RepoRoot() / "scripts" / "settings-frozen-names.txt");
    REQUIRE(in.good());
    std::string name, missing;
    std::size_t n = 0;
    while (std::getline(in, name))
    {
        if (name.empty() || name[0] == '#') continue;
        ++n;
        if (CVarRegistry::Get().Find(name).IsStale()) missing += "  " + name + "\n";
    }
    INFO(n << " frozen names; not registered in the test process:\n" << missing);
    CHECK(missing.empty());
}
TEST_CASE("sweep coverage: the allow-list holds no PENDING or UNLISTED entry", "[sweep][coverage]")
{
    std::ifstream in(RepoRoot() / "scripts" / "constant-allowlist.txt");
    std::string line, left;
    while (std::getline(in, line))
        if (line.find("|PENDING|") != std::string::npos || line.find("|UNLISTED|") != std::string::npos) left += "  " + line + "\n";
    INFO(left);
    CHECK(left.empty());
}
// The other direction (controller ruling, S6-45): a cvar the engine or editor
// registers is either a frozen SETTING name or a row of the inventory's
// "Existing cvars (e)" table (the pre-arc cvars the sweeps kept). A name a
// sweep invents without an inventory row fails here. Two prefixes are not
// names but families: "tests." (the suite's own fixtures) and "editor.keys."
// (one binding per editor action, registered by EditorActions::Register from
// the action table -- spec s7.2; the inventory's Shortcuts section names the
// family, the action table its members).
TEST_CASE("sweep coverage: every registered cvar is a frozen SETTING name or an inventoried existing cvar", "[sweep][coverage]")
{
    std::set<std::string, std::less<>> listed;
    {
        std::ifstream in(RepoRoot() / "scripts" / "settings-frozen-names.txt");
        REQUIRE(in.good());
        for (std::string line; std::getline(in, line);)
            if (!line.empty() && line[0] != '#') listed.insert(line);
    }
    std::size_t existing = 0;
    {
        std::ifstream in(RepoRoot() / "docs" / "superpowers" / "audits" / "2026-10-03-settings-inventory.md");
        REQUIRE(in.good());
        bool inTable = false;
        for (std::string line; std::getline(in, line);)
        {
            if (line.starts_with("### ")) { inTable = line == "### Existing cvars (e)"; continue; }
            if (!inTable || !line.starts_with("| ")) continue;
            const std::string_view row(line);
            const std::size_t end = row.find(" |", 2);
            const std::string_view name = row.substr(2, end == std::string_view::npos ? 0 : end - 2);
            if (name.empty() || name == "name" || name.starts_with("---")) continue;
            listed.insert(std::string(name));
            ++existing;
        }
    }
    REQUIRE(existing > 0);   // the table was found

    std::string unlisted;
    for (const std::string& name : CVarRegistry::Get().Names(true))
        if (!name.starts_with("tests.") && !name.starts_with("editor.keys.") && !listed.contains(name)) unlisted += "  " + name + "\n";
    INFO("registered in the test process but neither frozen nor an inventoried existing cvar:\n" << unlisted);
    CHECK(unlisted.empty());
}
