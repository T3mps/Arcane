// Settings arc S4 (spec s7.2): the shipped action table -- parseable, unique,
// conflict-free as shipped, and printing what the menus print today.
#include <catch2/catch_test_macros.hpp>
#include "Input/EditorActionTable.hpp"
#include "Input/EditorActions.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <set>
#include <string>

using namespace Arcane::Editor;

TEST_CASE("Action table: every entry is unique, named and parses to a bound chord", "[shortcuts]")
{
    std::set<std::string_view> ids;
    for (const EditorActionDesc& d : kEditorActionTable)
    {
        INFO(d.id);
        CHECK(ids.insert(d.id).second);
        CHECK_FALSE(d.displayName.empty());
        const auto c = ParseKeyChord(d.defaultChord);
        REQUIRE(c.has_value());
        CHECK(c->Bound());
        CHECK_FALSE(ReservedChordReason(*c).has_value());
    }
    CHECK(ids.size() == 50);
}

TEST_CASE("Action table: as shipped nothing is in conflict, and every same-context overlap is a designed one", "[shortcuts]")
{
    Arcane::CVarRegistry reg;
    EditorActions actions(reg);
    RegisterEditorActions(actions);
    for (std::string_view id : actions.Ids())
    {
        INFO(id);
        CHECK(actions.ConflictsOf(id).empty());
    }
    const std::set<std::pair<std::string_view, std::string_view>> designed = {
        { "editor.viewport.toolTranslate", "editor.camera.flyForward" },
        { "editor.viewport.toolRotate",    "editor.camera.flyUp" },
        { "editor.viewport.toolSelect",    "editor.camera.flyDown" },
    };
    for (const EditorActionDesc& a : kEditorActionTable)
        for (const EditorActionDesc& b : kEditorActionTable)
        {
            if (a.id >= b.id || a.context != b.context) continue;
            if (!SameKey(*ParseKeyChord(a.defaultChord), *ParseKeyChord(b.defaultChord))) continue;
            INFO(a.id << " vs " << b.id);
            CHECK((designed.contains({ a.id, b.id }) || designed.contains({ b.id, a.id })));
        }
}

TEST_CASE("Action table: menu text is today's menu text", "[shortcuts]")
{
    Arcane::CVarRegistry reg;
    EditorActions actions(reg);
    RegisterEditorActions(actions);
    CHECK(actions.MenuShortcut("edit.undo") == "Ctrl+Z");
    CHECK(actions.MenuShortcut("edit.redo") == "Ctrl+Y");
    CHECK(actions.MenuShortcut("file.newScene") == "Ctrl+N");
    CHECK(actions.MenuShortcut("file.openScene") == "Ctrl+O");
    CHECK(actions.MenuShortcut("file.saveScene") == "Ctrl+S");
    CHECK(actions.MenuShortcut("edit.cut") == "Ctrl+X");
    CHECK(actions.MenuShortcut("edit.copy") == "Ctrl+C");
    CHECK(actions.MenuShortcut("edit.paste") == "Ctrl+V");
    CHECK(actions.MenuShortcut("edit.duplicate") == "Ctrl+D");
    CHECK(actions.MenuShortcut("outliner.rename") == "F2");
    CHECK(actions.MenuShortcut("outliner.delete") == "Del");
    CHECK(actions.MenuShortcut("input.rebind") == "Enter");
    CHECK(actions.MenuShortcut("document.close") == "Ctrl+W");
}
