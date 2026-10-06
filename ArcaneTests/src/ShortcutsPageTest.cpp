// Settings arc S4 (spec s7.2): the shortcuts page -- rows, search, listen/
// cancel/clear, reserved refusal, red conflicts on every row, type flip, reset.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/TestEnvironment.hpp"
#include "Input/EditorActionTable.hpp"
#include "Input/EditorActions.hpp"
#include "Settings/SettingsApply.hpp"
#include "Settings/SettingsModel.hpp"
#include "Settings/ShortcutsPage.hpp"
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Input/KeyLayout.hpp>
#include <Json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>

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

TEST_CASE("Shortcuts page: Reset All survives a restart -- the archive takes the saved binding out of editor.json", "[shortcuts][settings-ui]")
{
    Arcane::Test::TempDir dir("shortcuts-reset-all");
    const std::filesystem::path file = dir.path / "editor.json";
    const auto boot = [&](Rig& r)   // Runtime: the EditorUser layer from <EditorUserDir>/Config
    {
        (void)Arcane::ApplyCVarDirectory(r.reg, dir.path, Arcane::SetBy::EditorUser, "editor-user");
        r.reg.Publish();
        r.actions.RefreshBindings();
    };
    const auto exitWrite = [&](Rig& r) { Arcane::WriteCVarArchive(r.reg, dir.path, Arcane::SetBy::EditorUser); };

    {   // Session 1: edit.copy -> F (conflicting with Frame Selection); the exit-time archive saves it.
        Rig r;
        REQUIRE(SetActionChord(r.actions, "edit.copy", *ParseKeyChord("F")));
        exitWrite(r);
        REQUIRE(std::filesystem::exists(file));
    }
    {   // Session 2: the binding came back from the file; Reset All, the debounced archive, then exit.
        Rig r;
        boot(r);
        REQUIRE(FormatKeyChord(*r.actions.ChordOf("edit.copy")) == "F");
        REQUIRE(r.Row(BuildShortcutRows(r.actions, ""), "edit.copy").conflict);
        SettingsArchiveQueue archive;
        ResetAllShortcuts(r.actions, [&](const std::string& cvar) { archive.MarkDirty(Arcane::SetBy::EditorUser, cvar, 0.0); });
        CHECK(archive.Dirty());
        CHECK(archive.Flush([&](Arcane::SetBy rung, const std::vector<std::string>& names)
        {
            return Arcane::WriteCVarRungArchive(r.reg, rung, dir.path, names);
        }));
        exitWrite(r);
    }
    if (std::filesystem::exists(file))
    {
        std::ifstream in(file, std::ios::binary);
        const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        INFO(text);
        const auto doc = nlohmann::json::parse(text, nullptr, false);
        REQUIRE_FALSE(doc.is_discarded());
        CHECK_FALSE(doc.contains("keys.edit.copy"));
        CHECK_FALSE(doc.contains("keys"));
    }
    {   // Session 3: the defaults, and no conflict.
        Rig r;
        boot(r);
        CHECK(FormatKeyChord(*r.actions.ChordOf("edit.copy")) == "Ctrl+C");
        CHECK_FALSE(r.reg.RungValue("editor.keys.edit.copy", Arcane::SetBy::EditorUser).has_value());
        for (const ShortcutRow& row : BuildShortcutRows(r.actions, "")) { INFO(row.id); CHECK_FALSE(row.conflict); CHECK(row.isDefault); }
    }
}

TEST_CASE("Shortcuts page: every page write reports its cvar to the archive sink", "[shortcuts][settings-ui]")
{
    Rig r;
    std::vector<std::string> heard;
    const ShortcutWriteSink sink = [&](const std::string& cvar) { heard.push_back(cvar); };
    REQUIRE(SetActionChord(r.actions, "edit.undo", *ParseKeyChord("Ctrl+K"), sink));
    REQUIRE(FlipKeyType(r.actions, "editor.camera.flyForward", sink));
    CHECK((heard == std::vector<std::string>{ "editor.keys.edit.undo", "editor.keys.editor.camera.flyForward" }));
    heard.clear();
    r.page.listeningId = "edit.redo";
    r.actions.SetListening(true);
    r.Frame({});
    r.Frame({ Keys::kScanBackspace });
    CHECK(FeedListen(r.actions, r.page, sink) == ListenOutcome::Cleared);
    CHECK((heard == std::vector<std::string>{ "editor.keys.edit.redo" }));
    heard.clear();
    ResetAllShortcuts(r.actions, sink);
    CHECK(heard.size() == kEditorActionTable.size());
}
