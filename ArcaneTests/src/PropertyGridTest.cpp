// PropertyGrid (inspector-ownership arc, Task 1): the shared page primitives,
// driven device-less through the REAL ImGui path (EditorInspectorVectorTest's
// harness shape: software atlas, window pinned at the origin, probe centres).
#include <catch2/catch_test_macros.hpp>
#include <Widgets/PropertyGrid.hpp>
#include <Widgets/EditorTheme.hpp>   // Theme::kError (the refused-value look)
#include <imgui.h>
#include <imgui_internal.h>   // ColorStack / StyleVarStack (the refused-style balance check)
#include <cstdlib>   // std::abs
#include <optional>
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
        std::function<std::optional<std::string>(std::string_view)> validate;   // the Name row's rule (empty = none)
        int nameStackDrift = 0;          // |colour + style-var stack change| across the Name row, summed over every frame
        bool errorDrawn = false;         // THIS frame: the Inspector window drew a vertex in Theme::kError

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
                    const ImGuiContext& g = *ImGui::GetCurrentContext();
                    const int colours = g.ColorStack.Size, vars = g.StyleVarStack.Size;
                    if (drawName) grid.TextRow("Name", name, [&](std::string v) { name = std::move(v); ++commits; }, false, validate);
                    else { ImGui::PushID("B"); grid.TextRow("Name", nameB, [&](std::string v) { nameB = std::move(v); ++commitsB; }); ImGui::PopID(); }
                    nameStackDrift += std::abs(g.ColorStack.Size - colours) + std::abs(g.StyleVarStack.Size - vars);
                    grid.CheckboxRow("Blocking", flag);
                    grid.ReadOnlyRow("Path", "<Keyboard>/space");
                    float seconds = storedSeconds;   // re-derived from the "model" EVERY frame, as the input pages do
                    if (grid.FloatRow("Seconds", seconds, 0.01f)) { storedSeconds = seconds; ++floatCommits; }
                    if (grid.FloatRow("Scale", scale, 0.01f)) ++scaleCommits;
                }
            }
            ImGui::End();
            ImGui::Render();
            // Draw data, not pixels: did anything in the Inspector use the refusal colour?
            const ImU32 error = ImGui::ColorConvertFloat4ToU32(Arcane::Editor::Theme::kError);
            errorDrawn = false;
            for (const ImDrawVert& v : ImGui::FindWindowByName("Inspector")->DrawList->VtxBuffer) errorDrawn = errorDrawn || v.col == error;
        }
        ImVec2 Centre(const std::string& key) { INFO(key); REQUIRE(probe.count(key) == 1); return probe.at(key); }
        void Click(ImVec2 at)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y); Frame();
            io.AddMouseButtonEvent(0, true); Frame();
            io.AddMouseButtonEvent(0, false); Frame();
        }
        void Type(const char* s) { ImGui::GetIO().AddInputCharactersUTF8(s); Frame(); }
        void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frame(); ImGui::GetIO().AddKeyEvent(k, false); Frame(); }
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

TEST_CASE("PropertyGrid: a TextRow value refused on Enter keeps the typed text and re-arms; focus loss with a refused value reverts", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("X");                                      // AutoSelectAll: replaces "Alpha"
    h.Key(ImGuiKey_Enter);
    h.Frame(); h.Frame();
    CHECK(h.commits == 0);
    CHECK(h.name == "Alpha");
    REQUIRE(h.state.textDrafts.size() == 1);
    CHECK(h.state.textDrafts.begin()->second.text == "X");      // kept
    CHECK(h.state.textDrafts.begin()->second.active);           // re-armed
    h.Type("Y");                                      // re-activation selected all: "Y" replaces "X"
    h.Click(ImVec2(600, 900));
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");

    h.Click(h.Centre("Name"));
    h.Type("X");
    h.Click(ImVec2(600, 900));                        // focus loss with a refused value
    h.Frame();
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");
    for (const auto& [id, d] : h.state.textDrafts) CHECK(d.text != "X");
}

TEST_CASE("PropertyGrid: a refused-Enter hold that expires because its row vanished never grabs focus later (final review)", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("X");
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Enter, true); h.Frame();         // refused: hold + a re-arm queued for the next draw
    h.drawName = false;                                      // the row vanishes before the re-arm lands
    io.AddKeyEvent(ImGuiKey_Enter, false); h.Frame(); h.Frame();
    h.drawName = true;                                       // back: the hold has expired
    for (int i = 0; i < 6; ++i) h.Frame();
    CHECK_FALSE(ImGui::IsAnyItemActive());                   // the box never took focus on its own
    CHECK(h.commits == 0);
    CHECK(h.name == "Alpha");
    bool showsModel = false;
    for (const auto& [id, d] : h.state.textDrafts)
    {
        CHECK_FALSE(d.active);
        CHECK(d.text != "X");                                // the refused text is gone
        showsModel = showsModel || d.text == "Alpha";
    }
    CHECK(showsModel);
}

TEST_CASE("PropertyGrid: a second Enter on a refused TextRow value, with no new typing, still keeps it and re-arms (D4, final review)", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    h.Click(h.Centre("Name"));
    h.Type("X");
    h.Key(ImGuiKey_Enter);                                   // refused: kept + re-armed
    h.Frame(); h.Frame();
    REQUIRE(h.state.textDrafts.size() == 1);
    REQUIRE(h.state.textDrafts.begin()->second.text == "X");
    REQUIRE(h.state.textDrafts.begin()->second.active);
    h.Key(ImGuiKey_Enter);                                   // again, no typing: this activation has no edit
    h.Frame(); h.Frame(); h.Frame();
    CHECK(h.commits == 0);
    CHECK(h.name == "Alpha");
    REQUIRE(h.state.textDrafts.size() == 1);
    CHECK(h.state.textDrafts.begin()->second.text == "X");      // still kept
    CHECK(h.state.textDrafts.begin()->second.active);           // still re-armed
}

TEST_CASE("PropertyGrid: a refused TextRow value draws in Theme::kError while typed and while held after Enter, a valid or committed value never does, and the style stacks balance", "[editor][inspector]")
{
    GridHarness h;
    h.validate = [](std::string_view v) -> std::optional<std::string> { return v == "X" ? std::optional<std::string>("taken") : std::nullopt; };
    h.Frame();
    CHECK_FALSE(h.errorDrawn);                               // the model's own value: no red
    h.Click(h.Centre("Name"));
    CHECK_FALSE(h.errorDrawn);                               // active, unchanged: no red
    h.Type("X"); h.Frame();                                  // the look trails the keystroke by one frame
    CHECK(h.errorDrawn);                                     // typing a refused value: red
    h.Key(ImGuiKey_Enter);                                   // refused: kept + re-armed (the hold)
    h.Frame(); h.Frame();
    REQUIRE(h.state.textDrafts.size() == 1);
    REQUIRE(h.state.textDrafts.begin()->second.text == "X");
    CHECK(h.errorDrawn);                                     // held after the refused Enter: still red
    h.Type("Y"); h.Frame();                                  // re-armed select-all: "Y" replaces "X"
    CHECK_FALSE(h.errorDrawn);                               // a valid value: no red
    h.Click(ImVec2(600, 900));
    h.Frame();
    CHECK(h.commits == 1);
    CHECK(h.name == "Y");
    CHECK_FALSE(h.errorDrawn);                               // committed: no red

    h.Click(h.Centre("Name"));
    h.Type("X"); h.Frame();
    CHECK(h.errorDrawn);
    h.Click(ImVec2(600, 900));                               // focus loss with a refused value: revert
    h.Frame();
    CHECK(h.name == "Y");
    CHECK_FALSE(h.errorDrawn);                               // reverted: no red
    CHECK(h.nameStackDrift == 0);                            // every push popped inside the row, every frame
}
