// PropertyGrid (inspector-ownership arc, Task 1): the shared page primitives,
// driven device-less through the REAL ImGui path (EditorInspectorVectorTest's
// harness shape: software atlas, window pinned at the origin, probe centres).
#include <catch2/catch_test_macros.hpp>
#include <Widgets/PropertyGrid.hpp>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace
{
    struct GridHarness
    {
        Arcane::Editor::PropertyGridState state;
        std::unordered_map<std::string, ImVec2> probe;
        ImGuiContext* prev = nullptr;
        ImGuiContext* ctx = nullptr;
        std::string name = "Alpha";
        bool flag = false;
        int commits = 0;
        bool sectionOpen = false;
        bool drawName = true;            // false = the A "Name" row vanishes; a B row draws under another id
        int commitsB = 0;
        std::string nameB = "Beta";
        float storedSeconds = 0.40f;     // the "model": Seconds is re-derived from it EVERY frame
        int floatCommits = 0;
        float scale = 1.0f;
        int scaleCommits = 0;

        GridHarness()
        {
            IMGUI_CHECKVERSION();
            prev = ImGui::GetCurrentContext();
            ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(ctx);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280.0f, 1024.0f);
            io.IniFilename = nullptr;
            unsigned char* pixels = nullptr; int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            state.probe = &probe;
        }
        ~GridHarness() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }

        void Frame()
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            probe.clear();
            ImGui::NewFrame();
            Arcane::Editor::PropertyGrid(state).CommitOrphans();   // once per frame, BEFORE any window that draws this state Begins
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(640, 1000), ImGuiCond_Always);
            ImGui::Begin("Inspector");
            Arcane::Editor::PropertyGrid grid(state);
            sectionOpen = grid.Section("Action");
            if (sectionOpen)
            {
                Arcane::Editor::PropertyGrid::Rows rows(grid, "##fields");
                if (rows)
                {
                    if (drawName) grid.TextRow("Name", name, [&](std::string v) { name = std::move(v); ++commits; });
                    else { ImGui::PushID("B"); grid.TextRow("Name", nameB, [&](std::string v) { nameB = std::move(v); ++commitsB; }); ImGui::PopID(); }
                    grid.CheckboxRow("Blocking", flag);
                    grid.ReadOnlyRow("Path", "<Keyboard>/space");
                    float seconds = storedSeconds;   // re-derived from the "model" EVERY frame, as the input pages do
                    if (grid.FloatRow("Seconds", seconds, 0.01f)) { storedSeconds = seconds; ++floatCommits; }
                    if (grid.FloatRow("Scale", scale, 0.01f)) ++scaleCommits;
                }
            }
            ImGui::End();
            ImGui::Render();
        }
        ImVec2 Centre(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        void Click(ImVec2 at)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMouseButtonEvent(0, false); Frame();
        }
        void Press(ImVec2 at) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(at.x, at.y); Frame(); io.AddMouseButtonEvent(0, true); Frame(); }
    };
}

TEST_CASE("PropertyGrid: rows draw into one shared grid, TextRow commits once, CheckboxRow toggles", "[editor][inspector]")
{
    GridHarness h;
    h.Frame();
    CHECK(h.sectionOpen);                       // DefaultOpen
    CHECK(h.state.labelColWidth > 0.0f);        // the first grid seeded the shared split
    REQUIRE(h.probe.count("Name") == 1);
    REQUIRE(h.probe.count("Blocking") == 1);

    h.Click(h.Centre("Blocking"));
    CHECK(h.flag);

    h.Click(h.Centre("Name"));                  // focus the text field
    ImGui::GetIO().AddInputCharacter('X'); h.Frame();
    CHECK(h.commits == 0);                      // typing alone never commits
    h.Click(ImVec2(600.0f, 900.0f));            // click empty window space: deactivate
    CHECK(h.commits == 1);
    CHECK(h.name == "X");                       // AutoSelectAll: the click selected "Alpha", the keystroke replaced it (D06)
    CHECK(h.name.find('X') != std::string::npos);
    h.Frame();
    CHECK(h.commits == 1);                      // exactly once
}

TEST_CASE("PropertyGrid: a draft deactivated while its row is not drawn still commits to the row's own target", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    h.Click(h.Centre("Name"));
    ImGui::GetIO().AddInputCharacter('X'); h.Frame();
    CHECK(h.commits == 0);
    h.drawName = false;                                  // the A row vanishes (page switched / tab hidden); the B row draws under another id
    h.Frame(); h.Frame(); h.Frame();
    CHECK(h.commits == 1);                               // flushed by CommitOrphans, through A's stored commit
    CHECK(h.name.find('X') != std::string::npos);
    CHECK(h.commitsB == 0);
    CHECK(h.nameB == "Beta");
    h.Frame();
    CHECK(h.commits == 1);
}

TEST_CASE("PropertyGrid: FloatRow accumulates a multi-frame drag against a per-frame re-seeded value and commits once", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    const ImVec2 c = h.Centre("Seconds");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    for (int i = 1; i <= 3; ++i) { io.AddMousePosEvent(c.x + 15.0f * i, c.y); h.Frame(); }   // each step beats the 3 px drag threshold
    CHECK(h.floatCommits == 0);                          // in flight: nothing committed
    io.AddMouseButtonEvent(0, false); h.Frame();
    CHECK(h.floatCommits == 1);                          // exactly once, on release
    CHECK(h.storedSeconds > 0.40f + 0.25f);              // the whole gesture, not one frame's delta, not the original
    h.Frame();
    CHECK(h.floatCommits == 1);
    CHECK(h.state.numericDrafts.empty());
    h.Click(h.Centre("Seconds"));                        // press + release without moving
    CHECK(h.floatCommits == 1);                          // no MarkItemEdited: no commit
}

TEST_CASE("PropertyGrid: Escape during a numeric drag restores the seed and commits nothing", "[editor][inspector]")
{
    GridHarness h; h.Frame();
    const ImVec2 c = h.Centre("Scale");
    ImGuiIO& io = ImGui::GetIO();
    h.Press(c);
    io.AddMousePosEvent(c.x + 40.0f, c.y); h.Frame(); h.Frame();
    CHECK(h.scale != 1.0f);                              // the drag moved it (write-through)
    io.AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    io.AddMouseButtonEvent(0, false); h.Frame(); h.Frame();
    CHECK(h.scale == 1.0f);
    CHECK(h.scaleCommits == 0);
}
