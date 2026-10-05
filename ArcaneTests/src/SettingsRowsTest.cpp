// Settings arc S3-8: one settings row through the REAL ImGui path -- the
// widget by type/hint, a click edit as one undo step, a drag applied live
// and recorded as ONE step, Escape restoring the rung exactly, the reset
// arrow, a text row committing on Enter.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Settings/SettingsApply.hpp>
#include <Settings/SettingsRows.hpp>
#include <Panels/AssetPanelModel.hpp>
#include <Astra/Registry/Registry.hpp>
#include <imgui.h>

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Arcane;
using namespace Arcane::Editor;
using Arcane::Test::AddSetting;

namespace
{
    struct RowHarness
    {
        Arcane::Test::SettingsImGuiHarness imgui{ ImVec2(1280.0f, 1024.0f) };
        CVarRegistry reg;
        PropertyGridState grid;
        Arcane::CommandStack undo{ []() -> Astra::Registry& { throw std::logic_error("settings undo never touches the scene registry"); } };
        EditGesture::GestureState gesture;
        SettingsGestureMemo memo;
        std::unordered_map<std::string, std::string> drafts;
        SettingsArchiveQueue archive;
        SettingsWindowKind window = SettingsWindowKind::Project;
        bool projectOpen = true;
        std::vector<std::string> names;
        std::vector<SettingRowResult> results;
        std::string lastTooltip, lastContextMenu;

        RowHarness() { grid.probe = &imgui.probe; }

        void Frame()
        {
            imgui.Frame([&]
            {
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
                ImGui::SetNextWindowSize(ImVec2(900.0f, 900.0f));
                ImGui::Begin("Rows");
                PropertyGrid pg(grid);
                const SettingsEditSink sink = ArchiveSink(&archive, [] { return 0.0; });
                SettingsRowContext ctx{ .registry = reg, .window = window, .grid = pg, .undo = undo, .gesture = gesture,
                                        .memo = memo, .sink = sink, .textDrafts = drafts, .projectOpen = projectOpen };
                results.clear();
                {
                    PropertyGrid::Rows rows(pg, "##rows");
                    if (rows)
                        for (const std::string& n : names) results.push_back(DrawSettingRow(ctx, n));
                }
                lastTooltip = ctx.lastTooltip;
                lastContextMenu = ctx.lastContextMenu;
                ImGui::End();
            });
        }
        ImVec2 At(const std::string& key) { INFO(key); REQUIRE(imgui.probe.count(key) == 1); return imgui.probe.at(key); }
        void Click(const std::string& key) { const ImVec2 p = At(key); Arcane::Test::ClickAt(p, [&] { Frame(); }); }
    };

    CVarDescInfo Desc(CVarType type, std::string widget = {}, std::optional<CVarValue> min = {}, std::optional<CVarValue> max = {})
    {
        CVarDescInfo d;
        d.type = type;
        d.widget = std::move(widget);
        d.min = std::move(min);
        d.max = std::move(max);
        return d;
    }
}

TEST_CASE("RowWidgetFor picks the widget from the type and the widget hint", "[settings-ui]")
{
    CHECK(RowWidgetFor(Desc(CVarType::Bool)) == RowWidget::Checkbox);
    CHECK(RowWidgetFor(Desc(CVarType::Int32)) == RowWidget::Int);
    CHECK(RowWidgetFor(Desc(CVarType::UInt64)) == RowWidget::IntText);
    CHECK(RowWidgetFor(Desc(CVarType::UInt32, {}, CVarValue::UInt32(0), CVarValue::UInt32(64))) == RowWidget::Int);
    CHECK(RowWidgetFor(Desc(CVarType::Int64, {}, CVarValue::Int64(0), CVarValue::Int64(1ll << 40))) == RowWidget::IntText);
    CHECK(RowWidgetFor(Desc(CVarType::Float32)) == RowWidget::Float);
    CHECK(RowWidgetFor(Desc(CVarType::Float32, "slider", CVarValue::Float32(0.0f), CVarValue::Float32(1.0f))) == RowWidget::Slider);
    CHECK(RowWidgetFor(Desc(CVarType::Float32, "slider")) == RowWidget::Float);   // a slider needs both bounds
    CHECK(RowWidgetFor(Desc(CVarType::Float64)) == RowWidget::Double);
    CHECK(RowWidgetFor(Desc(CVarType::String)) == RowWidget::Text);
    CHECK(RowWidgetFor(Desc(CVarType::String, "asset:texture")) == RowWidget::Asset);
    CHECK(RowWidgetFor(Desc(CVarType::String, "path:dir")) == RowWidget::Path);
    CHECK(RowWidgetFor(Desc(CVarType::String, "keychord")) == RowWidget::KeyChord);
    CHECK(RowWidgetFor(Desc(CVarType::String, "font")) == RowWidget::Font);
    CHECK(RowWidgetFor(Desc(CVarType::Color)) == RowWidget::Color);
    CHECK(RowWidgetFor(Desc(CVarType::Vec3)) == RowWidget::Vec);
    CHECK(RowWidgetFor(Desc(CVarType::Enum)) == RowWidget::Enum);
    CHECK(AssetKindFilterFor("asset:texture") == static_cast<int>(AssetKind::Texture));
    CHECK(AssetKindFilterFor("asset:Input Actions") == static_cast<int>(AssetKind::InputActions));
    CHECK(AssetKindFilterFor("asset:") == -1);
    CHECK(AssetKindFilterFor("path:file") == -1);
}

TEST_CASE("A checkbox row writes the window's rung on click, as one undo step", "[settings-ui]")
{
    RowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    h.names = { "render.vsync" };
    h.Frame();
    h.Frame();
    h.Click("Vsync");
    CHECK(h.reg.RungValue("render.vsync", SetBy::Project) == std::optional<CVarValue>(CVarValue::Bool(false)));
    CHECK(h.archive.Dirty());
    REQUIRE(h.undo.CanUndo());
    h.undo.Undo();
    CHECK_FALSE(h.reg.RungValue("render.vsync", SetBy::Project).has_value());
    CHECK_FALSE(h.undo.CanUndo());
}

TEST_CASE("A drag on a number row applies live and lands as ONE undo step; Escape leaves the rung as it was", "[settings-ui]")
{
    RowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "physics.speed", { .type = CVarType::Float32, .def = CVarValue::Float32(1.0f) }).IsStale());
    h.names = { "physics.speed" };
    h.Frame();
    h.Frame();
    int liveChanges = 0;
    float last = 1.0f;
    Arcane::Test::DragAt(h.At("Speed"), 30.0f, [&]
    {
        h.Frame();
        if (const auto v = h.reg.RungValue("physics.speed", SetBy::Project); v && v->AsFloat32() != last)
        {
            ++liveChanges;
            last = v->AsFloat32();
        }
    });
    CHECK(liveChanges >= 2);                                   // the value followed the mouse, frame by frame
    const std::optional<CVarValue> after = h.reg.RungValue("physics.speed", SetBy::Project);
    REQUIRE(after.has_value());
    CHECK(after->AsFloat32() > 1.0f);
    REQUIRE(h.undo.CanUndo());
    h.undo.Undo();
    CHECK_FALSE(h.reg.RungValue("physics.speed", SetBy::Project).has_value());
    CHECK_FALSE(h.undo.CanUndo());                             // one step, not one per frame
    h.Frame();
    Arcane::Test::DragAt(h.At("Speed"), 30.0f, [&] { h.Frame(); }, /*escapeBeforeRelease=*/true);
    CHECK_FALSE(h.reg.RungValue("physics.speed", SetBy::Project).has_value());   // no stray record equal to the value below
    CHECK_FALSE(h.undo.CanUndo());
}

TEST_CASE("The reset arrow shows only when the value differs from the default, and resets undoably", "[settings-ui]")
{
    RowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.lod", { .type = CVarType::Int32, .def = CVarValue::Int32(1),
        .min = CVarValue::Int32(0), .max = CVarValue::Int32(8) }).IsStale());
    h.names = { "render.lod" };
    h.Frame();
    CHECK(h.imgui.probe.count("Lod#reset") == 0);
    REQUIRE(h.reg.SetRung("render.lod", SetBy::Project, CVarValue::Int32(5), "project"));
    h.reg.Publish();
    h.Frame();
    h.Click("Lod#reset");
    CHECK_FALSE(h.reg.RungValue("render.lod", SetBy::Project).has_value());
    h.undo.Undo();
    CHECK(h.reg.RungValue("render.lod", SetBy::Project) == std::optional<CVarValue>(CVarValue::Int32(5)));
}

TEST_CASE("A text row commits once on Enter; Escape keeps the old text", "[settings-ui]")
{
    RowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "app.title", { .type = CVarType::String, .def = CVarValue::String("") }).IsStale());
    h.names = { "app.title" };
    h.Frame();
    h.Frame();
    h.Click("Title");
    Arcane::Test::TypeText("Hello", [&] { h.Frame(); });
    CHECK_FALSE(h.reg.RungValue("app.title", SetBy::Project).has_value());   // typing commits nothing
    Arcane::Test::PressKey(ImGuiKey_Enter, [&] { h.Frame(); });
    CHECK(h.reg.RungValue("app.title", SetBy::Project) == std::optional<CVarValue>(CVarValue::String("Hello")));
    CHECK(h.undo.CanUndo());
    h.Click("Title");
    Arcane::Test::TypeText("XYZ", [&] { h.Frame(); });
    Arcane::Test::PressKey(ImGuiKey_Escape, [&] { h.Frame(); });
    CHECK(h.reg.RungValue("app.title", SetBy::Project) == std::optional<CVarValue>(CVarValue::String("Hello")));
}

TEST_CASE("An overridden row is read-only, names the winning rung, and Clear override pops it undoably", "[settings-ui]")
{
    RowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    REQUIRE(h.reg.Set(h.reg.Find("render.vsync"), CVarValue::Bool(false), SetBy::Console) == SetResult::Applied);
    h.reg.Publish();
    h.names = { "render.vsync" };
    h.Frame();
    h.Frame();
    REQUIRE(h.results.size() == 1);
    CHECK(h.results[0].overridden);
    CHECK(h.results[0].readOnly);
    CHECK(h.imgui.probe.count("Vsync#reset") == 0);
    h.Click("Vsync#clear");
    CHECK_FALSE(h.reg.RungValue("render.vsync", SetBy::Console).has_value());
    h.Frame();
    CHECK_FALSE(h.results[0].overridden);
    h.undo.Undo();
    CHECK(h.reg.RungValue("render.vsync", SetBy::Console) == std::optional<CVarValue>(CVarValue::Bool(false)));
}

TEST_CASE("A Preferences row's switch moves the value between All projects and This project", "[settings-ui]")
{
    RowHarness h;
    h.window = SettingsWindowKind::Preferences;
    REQUIRE_FALSE(AddSetting(h.reg, "editor.graph.fitMinZoom", { .type = CVarType::Float32, .def = CVarValue::Float32(0.5f),
        .scope = SettingScope::PreferencesMachine }).IsStale());
    h.names = { "editor.graph.fitMinZoom" };
    h.Frame();
    h.Frame();
    h.Click("Fit Min Zoom#scope");                                   // All projects -> This project
    CHECK(h.reg.RungValue("editor.graph.fitMinZoom", SetBy::User) == std::optional<CVarValue>(CVarValue::Float32(0.5f)));
    h.Frame();
    h.Click("Fit Min Zoom#scope");                                   // back: nothing shared yet, so it is promoted
    CHECK_FALSE(h.reg.RungValue("editor.graph.fitMinZoom", SetBy::User).has_value());
    CHECK(h.reg.RungValue("editor.graph.fitMinZoom", SetBy::EditorUser) == std::optional<CVarValue>(CVarValue::Float32(0.5f)));
    CHECK(h.undo.CanUndo());
}

TEST_CASE("Badges follow the apply mode and the Deterministic flag", "[settings-ui]")
{
    CVarDescInfo d;
    d.apply = ApplyMode::Restart;
    CHECK((BadgesFor(d) == std::vector<RowBadge>{ RowBadge::Restart }));
    d.apply = ApplyMode::NextWorld;
    d.flags = CVarFlags::Deterministic;
    CHECK((BadgesFor(d) == std::vector<RowBadge>{ RowBadge::NextWorld, RowBadge::Deterministic }));
    d.apply = ApplyMode::Live;
    d.flags = CVarFlags::None;
    CHECK(BadgesFor(d).empty());
}

TEST_CASE("Hovering a row's label shows its help and cvar name; right-click opens the Copy name / Explain menu", "[settings-ui]")
{
    RowHarness h;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true),
        .help = "Wait for the vertical blank." }).IsStale());
    h.names = { "render.vsync" };
    h.Frame();
    h.Frame();
    const ImVec2 label = h.At("Vsync#label");
    ImGui::GetIO().AddMousePosEvent(label.x, label.y);
    for (int i = 0; i < 40; ++i) h.Frame();                          // past the tooltip delay, mouse still
    CHECK(h.lastTooltip == "render.vsync");
    ImGui::GetIO().AddMouseButtonEvent(1, true);
    h.Frame();
    ImGui::GetIO().AddMouseButtonEvent(1, false);
    h.Frame();
    h.Frame();
    CHECK(h.lastContextMenu == "render.vsync");
}

TEST_CASE("With no project open, Project-rung rows are read-only", "[settings-ui]")
{
    RowHarness h;
    h.projectOpen = false;
    REQUIRE_FALSE(AddSetting(h.reg, "render.vsync", { .type = CVarType::Bool, .def = CVarValue::Bool(true) }).IsStale());
    h.names = { "render.vsync" };
    h.Frame();
    h.Frame();
    REQUIRE(h.results.size() == 1);
    CHECK(h.results[0].readOnly);
    CHECK_FALSE(h.results[0].overridden);
    CHECK(h.imgui.probe.count("Vsync#reset") == 0);
}
