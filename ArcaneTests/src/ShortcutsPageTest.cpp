// Settings arc S4 (spec s7.2): the shortcuts page -- rows, search, listen/
// cancel/clear, reserved refusal, red conflicts on every row, type flip, reset.
#include <catch2/catch_test_macros.hpp>
#include "Input/EditorActionTable.hpp"
#include "Input/EditorActions.hpp"
#include "Settings/SettingsModel.hpp"
#include "Settings/ShortcutsPage.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Input/KeyLayout.hpp>
#include <algorithm>
#include <initializer_list>

using namespace Arcane::Editor;
namespace Keys = Arcane::Keys;

namespace
{
    struct Rig
    {
        Arcane::CVarRegistry reg;
        EditorActions actions{ reg };
        ShortcutsPageState page;
        Rig() { RegisterEditorActions(actions); }
        void Frame(std::initializer_list<std::uint32_t> scancodes)
        {
            Arcane::InputSnapshot s;
            for (std::uint32_t sc : scancodes)
            {
                s.SetScancode(sc);
                if (const std::int32_t kc = Arcane::QwertyKeyLayout().KeycodeFor(sc)) s.AddKeycode(static_cast<std::uint32_t>(kc));
            }
            ActionFrameInput in; in.snap = &s; in.dt = 1.0 / 60.0;
            actions.BeginFrame(in);
        }
        const ShortcutRow& Row(const std::vector<ShortcutRow>& rows, std::string_view id)
        {
            const auto it = std::find_if(rows.begin(), rows.end(), [&](const ShortcutRow& r) { return r.id == id; });
            REQUIRE(it != rows.end());
            return *it;
        }
        ListenOutcome Listen(std::string_view id, std::initializer_list<std::uint32_t> keys)
        {
            page.listeningId = std::string(id);
            actions.SetListening(true);
            Frame({});
            Frame(keys);
            return FeedListen(actions, page);
        }
    };
}

TEST_CASE("Shortcuts page: one row per action, searchable by name, context and chord", "[shortcuts][settings-ui]")
{
    Rig r;
    CHECK(BuildShortcutRows(r.actions, "").size() == kEditorActionTable.size());
    const auto undo = BuildShortcutRows(r.actions, "undo");
    REQUIRE(undo.size() == 1);
    CHECK(undo[0].chord == "Ctrl+Z");
    CHECK(undo[0].context == "Global");
    CHECK(BuildShortcutRows(r.actions, "ctrl+w").size() == 1);
    CHECK(BuildShortcutRows(r.actions, "input actions").size() == 7);
    for (const ShortcutRow& row : BuildShortcutRows(r.actions, "")) { INFO(row.id); CHECK_FALSE(row.conflict); }
}

TEST_CASE("Shortcuts page: listen binds, Esc cancels, Backspace clears, a reserved chord is refused", "[shortcuts][settings-ui]")
{
    Rig r;
    CHECK(r.Listen("edit.undo", { Keys::kScanEscape }) == ListenOutcome::Cancelled);
    CHECK(FormatKeyChord(*r.actions.ChordOf("edit.undo")) == "Ctrl+Z");
    CHECK(r.Listen("edit.undo", { Keys::kScanLAlt, Keys::kScanF4 }) == ListenOutcome::Refused);
    CHECK_FALSE(r.page.refusal.empty());
    CHECK(FormatKeyChord(*r.actions.ChordOf("edit.undo")) == "Ctrl+Z");
    CHECK(r.Listen("edit.undo", { Keys::kScanLCtrl, Keys::ScanLetter('K') }) == ListenOutcome::Bound);
    CHECK(FormatKeyChord(*r.actions.ChordOf("edit.undo")) == "Ctrl+K");
    CHECK(r.page.listeningId.empty());
    CHECK_FALSE(r.actions.Listening());
    CHECK(r.Listen("edit.undo", { Keys::kScanBackspace }) == ListenOutcome::Cleared);
    CHECK_FALSE(r.actions.ChordOf("edit.undo")->Bound());
    CHECK(r.Listen("editor.camera.flyForward", { Keys::ScanLetter('I') }) == ListenOutcome::Bound);
    CHECK(FormatKeyChord(*r.actions.ChordOf("editor.camera.flyForward")) == "[KeyI]");   // the row was Physical
}

TEST_CASE("Shortcuts page: a conflict turns EVERY involved row red, names the others and who fires; Reset All clears it", "[shortcuts][settings-ui]")
{
    Rig r;
    REQUIRE(SetActionChord(r.actions, "edit.copy", *ParseKeyChord("F")));
    const auto rows = BuildShortcutRows(r.actions, "");
    const ShortcutRow& copy = r.Row(rows, "edit.copy");
    CHECK(copy.conflict);
    CHECK(r.Row(rows, "editor.view.frameSelected").conflict);
    CHECK(r.Row(rows, "graph.frameSelected").conflict);
    CHECK(copy.tooltip.find("Frame Selection (Global)") != std::string::npos);
    CHECK(copy.tooltip.find("Frame Selection (graph) (Graph)") != std::string::npos);
    CHECK(copy.tooltip.find("fires") != std::string::npos);
    CHECK_FALSE(r.Row(rows, "edit.paste").conflict);
    ResetAllShortcuts(r.actions);
    for (const ShortcutRow& row : BuildShortcutRows(r.actions, "")) { INFO(row.id); CHECK_FALSE(row.conflict); CHECK(row.isDefault); }
    CHECK_FALSE(r.reg.RungValue("editor.keys.edit.copy", Arcane::SetBy::EditorUser).has_value());   // dropped, not pinned
}

TEST_CASE("Shortcuts page: the Type column flips Labelled <-> Physical", "[shortcuts][settings-ui]")
{
    Rig r;
    REQUIRE(FlipKeyType(r.actions, "editor.camera.flyForward"));
    CHECK(FormatKeyChord(*r.actions.ChordOf("editor.camera.flyForward")) == "W");
    REQUIRE(FlipKeyType(r.actions, "editor.camera.flyForward"));
    CHECK(FormatKeyChord(*r.actions.ChordOf("editor.camera.flyForward")) == "[KeyW]");
}

TEST_CASE("Shortcuts page: listening ends when the page is not drawn, so a closed window never silences the editor", "[shortcuts][settings-ui]")
{
    Rig r;
    r.page.listeningId = "edit.undo";
    r.actions.SetListening(true);
    r.page.drawnFrame = 41;
    EndListenIfPageHidden(r.actions, r.page, 41);   // drawn this frame: still listening
    CHECK(r.actions.Listening());
    CHECK(r.page.listeningId == "edit.undo");
    EndListenIfPageHidden(r.actions, r.page, 42);   // closed, or another page selected
    CHECK_FALSE(r.actions.Listening());
    CHECK(r.page.listeningId.empty());
    CHECK(FormatKeyChord(*r.actions.ChordOf("edit.undo")) == "Ctrl+Z");
}

TEST_CASE("Shortcuts page: Preferences > Keyboard holds the page and the editor.keys cvars", "[shortcuts][settings-ui]")
{
    Rig r;
    const auto undo = r.reg.Describe("editor.keys.edit.undo");
    REQUIRE(undo.has_value());
    CHECK(undo->categoryPath == "Keyboard");

    SettingsModel m;
    m.SetPages({ SettingsPageRef{ Arcane::SettingScope::PreferencesMachine, "Keyboard", "Keyboard Shortcuts" } });
    m.Rebuild(r.reg, Arcane::SettingScope::PreferencesMachine);
    const SettingsTreeNode* keyboard = m.Find("Keyboard");
    REQUIRE(keyboard != nullptr);
    CHECK(std::find(keyboard->cvars.begin(), keyboard->cvars.end(), "editor.keys.edit.undo") != keyboard->cvars.end());
    CHECK(m.Find("Editor/Keys") == nullptr);
}
