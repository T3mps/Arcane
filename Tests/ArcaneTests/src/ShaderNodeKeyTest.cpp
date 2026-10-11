// Node page s5.1.1: the shader document's node selection key
// "node:<pass>:<id>" -- canonical unsigned decimals only, so each node has
// exactly ONE history key -- and the canvas-read rule. Pure, no ImGui.
#include <catch2/catch_test_macros.hpp>

#include "Documents/ShaderNodeKey.hpp"

using namespace Arcane::Editor;

TEST_CASE("ShaderNodeKey: Parse and Format round-trip canonical keys", "[editor][nodepage]")
{
    for (const NodeKey k : { NodeKey{ 0, 7 }, NodeKey{ 2, 1 }, NodeKey{ 10, 4294967295u } })
    {
        const std::string text = FormatNodeKey(k);
        INFO(text);
        const std::optional<NodeKey> back = ParseNodeKey(text);
        REQUIRE(back.has_value());
        CHECK(*back == k);
    }
    CHECK(FormatNodeKey({ 0, 7 }) == "node:0:7");
    CHECK(FormatNodeKey({ 3, 12 }) == "node:3:12");
}

TEST_CASE("ShaderNodeKey: every non-canonical spelling is refused", "[editor][nodepage]")
{
    for (const char* bad : { "node:01:7", "node:0:07", "node:0:0", "node:7", "node: 0:7", "node:0:7x",
                             "node:0:7 ", "node:-1:7", "node:+0:7", "node::7", "node:0:", "node:0:7:1",
                             "Node:0:7", "material", "", "node:0:4294967296" })
    {
        INFO(bad);
        CHECK_FALSE(ParseNodeKey(bad).has_value());
    }
}

TEST_CASE("ShaderNodeKey: --select-in-document paths are <id> (active pass) or <pass>/<id>", "[editor][nodepage]")
{
    CHECK(ParseNodeSelectPath("7", 0) == std::optional<NodeKey>{ NodeKey{ 0, 7 } });
    CHECK(ParseNodeSelectPath("7", 3) == std::optional<NodeKey>{ NodeKey{ 3, 7 } });
    CHECK(ParseNodeSelectPath("2/7", 0) == std::optional<NodeKey>{ NodeKey{ 2, 7 } });
    for (const char* bad : { "", "0", "07", "2/", "/7", "a/7", "2/7/1", "2/0", "Player/Jump" })
    {
        INFO(bad);
        CHECK_FALSE(ParseNodeSelectPath(bad, 0).has_value());
    }
}

TEST_CASE("ShaderNodeKey: the canvas read mirrors exactly one selected node, and only unapplied changes are events",
          "[editor][nodepage]")
{
    CHECK(ReadCanvasSelection(1, 9, 2, true, false).sel == std::optional<NodeKey>{ NodeKey{ 2, 9 } });
    CHECK(ReadCanvasSelection(1, 9, 2, true, false).event);            // a click / the Problems locator
    CHECK_FALSE(ReadCanvasSelection(1, 9, 2, true, true).event);      // our own restore: never an event
    CHECK_FALSE(ReadCanvasSelection(1, 9, 2, false, false).event);    // nothing changed
    CHECK_FALSE(ReadCanvasSelection(0, 0, 0, true, false).sel.has_value());   // background clear...
    CHECK(ReadCanvasSelection(0, 0, 0, true, false).event);                   // ...is still an event
    CHECK_FALSE(ReadCanvasSelection(2, 9, 0, true, false).sel.has_value());   // 2 = "two or more": material page
}
