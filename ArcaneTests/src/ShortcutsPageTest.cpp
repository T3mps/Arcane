// Settings arc S4 (spec s7.2): the shortcuts page -- rows, search, listen/
// cancel/clear, reserved refusal, red conflicts on every row, type flip, reset.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/TestEnvironment.hpp"
#include "Helpers/TestTypeContext.hpp"
#include "Input/EditorActionTable.hpp"
#include "Input/EditorActions.hpp"
#include "Settings/SettingsApply.hpp"
#include "Settings/SettingsModel.hpp"
#include "Settings/ShortcutsPage.hpp"
#include <Arcane/Base/Runtime.hpp>
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

TEST_CASE("Shortcuts page: Conflicts only keeps exactly the red rows, Graph context included", "[shortcuts][settings-ui]")
{
    // S4-GATE: the conflict capture's Graph row sat below the 1080p fold; the
    // filter (and editor.settings.keysConflictsOnly for automation) shows it.
    Rig r;
    CHECK(BuildShortcutRows(r.actions, "", true).empty());
    REQUIRE(SetActionChord(r.actions, "edit.copy", *ParseKeyChord("F")));
    std::vector<std::string> ids;
    for (const ShortcutRow& row : BuildShortcutRows(r.actions, "", true)) { CHECK(row.conflict); ids.push_back(row.id); }
    CHECK((ids == std::vector<std::string>{ "edit.copy", "editor.view.frameSelected", "graph.frameSelected" }));
    CHECK(BuildShortcutRows(r.actions, "graph", true).size() == 1);   // the search still narrows
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
        ResetAllShortcuts(r.actions, [&](Arcane::SetBy rung, const std::string& cvar) { archive.MarkDirty(rung, cvar, 0.0); });
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
    const ShortcutWriteSink sink = [&](Arcane::SetBy rung, const std::string& cvar)
    {
        CHECK(rung == Arcane::SetBy::EditorUser);   // no User records here: every write is the page's own rung
        heard.push_back(cvar);
    };
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

TEST_CASE("Shortcuts page: a User-rung binding is marked 'Overridden by', is clearable from the row, and Reset All clears it too", "[shortcuts][settings-ui]")
{
    // S4-GATE: a per-project User value of editor.keys.<id> (a hand edit of the
    // project's user file) silently shadowed the page's EditorUser writes: the
    // page drew no marker and Reset All cleared EditorUser only.
    Rig r;
    std::vector<std::pair<Arcane::SetBy, std::string>> heard;
    const ShortcutWriteSink sink = [&](Arcane::SetBy rung, const std::string& cvar) { heard.emplace_back(rung, cvar); };
    const auto userOverride = [&](std::string_view id, const char* chord)
    {
        REQUIRE(r.reg.Set(r.actions.HandleOf(id), Arcane::CVarValue::String(chord), Arcane::SetBy::User, "user",
                          Arcane::CVarContext::Editor) == Arcane::SetResult::Applied);
        r.reg.Publish();
        r.actions.RefreshBindings();
    };
    userOverride("edit.undo", "Ctrl+J");
    {
        const ShortcutRow row = r.Row(BuildShortcutRows(r.actions, ""), "edit.undo");
        CHECK(row.overriddenBy == Arcane::SetBy::User);
        CHECK(row.chord == "Ctrl+J");
        CHECK(r.Row(BuildShortcutRows(r.actions, ""), "edit.redo").overriddenBy == Arcane::SetBy::Default);
    }

    SECTION("binding over it is saved but refused with the reason, never reported as Cancelled")
    {
        CHECK(r.Listen("edit.undo", { Keys::kScanLCtrl, Keys::ScanLetter('K') }) == ListenOutcome::Refused);
        CHECK(r.page.refusal == "Ctrl+K is saved for all projects, but the User (this project) binding still wins: use Clear override on the row.");
        CHECK(r.reg.RungValue("editor.keys.edit.undo", Arcane::SetBy::EditorUser) == std::optional<Arcane::CVarValue>(Arcane::CVarValue::String("Ctrl+K")));
        CHECK(FormatKeyChord(*r.actions.ChordOf("edit.undo")) == "Ctrl+J");
        CHECK(r.Listen("edit.undo", { Keys::kScanBackspace }) == ListenOutcome::Refused);
        CHECK(r.page.refusal.starts_with("The clear is saved for all projects, but the User (this project) binding still wins"));
    }
    SECTION("Clear override removes the User value; the row shows what is underneath")
    {
        REQUIRE(SetActionChord(r.actions, "edit.undo", *ParseKeyChord("Ctrl+K")) == false);   // recorded beneath User
        heard.clear();
        CHECK(ClearShortcutOverride(r.actions, "edit.undo", sink));
        CHECK((heard == std::vector<std::pair<Arcane::SetBy, std::string>>{ { Arcane::SetBy::User, "editor.keys.edit.undo" } }));
        const ShortcutRow row = r.Row(BuildShortcutRows(r.actions, ""), "edit.undo");
        CHECK(row.overriddenBy == Arcane::SetBy::Default);
        CHECK(row.chord == "Ctrl+K");
        CHECK_FALSE(ClearShortcutOverride(r.actions, "edit.undo", sink));   // nothing above EditorUser any more
    }
    SECTION("a --set binding is marked too, and Clear override removes it")
    {
        REQUIRE(r.reg.Set(r.actions.HandleOf("edit.redo"), Arcane::CVarValue::String("Ctrl+L"), Arcane::SetBy::CommandLine, "",
                          Arcane::CVarContext::Editor) == Arcane::SetResult::Applied);
        r.reg.Publish();
        r.actions.RefreshBindings();
        CHECK(r.Row(BuildShortcutRows(r.actions, ""), "edit.redo").overriddenBy == Arcane::SetBy::CommandLine);
        CHECK(ClearShortcutOverride(r.actions, "edit.redo", sink));
        CHECK(FormatKeyChord(*r.actions.ChordOf("edit.redo")) == "Ctrl+Y");
    }
    SECTION("Reset All clears the User rung as well as EditorUser, and tells the archive both")
    {
        heard.clear();
        ResetAllShortcuts(r.actions, sink);
        CHECK_FALSE(r.reg.RungValue("editor.keys.edit.undo", Arcane::SetBy::User).has_value());
        CHECK(FormatKeyChord(*r.actions.ChordOf("edit.undo")) == "Ctrl+Z");
        CHECK(std::count(heard.begin(), heard.end(), std::pair<Arcane::SetBy, std::string>{ Arcane::SetBy::User, "editor.keys.edit.undo" }) == 1);
        CHECK(std::count_if(heard.begin(), heard.end(), [](const auto& h) { return h.first == Arcane::SetBy::EditorUser; })
              == static_cast<std::ptrdiff_t>(kEditorActionTable.size()));
        for (const ShortcutRow& row : BuildShortcutRows(r.actions, "")) { INFO(row.id); CHECK(row.overriddenBy == Arcane::SetBy::Default); }
    }
}

TEST_CASE("Shortcuts page: a write the registry refuses is Refused with its reason, not Cancelled", "[shortcuts][settings-ui]")
{
    // S4-GATE: FeedListen reported every failed WriteChord (bind or Backspace
    // clear) as ListenOutcome::Cancelled with no text.
    Rig r;
    r.reg.UnregisterModule(r.reg.Describe("editor.keys.edit.copy")->module);   // the action's cvar is gone: its handle is stale
    r.actions.RefreshBindings();
    CHECK(r.Listen("edit.copy", { Keys::kScanLCtrl, Keys::ScanLetter('K') }) == ListenOutcome::Refused);
    CHECK(r.page.refusal == "Ctrl+K was not saved: the action's setting is no longer registered.");
    CHECK(r.Listen("edit.copy", { Keys::kScanBackspace }) == ListenOutcome::Refused);
    CHECK(r.page.refusal == "The clear was not saved: the action's setting is no longer registered.");
}

TEST_CASE("Shortcuts: the boot declares editor.keys.* before any config rung -- a saved shortcut and a --set editor.keys.* are live at boot", "[shortcuts][settings-ui][settings]")
{
    // S4-20 fix: the registry layers a rung onto cvars that already exist, never
    // onto a later Register, and EditorActions::Get() declares its table lazily.
    // A first Get() in the frame loop (after the rungs) dropped the Keyboard
    // page's saved shortcuts on a cold boot and refused --set editor.keys.* as
    // unknown. ArcaneEditor's main() now calls Get() before HostBoot's early
    // rungs (S4-GATE; main.cpp is not in this binary; this pins both orders,
    // and EditorWitnessTest E12 pins the real process).
    Arcane::Test::TempDir dir("shortcuts-boot-order");
    {
        std::ofstream out(dir.path / "editor.json", std::ios::binary);
        out << R"({ "keys": { "edit.copy": "F" } })";
    }
    const std::vector<std::string> sets{ "editor.keys.edit.cut=Ctrl+Shift+X" };

    SECTION("rungs applied BEFORE the table is declared never reach it -- the old boot order")
    {
        Arcane::CVarRegistry reg;
        (void)Arcane::ApplyCVarDirectory(reg, dir.path, Arcane::SetBy::EditorUser, "editor-user");
        Arcane::ApplyCVarCommandLine(reg, sets, Arcane::CVarContext::Editor);   // warns: unknown
        EditorActions actions(reg);
        RegisterEditorActions(actions);
        reg.Publish();
        actions.RefreshBindings();
        CHECK(FormatKeyChord(*actions.ChordOf("edit.copy")) == "Ctrl+C");
        CHECK(FormatKeyChord(*actions.ChordOf("edit.cut")) == "Ctrl+X");
    }

    SECTION("the editor boot's order: Get(), then the Runtime, the EditorUser rung and the --set list")
    {
        EditorActions& keys = EditorActions::Get();   // main(), before ApplyEarlyConfigRungs
        Arcane::CVarRegistry& cvars = Arcane::CVarRegistry::Get();
        struct Cleanup
        {
            EditorActions& keys;
            ~Cleanup()
            {
                Arcane::CVarRegistry& r = Arcane::CVarRegistry::Get();
                r.RevertLayer(Arcane::SetBy::EditorUser);
                (void)r.ClearRung(r.Find("editor.keys.edit.cut"), Arcane::SetBy::CommandLine);
                r.Publish();
                keys.RefreshBindings();
            }
        } cleanup{ keys };

        Arcane::Runtime rt(Arcane::Test::Process());   // applies the engine rung
        rt.SetEditorUserConfigDir(dir.path);            // StageRuntimeCreate: the EditorUser rung
        Arcane::ApplyCVarCommandLine(cvars, sets, Arcane::CVarContext::Editor);   // input_config: --set
        cvars.Publish();
        keys.RefreshBindings();

        CHECK(FormatKeyChord(*keys.ChordOf("edit.copy")) == "F");
        CHECK(cvars.Explain("editor.keys.edit.copy")->setBy == Arcane::SetBy::EditorUser);
        CHECK(FormatKeyChord(*keys.ChordOf("edit.cut")) == "Ctrl+Shift+X");
        CHECK(cvars.Explain("editor.keys.edit.cut")->setBy == Arcane::SetBy::CommandLine);

        rt.SetEditorUserConfigDir({});   // archiving is off: nothing is written back
        keys.RefreshBindings();
        CHECK(FormatKeyChord(*keys.ChordOf("edit.copy")) == "Ctrl+C");
    }
}
