// Settings arc S4 (spec s7.2): menus print the bound chord, so a rebinding shows.
#include <catch2/catch_test_macros.hpp>
#include "Input/EditorActions.hpp"
#include "Input/MenuShortcut.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <string>

TEST_CASE("MenuKey prints the current chord and follows a rebinding", "[shortcuts]")
{
    Arcane::Editor::EditorActions& keys = Arcane::Editor::EditorActions::Get();
    CHECK(std::string(Arcane::Editor::MenuKey("edit.undo").c_str()) == "Ctrl+Z");
    CHECK(std::string(Arcane::Editor::MenuKey("no.such.action").c_str()).empty());
    Arcane::CVarRegistry& reg = keys.Registry();
    reg.Set(keys.HandleOf("edit.undo"), Arcane::CVarValue::String("Alt+Backspace"), Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor);
    reg.PublishImmediate();
    keys.RefreshBindings();
    CHECK(std::string(Arcane::Editor::MenuKey("edit.undo").c_str()) == "Alt+Backspace");
    reg.RevertLayer(Arcane::SetBy::EditorUser);
    reg.PublishImmediate();
    keys.RefreshBindings();
}
