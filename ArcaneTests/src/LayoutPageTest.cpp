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
