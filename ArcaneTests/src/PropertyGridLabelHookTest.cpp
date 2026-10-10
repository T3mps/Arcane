// Settings arc S3-7: RowDecor::label -- a hook run while a row's LABEL is
// the last item (the settings rows hang their help tooltip and context menu
// there), on value rows and read-only rows, without disturbing the value
// widget's LastItemData contract (PropertyGrid.hpp:25-40).
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsFixtures.hpp"
#include <Widgets/PropertyGrid.hpp>
#include <imgui.h>

TEST_CASE("RowDecor::label runs while the label is the last item, on value and read-only rows", "[editor][inspector][settings-ui]")
{
    Arcane::Test::SettingsImGuiHarness h(ImVec2(1280.0f, 720.0f));
    Arcane::Editor::PropertyGridState state;
    state.probe = &h.probe;
    float value = 1.0f;
    int commits = 0, hookCalls = 0, readOnlyHookCalls = 0;
    bool labelHovered = false;
    ImVec2 labelMin{}, labelMax{};
    auto frame = [&]
    {
        h.Frame([&]
        {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
            ImGui::Begin("Grid");
            Arcane::Editor::PropertyGrid grid(state);
            {
                Arcane::Editor::PropertyGrid::Rows rows(grid, "##rows");
                if (rows)
                {
                    Arcane::Editor::RowDecor decor;
                    decor.reset = true;
                    decor.resetActive = true;
                    decor.label = [&](bool)
                    {
                        ++hookCalls;
                        labelHovered = ImGui::IsItemHovered();
                        labelMin = ImGui::GetItemRectMin();
                        labelMax = ImGui::GetItemRectMax();
                    };
                    grid.SetNextRowDecor(decor);
                    if (grid.FloatRow("Speed", value, 0.01f)) ++commits;
                    Arcane::Editor::RowDecor ro;
                    ro.label = [&](bool) { ++readOnlyHookCalls; };
                    grid.SetNextRowDecor(ro);
                    grid.ReadOnlyRow("Name", "fixed");
                }
            }
            ImGui::End();
        });
    };
    frame();
    frame();
    CHECK(hookCalls == 2);
    CHECK(readOnlyHookCalls == 2);
    REQUIRE(h.probe.count("Speed") == 1);
    CHECK(h.probe.count("Speed#reset") == 1);                 // the reset slot still draws after the hook
    CHECK(labelMax.x < h.probe.at("Speed").x);                // the hook saw the LABEL, left of the value
    const ImVec2 labelCentre((labelMin.x + labelMax.x) * 0.5f, (labelMin.y + labelMax.y) * 0.5f);
    ImGui::GetIO().AddMousePosEvent(labelCentre.x, labelCentre.y);
    frame();
    CHECK(labelHovered);
    const ImVec2 valueAt = h.probe.at("Speed");
    ImGui::GetIO().AddMousePosEvent(valueAt.x, valueAt.y);
    frame();
    CHECK_FALSE(labelHovered);
    Arcane::Test::DragAt(valueAt, 30.0f, frame);              // the value is still LastItemData: one commit
    CHECK(commits == 1);
    CHECK(value > 1.0f);
}
