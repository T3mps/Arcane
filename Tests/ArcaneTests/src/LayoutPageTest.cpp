// Settings arc S4 (spec s7.4): Save Current As / Load / Delete / Set As
// Default / Reset To Factory / panels at start.
#include <catch2/catch_test_macros.hpp>
#include "Panels/LayoutLibrary.hpp"
#include "Settings/LayoutPage.hpp"
#include "Settings/LayoutSettings.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <filesystem>

using namespace Arcane::Editor;

namespace
{
    void Revert() { Arcane::CVarRegistry::Get().RevertLayer(Arcane::SetBy::EditorUser); Arcane::CVarRegistry::Get().PublishImmediate(); }
}

TEST_CASE("Layout page: save, set default, load request, delete the default clears it", "[settings-ui][editor]")
{
    LayoutPageState st;
    st.dir = std::filesystem::temp_directory_path() / "s4-layout-page";
    std::filesystem::remove_all(st.dir);
    REQUIRE(SaveLayoutAs(st, "Wide", "[Window][X]\n"));
    CHECK(st.listDirty);
    CHECK(LayoutLibrary(st.dir).Exists("Wide"));
    CHECK_FALSE(SaveLayoutAs(st, "bad/name", "x"));
    CHECK_FALSE(st.status.empty());
    REQUIRE(SetDefaultLayout(st, "Wide"));
    CHECK(cvar_layoutDefault.Get() == "Wide");
    RequestLoadLayout(st, "Wide");
    REQUIRE(st.loadRequest.has_value());
    CHECK(*st.loadRequest == "Wide");
    REQUIRE(DeleteLayout(st, "Wide"));
    CHECK_FALSE(LayoutLibrary(st.dir).Exists("Wide"));
    CHECK(cvar_layoutDefault.Get().empty());

    std::filesystem::remove_all(st.dir);
    Revert();
}

TEST_CASE("Layout page: panels at start write the panel list", "[settings-ui][editor]")
{
    PanelVisibility vis;
    vis.visible[static_cast<std::size_t>(PanelId::AssetGraph)] = false;
    REQUIRE(SetOpenPanelsAtStart(vis));
    CHECK(cvar_layoutOpenPanelsAtStart.Get() == "Outliner,Inspector,Asset Browser,Asset Status,Problems,Console");
    REQUIRE(SetOpenPanelsAtStart(PanelVisibility{}));
    CHECK(cvar_layoutOpenPanelsAtStart.Get() == "*");
    Revert();
}

// S4-19 deferral: a Set As Default that a stronger rung (here --set) outranks
// must report the refusal, not the success line.
TEST_CASE("Layout page: a refused default write reports the refusal instead of success", "[settings-ui][editor]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Revert();
    REQUIRE(reg.Set(cvar_layoutDefault.Handle(), Arcane::CVarValue::String("Locked"),
                    Arcane::SetBy::CommandLine, "test", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied);
    reg.PublishImmediate();

    LayoutPageState st;
    CHECK_FALSE(SetDefaultLayout(st, "Wide"));
    CHECK(st.status.starts_with("Default not applied"));
    CHECK(st.status.find("stronger") != std::string::npos);
    CHECK(cvar_layoutDefault.Get() == "Locked");

    reg.ClearRung(cvar_layoutDefault.Handle(), Arcane::SetBy::CommandLine);
    Revert();
    CHECK(cvar_layoutDefault.Get().empty());
}

// S4-GATE deferral: deleting the default layout while a stronger rung holds it
// must keep the refused clear visible, not overwrite it with "Deleted".
TEST_CASE("Layout page: deleting the default layout under a stronger rung keeps the refusal visible", "[settings-ui][editor]")
{
    Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
    Revert();
    LayoutPageState st;
    st.dir = std::filesystem::temp_directory_path() / "s6gate-layout-page";
    std::filesystem::remove_all(st.dir);
    REQUIRE(SaveLayoutAs(st, "Wide", "[Window][X]\n"));
    REQUIRE(reg.Set(cvar_layoutDefault.Handle(), Arcane::CVarValue::String("Wide"),
                    Arcane::SetBy::CommandLine, "test", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied);
    reg.PublishImmediate();

    CHECK(DeleteLayout(st, "Wide"));
    CHECK_FALSE(LayoutLibrary(st.dir).Exists("Wide"));
    INFO(st.status);
    CHECK(st.status.starts_with("Deleted 'Wide', but it is still the default"));
    CHECK(st.status.find("Default not applied") != std::string::npos);
    CHECK(cvar_layoutDefault.Get() == "Wide");

    reg.ClearRung(cvar_layoutDefault.Handle(), Arcane::SetBy::CommandLine);
    Revert();
    std::filesystem::remove_all(st.dir);
}
