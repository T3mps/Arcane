// Console model: category derivation from the engine's "Subsystem: " prefixes,
// and identical-row collapsing. Pure functions, no ImGui ([editor]).

#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ConsoleModel.hpp>

namespace
{
    Arcane::Editor::ConsoleEntry Entry(Arcane::DiagSeverity level, std::string message)
    {
        Arcane::Editor::ConsoleEntry e;
        e.level    = level;
        e.message  = std::move(message);
        e.category = std::string(Arcane::Editor::CategoryForMessage(e.message));
        return e;
    }
}

TEST_CASE("CategoryForMessage maps the engine's subsystem prefixes", "[editor]")
{
    using Arcane::Editor::CategoryForMessage;
    CHECK(CategoryForMessage("AssetRegistry: duplicate id, keeping first") == "Assets");
    CHECK(CategoryForMessage("Assets: unresolved asset id 1234") == "Assets");
    CHECK(CategoryForMessage("plugin: initial load failed") == "Plugin");
    CHECK(CategoryForMessage("scene load: unknown component \"Foo\" skipped") == "Scene");
    CHECK(CategoryForMessage("SpriteMaterialCache: failed to compile") == "Material");
    CHECK(CategoryForMessage("PostChainCache: createShader failed") == "Material");
    CHECK(CategoryForMessage("LoadMaterialAsset: not a material") == "Material");
}

TEST_CASE("CategoryForMessage falls back to General for an unknown prefix", "[editor]")
{
    CHECK(Arcane::Editor::CategoryForMessage("Arcane Editor host, backend Vulkan") == "General");
    CHECK(Arcane::Editor::CategoryForMessage("") == "General");
}

TEST_CASE("CollapseConsole folds identical level+category+message into one row", "[editor]")
{
    const std::vector<Arcane::Editor::ConsoleEntry> entries = {
        Entry(Arcane::DiagSeverity::Warning, "Assets: unresolved asset id 1"),
        Entry(Arcane::DiagSeverity::Warning, "Assets: unresolved asset id 1"),
        Entry(Arcane::DiagSeverity::Warning, "Assets: unresolved asset id 1"),
        Entry(Arcane::DiagSeverity::Info,    "booted"),
    };

    const std::vector<Arcane::Editor::CollapsedRow> rows =
        Arcane::Editor::CollapseConsole(entries);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].count == 3);
    CHECK(rows[0].first->message == "Assets: unresolved asset id 1");
    CHECK(rows[1].count == 1);
    CHECK(rows[1].first->message == "booted");
}

TEST_CASE("CollapseConsole keeps first-seen order and does not merge across severity", "[editor]")
{
    const std::vector<Arcane::Editor::ConsoleEntry> entries = {
        Entry(Arcane::DiagSeverity::Warning, "same text"),
        Entry(Arcane::DiagSeverity::Error,   "same text"),
        Entry(Arcane::DiagSeverity::Warning, "same text"),
    };

    const std::vector<Arcane::Editor::CollapsedRow> rows =
        Arcane::Editor::CollapseConsole(entries);

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].first->level == Arcane::DiagSeverity::Warning);
    CHECK(rows[0].count == 2);      // both warnings fold, even though not adjacent
    CHECK(rows[1].first->level == Arcane::DiagSeverity::Error);
    CHECK(rows[1].count == 1);
}

TEST_CASE("CollapseConsole on an empty span yields no rows", "[editor]")
{
    CHECK(Arcane::Editor::CollapseConsole({}).empty());
}
