// InspectorHost (inspector-ownership spec s5): the PURE routing -- which
// source's page an Inspector instance shows -- driven with fake sources, no
// ImGui. Every rule in spec s3.1/s3.3/s6 has a case here. The ImGui cases
// (the instance list's ini section, one device-less frame of the windows)
// run on bare contexts, no device.
#include <catch2/catch_test_macros.hpp>
#include <Panels/DefaultLayout.hpp>
#include <Panels/InspectorHost.hpp>
#include <Panels/InspectorWindows.hpp>   // RegisterInspectorInstancesSettings
#include <imgui.h>
#include <imgui_internal.h>   // ClearIniSettings (the windowed switch's reset)
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace Arcane::Editor;

namespace
{
    struct FakePage final : InspectorPage
    {
        std::vector<InspectorCrumb> crumbs;
        std::vector<InspectorCrumb> Breadcrumb() const override { return crumbs; }
        void Draw(PropertyGrid&) override {}
    };
    struct FakeSource final : InspectorSource
    {
        std::string name;
        std::string kind;
        std::string key;
        bool restoreOk = true;
        bool hasPage = true;                // false: Page() is null (an asset source with nothing selected)
        std::string normalizeTo;            // non-empty: RestoreSelection lands on THIS key, not the asked one (a member died in between)
        std::vector<std::string> restored;
        FakePage page;
        explicit FakeSource(std::string n, std::string k = "scene") : name(std::move(n)), kind(std::move(k)) {}
        std::string SourceName() const override { return name; }
        std::string_view Kind() const override { return kind; }
        InspectorPage* Page() override { return hasPage ? &page : nullptr; }
        InspectorPage* PageFor(std::string_view) override { return restoreOk ? &page : nullptr; }
        std::string SelectionKey() const override { return key; }
        bool RestoreSelection(std::string_view k) override
        {
            if (!restoreOk) return false;
            key = normalizeTo.empty() ? std::string(k) : normalizeTo;
            restored.push_back(key);
            return true;
        }
        bool Resolves(std::string_view) const override { return restoreOk; }
    };
    struct World
    {
        FakeSource scene{ "Scene", "scene" }, doc{ "Player.arcinput", "input-actions" }, other{ "brick.png", "assets" }, doc2{ "Menu.arcinput", "input-actions" };
        InspectorHost host{ scene };
        World() { host.AddSource(doc); host.AddSource(other); host.AddSource(doc2); }
        void Select(FakeSource& s, std::string k) { s.key = std::move(k); host.NotifySelected(s); }
    };
}

TEST_CASE("InspectorHost: the last-selecting source wins; nothing else moves it", "[editor][inspector]")
{
    World w;
    CHECK(&w.host.Current() == &w.scene);            // fallback at rest
    w.Select(w.doc, "m/a//");
    CHECK(&w.host.Current() == &w.doc);
    w.Select(w.scene, "7");
    CHECK(&w.host.Current() == &w.scene);
    // Focus is not selection: touching the other source's page, adding an
    // instance, reading keys -- none of it is an event.
    (void)w.other.Page(); (void)w.host.AddInstance(); (void)w.host.SourceFor(0);
    CHECK(&w.host.Current() == &w.scene);
    // A deselect (empty key) is not a selection event either.
    w.Select(w.doc, "");
    CHECK(&w.host.Current() == &w.scene);
    // A re-select of the scene's UNCHANGED key is still a selection event
    // (UE: SelectActor re-notifies the Details views even when the set did
    // not change): doc selects, scene re-selects "7" -> scene wins again, and
    // history gains a scene entry so Back returns to the document.
    w.Select(w.doc, "m/a//");
    CHECK(&w.host.Current() == &w.doc);
    w.Select(w.scene, "7");
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.CanGoBack());
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.doc);
}

TEST_CASE("SelectionEdge: a selection event is a gesture (epoch moved) with a non-empty key", "[editor][inspector]")
{
    SelectionEdge edge;
    CHECK(edge.Observe(1, "7"));        // first gesture
    CHECK_FALSE(edge.Observe(1, "7"));  // nothing happened (focus, idle)
    CHECK_FALSE(edge.Observe(1, "9"));  // key moved WITHOUT a gesture (a prune re-primaried): not an event
    CHECK(edge.Observe(2, "7"));        // re-clicking the already-selected entity IS an event
    CHECK_FALSE(edge.Observe(3, ""));   // a clear moves the epoch but is never an event
    CHECK(edge.Observe(4, "7"));        // re-selecting after a clear is one
}

TEST_CASE("InspectorHost: pin holds a page while others select; a second instance follows", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const int second = w.host.AddInstance();
    w.Select(w.scene, "7");
    CHECK(w.host.SourceFor(0) == &w.doc);
    CHECK(w.host.Find(0)->pinnedKey == "m/a//");
    CHECK(w.host.SourceFor(second) == &w.scene);
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
    w.scene.restoreOk = false;                       // PageFor(key) is null: nothing to hold
    w.host.SetPinned(second, true);
    CHECK_FALSE(w.host.Find(second)->pinned);
    w.scene.restoreOk = true; w.scene.key.clear();   // an EMPTY key whose page resolves (a document's asset page) IS pinnable
    w.host.SetPinned(second, true);
    CHECK(w.host.Find(second)->pinned);
    CHECK(w.host.Find(second)->pinnedKey.empty());
    w.host.SetPinned(second, false);
    w.host.RemoveInstance(0);                        // refused: instance 0 is permanent
    CHECK(w.host.Instances().size() == 2);
    w.host.RemoveInstance(second);
    CHECK(w.host.Instances().size() == 1);
}

TEST_CASE("InspectorHost: a closed source releases the follower and flags the pin", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const int second = w.host.AddInstance();
    w.Select(w.doc, "m/b//");
    w.host.RemoveSource(w.doc);
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.SourceFor(second) == &w.scene);
    REQUIRE(w.host.Find(0)->pinned);
    CHECK(w.host.Find(0)->sourceClosed);
    CHECK(w.host.SourceFor(0) == nullptr);
    CHECK(w.host.Find(0)->pinnedName == "Player.arcinput");
    CHECK(w.host.History().empty());                 // every entry named the closed source
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost: history back/forward restores the selection in its source", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a");
    w.Select(w.doc, "b");
    w.Select(w.scene, "7");
    REQUIRE(w.host.History().size() == 3);
    CHECK(w.host.History()[2].label == "Scene");   // FakePage has no crumbs: the label is the source name
    CHECK(w.host.HistoryCursor() == 2);
    CHECK(w.host.CanGoBack());
    CHECK_FALSE(w.host.CanGoForward());
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.doc);
    CHECK(w.doc.restored.back() == "b");
    // The document re-reports the restored key next frame: the same entry
    // as the cursor, so nothing is pushed and forward history survives.
    w.host.NotifySelected(w.doc);
    CHECK(w.host.History().size() == 3);
    CHECK(w.host.CanGoForward());
    REQUIRE(w.host.GoBack());
    CHECK(w.doc.restored.back() == "a");
    REQUIRE(w.host.GoForward());
    CHECK(w.doc.restored.back() == "b");
    // A NEW selection at cursor 1 truncates the forward entries.
    w.Select(w.other, "n1");
    CHECK(w.host.History().size() == 3);
    CHECK_FALSE(w.host.CanGoForward());
}

TEST_CASE("InspectorHost: GoBack skips and prunes entries that no longer resolve", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1");
    w.Select(w.doc, "a");
    w.Select(w.doc, "b");
    w.Select(w.scene, "7");
    w.doc.restoreOk = false;                         // the bindings were deleted
    REQUIRE(w.host.GoBack());
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.scene.restored.back() == "1");
    CHECK(w.host.History().size() == 2);             // both doc entries pruned
    w.scene.restoreOk = false;
    CHECK_FALSE(w.host.GoForward());                 // "7" unresolvable: pruned, nothing to land on
    CHECK(w.host.History().size() == 1);
}

TEST_CASE("InspectorHost: history is capped at 32 and never grows past it", "[editor][inspector]")
{
    World w;
    for (int i = 0; i < 40; ++i) w.Select(w.doc, "k" + std::to_string(i));
    CHECK(w.host.History().size() == InspectorHost::kHistoryDepth);
    CHECK(w.host.History().front().key == "k8");
    CHECK(w.host.HistoryCursor() == InspectorHost::kHistoryDepth - 1);
}

TEST_CASE("InspectorHost: ReleaseAll drops every non-fallback source, the history and the pins", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a");
    w.host.SetPinned(0, true);
    const int ids[] = { 2, 3, 3, 0, 99 }; w.host.SetInstanceIds(ids);   // {0,2,3}: 0 implicit, dup + out-of-range dropped
    CHECK(w.host.Instances().size() == 3);
    w.host.ReleaseAll();
    CHECK(&w.host.Current() == &w.scene);
    CHECK(w.host.History().empty());
    for (const auto& inst : w.host.Instances()) CHECK_FALSE(inst.pinned);
    CHECK(w.host.Instances().size() == 3);           // instances are layout, not project state
    w.host.NotifySelected(w.doc);                    // an unregistered source is ignored
    CHECK(&w.host.Current() == &w.scene);
}

TEST_CASE("InspectorHost: InvalidateSource prunes history and releases pins but keeps the source registered", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1");
    w.host.SetPinned(0, true);
    w.Select(w.doc, "a");
    REQUIRE(w.host.History().size() == 2);
    w.host.InvalidateSource(w.scene);                // the registry was replaced
    CHECK(&w.host.Current() == &w.doc);              // current is untouched
    REQUIRE(w.host.History().size() == 1);
    CHECK(w.host.History()[0].key == "a");
    CHECK(w.host.HistoryCursor() == 0);
    REQUIRE(w.host.Find(0)->pinned);
    CHECK(w.host.SourceFor(0) == nullptr);           // "Pinned selection is gone"
    CHECK_FALSE(w.host.Find(0)->sourceClosed);
    w.host.NotifySelected(w.scene);                  // still registered: the next scene selection lands
    CHECK(&w.host.Current() == &w.scene);
    w.host.SetPinned(0, false);
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost: PruneStale drops entries that no longer resolve and CanGoBack turns false without a click", "[editor][inspector]")
{
    World w;
    w.Select(w.scene, "1"); w.Select(w.doc, "a"); w.Select(w.doc, "b"); w.Select(w.scene, "7");
    w.scene.restoreOk = false; w.doc.restoreOk = false;
    w.host.PruneStale();
    CHECK(w.host.History().empty());
    CHECK_FALSE(w.host.CanGoBack());
    CHECK_FALSE(w.host.CanGoForward());
    CHECK(w.scene.restored.empty());                 // nothing was selected by the prune
}

TEST_CASE("InspectorHost: pruning an earlier duplicate keeps the cursor on the same entry", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a"); w.Select(w.other, "x"); w.Select(w.doc, "b"); w.Select(w.doc, "a");
    REQUIRE(w.host.History().size() == 4);
    w.host.RemoveSource(w.other);
    REQUIRE(w.host.History().size() == 3);
    CHECK(w.host.HistoryCursor() == 2);
    CHECK(w.host.History()[2].key == "a");
    REQUIRE(w.host.GoBack());
    CHECK(w.doc.restored.back() == "b");             // a key search would have landed on index 0 and Back done nothing
}

TEST_CASE("InspectorHost: a restore that lands on a normalized key keeps forward history", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "a,b,c"); w.Select(w.scene, "7");
    w.doc.normalizeTo = "a,c";                       // b died between push and restore
    REQUIRE(w.host.GoBack());
    CHECK(w.host.History()[0].key == "a,c");         // re-snapshotted
    w.host.NotifySelected(w.doc);                    // the next-frame re-report
    CHECK(w.host.History().size() == 2);             // an echo, not a new entry
    CHECK(w.host.CanGoForward());
    REQUIRE(w.host.GoForward());
    CHECK(w.scene.restored.back() == "7");
}

TEST_CASE("InspectorHost: JumpTo lands on the chosen entry or prunes only it; entries carry labels", "[editor][inspector]")
{
    World w;
    w.doc.page.crumbs = { { "Player.arcinput", {}, {} }, { "Player", {}, {} }, { "Jump", {}, {} } };
    w.Select(w.scene, "1"); w.Select(w.doc, "a"); w.Select(w.doc, "b"); w.Select(w.scene, "7");
    CHECK(w.host.History()[1].label == "Player.arcinput > Player > Jump");
    REQUIRE(w.host.JumpTo(1));
    CHECK(w.doc.restored.back() == "a");
    CHECK(w.host.HistoryCursor() == 1);
    CHECK(w.host.CanGoForward());
    CHECK(w.host.BackEntry() != nullptr); CHECK(w.host.BackEntry()->key == "1");
    w.doc.restoreOk = false;
    CHECK_FALSE(w.host.JumpTo(2));                   // that one entry is pruned, nothing else moves
    CHECK(w.host.History().size() == 3);
    CHECK(w.host.HistoryCursor() == 1);
    CHECK_FALSE(w.host.JumpTo(1));                   // index == cursor: no-op
    CHECK_FALSE(w.host.JumpTo(99));
}

TEST_CASE("InspectorHost: instance ids are a reusable pool of 8 slots, never minted", "[editor][inspector]")
{
    World w;
    CHECK(w.host.AddInstance() == 1);
    CHECK(w.host.AddInstance() == 2);
    w.host.RemoveInstance(1);
    CHECK(w.host.AddInstance() == 1);                // the freed slot, not 3
    for (int i = 0; i < 8; ++i) (void)w.host.AddInstance();
    CHECK(w.host.Instances().size() == static_cast<std::size_t>(InspectorHost::kMaxInstances));
    CHECK(w.host.AddInstance() == -1);
    w.host.RemoveInstance(0);                        // still refused
    CHECK(w.host.Instances().size() == static_cast<std::size_t>(InspectorHost::kMaxInstances));
    const int ids[] = { 0, 3, 3, 42, -1 };
    w.host.SetInstanceIds(ids);
    REQUIRE(w.host.Instances().size() == 2);
    CHECK(w.host.Instances()[1].id == 3);
}

TEST_CASE("InspectorHost: RepinKey moves only the pinned instance's key", "[editor][inspector]")
{
    World w;
    w.Select(w.doc, "m/a//");
    w.host.SetPinned(0, true);
    const auto historyBefore = w.host.History().size();
    w.host.RepinKey(0, "m///");
    CHECK(w.host.Find(0)->pinnedKey == "m///");
    CHECK(w.host.SourceFor(0) == &w.doc);
    CHECK(w.doc.key == "m/a//");                       // the source's selection did not move
    CHECK(w.host.History().size() == historyBefore);   // no history push
    w.host.SetPinned(0, false);
    w.host.RepinKey(0, "x");                           // unpinned: ignored
    CHECK(w.host.Find(0)->pinnedKey.empty());
}

// The [EditorInspector][Instances] ini section (arc-1 debt F): the handler
// moved beside InspectorWindows so it can be driven on a bare ImGui context
// (the ShaderEditorDocumentTest "material panel layout round-trips through
// imgui.ini" pattern). The clear half is what a WINDOWED project switch does
// (EditorApp::RetargetLayoutIni: ClearIniSettings, then the incoming file):
// a file without the section must leave exactly {0}, never the outgoing
// project's extra instances.
TEST_CASE("InspectorHost: the [EditorInspector][Instances] section round-trips, and a clear + reload without it resets to {0}", "[editor][inspector]")
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    // Restores the previous context even when a REQUIRE below throws.
    struct ContextGuard
    {
        ImGuiContext* ctx; ImGuiContext* prev;
        ~ContextGuard() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    } guard{ ctx, prev };
    ImGui::SetCurrentContext(ctx);
    ImGui::GetIO().IniFilename = nullptr;   // never let a test touch a real ini
    FakeSource scene{ "Scene" };            // InspectorHost has no default ctor: it needs its fallback source
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    RegisterInspectorInstancesSettings(host);                           // idempotent
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=2,3\n");
    REQUIRE(host.Instances().size() == 3);
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    CHECK(saved.find("[EditorInspector][Instances]\nIds=2,3") != std::string::npos);
    CHECK(saved.find("[EditorInspector]") == saved.rfind("[EditorInspector]"));   // one section
    ImGui::ClearIniSettings();                                          // the switch's reset
    ImGui::LoadIniSettingsFromMemory("[EditorPanels][Visibility]\nConsole=1\n");
    CHECK(host.Instances().size() == 1);
    CHECK(host.Instances()[0].id == 0);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=0,3,3,42,-1\n");
    CHECK(host.Instances().size() == 2);                                // {0,3}
}

// ---- Filters= persistence, the legacy upgrade, the default configuration (spec s6/s7) ----
namespace
{
    // One bare ImGui context per case (the existing ini case's shape).
    struct IniContext
    {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGuiContext* ctx = ImGui::CreateContext();
        IniContext() { ImGui::SetCurrentContext(ctx); ImGui::GetIO().IniFilename = nullptr; }
        ~IniContext() { ImGui::DestroyContext(ctx); ImGui::SetCurrentContext(prev); }
    };
}

TEST_CASE("InspectorHost ini: Filters= round-trips after Ids=; a Filters= line means no upgrade", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1,3\nFilters=0:assets,1:scene+input-actions+material+sprite+mesh\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(host.Find(1)->filter == InspectorFilter::Only("assets"));
    CHECK(host.Find(3)->filter.IsAll());
    const std::string saved = ImGui::SaveIniSettingsToMemory();
    CHECK(saved.find("Ids=1,3\nFilters=0:assets,1:scene+input-actions+material+sprite+mesh\n") != std::string::npos);
}

TEST_CASE("InspectorHost ini: garbage Filters= entries are sanitized, never thrown on", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory(
        "[EditorInspector][Instances]\nIds=2\nFilters=7:scene,x:assets,2,0:bogus+assets,,2:scene+assets+input-actions+material+sprite+mesh,\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());           // the line was present: not legacy
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));   // bogus dropped
    CHECK(host.Find(2)->filter.IsAll());                    // all-excluded -> All
    CHECK(host.Instances().size() == 2);                    // id 7 never created
}

TEST_CASE("InspectorHost ini: a layout without Filters= is flagged once for the legacy upgrade", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=\n");
    CHECK(host.TakeLegacyLayoutUpgrade());
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());            // consumed
    ImGui::LoadIniSettingsFromMemory("[EditorPanels][Visibility]\nConsole=1\n");   // no section at all: also legacy
    CHECK(host.TakeLegacyLayoutUpgrade());
    // A second load WITHOUT ClearIniSettings between: instances absent from
    // the Filters= line read as All (spec s7), survivors of Ids= included.
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=0:assets,1:scene\n");
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    REQUIRE(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    REQUIRE(host.Find(1)->filter == InspectorFilter::AllBut("scene"));
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=\n");   // empty line: a real answer
    CHECK_FALSE(host.TakeLegacyLayoutUpgrade());
    CHECK(host.Find(0)->filter.IsAll());
    CHECK(host.Find(1)->filter.IsAll());
}

TEST_CASE("InspectorHost: the default and the legacy-upgrade configurations", "[editor][inspector]")
{
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    (void)host.AddInstance(); (void)host.AddInstance();     // {0,1,2}
    host.ApplyDefaultInspectorLayout();
    REQUIRE(host.Instances().size() == 2);
    CHECK(host.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(host.Find(InspectorHost::kAssetsInstanceId)->filter == InspectorFilter::Only("assets"));

    InspectorHost legacy{ scene };
    const int ids[] = { 1 };
    legacy.SetInstanceIds(ids);                             // the user already had "Inspector 2"
    const int assetsId = legacy.UpgradeLegacyInspectorLayout();
    CHECK(assetsId == 2);                                   // lowest FREE id; the user's 1 is untouched
    CHECK(legacy.Find(1)->filter.IsAll());
    CHECK(legacy.Find(0)->filter == InspectorFilter::AllBut("assets"));
    CHECK(legacy.Find(2)->filter == InspectorFilter::Only("assets"));
}

TEST_CASE("InspectorWindowTitle: the filter label rides the title; the ### id never changes", "[editor][inspector]")
{
    InspectorHost::Instance main;                        // id 0, All
    CHECK(InspectorWindowTitle(main) == "Inspector###Inspector");
    // The stable id: every title of instance 0 hashes to the same window id,
    // and ImHashStr skips "###" (imgui.cpp:2539-2544), so that id IS the legacy
    // bare "Inspector" one -- true by construction; these CHECKs pin it.
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr("Inspector - Scene###Inspector"));
    CHECK(ImHashStr("Inspector###Inspector") == ImHashStr(kPrimaryInspectorWindowId));
    CHECK(ImHashStr(kPrimaryInspectorWindowId) == ImHashStr("Inspector"));   // [Window][Inspector] carries over
    main.filter = InspectorFilter::AllBut("assets");
    CHECK(InspectorWindowTitle(main) == "Inspector - All but Assets###Inspector");
    InspectorHost::Instance second; second.id = 1;
    CHECK(InspectorWindowTitle(second) == "Inspector 2###inspector_1");
    second.filter = InspectorFilter::Only("assets");
    CHECK(InspectorWindowTitle(second) == "Inspector 2 - Assets###inspector_1");
}

// The windows themselves, one device-less frame: the filtered title reaches
// the ImGui window, and an instance whose filter admits nothing registered
// draws its one-line note. The note text is probed through ImGui's own
// LogToBuffer (every RenderText is logged while it is on), read BEFORE
// Render: the log ends with the implicit Debug window at EndFrame.
TEST_CASE("DrawInspectorWindows: filtered titles and the no-source / no-selection lines", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);   // the software atlas (InputActionsDocumentUiTest's shape)
    FakeSource scene{ "Scene", "scene" };
    scene.hasPage = false;                              // a routed source with nothing to show
    InspectorHost host{ scene };
    const int input = host.AddInstance();               // 1
    REQUIRE(input == 1);
    REQUIRE(host.SetFilter(input, InspectorFilter::Only("input-actions")));   // no input-actions source registered
    const int docs = host.AddInstance();                // 2: several kinds admitted, none registered
    InspectorFilter noSceneNoAssets;
    noSceneNoAssets.excluded = { "scene", "assets" };
    REQUIRE(host.SetFilter(docs, noSceneNoAssets));
    InspectorWindowsState state;

    ImGui::NewFrame();
    ImGui::LogToBuffer();
    (void)DrawInspectorWindows(host, state, nullptr);
    const std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();

    ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr("###inspector_1"));
    REQUIRE(w != nullptr);
    CHECK(std::string(w->Name) == "Inspector 2 - Input Actions###inspector_1");
    ImGuiWindow* primary = ImGui::FindWindowByName(kPrimaryInspectorWindowId);
    REQUIRE(primary != nullptr);
    CHECK(std::string(primary->Name) == "Inspector###Inspector");
    INFO(logged);
    CHECK(logged.find("No Input Actions document open") != std::string::npos);
    CHECK(logged.find("Nothing to show for this filter") != std::string::npos);
    CHECK(logged.find("No selection") != std::string::npos);   // instance 0: the scene is routed but has no page
}

TEST_CASE("InspectorHost ini: ClearAllFn resets filters with the list", "[editor][inspector]")
{
    IniContext ic;
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    RegisterInspectorInstancesSettings(host);
    ImGui::LoadIniSettingsFromMemory("[EditorInspector][Instances]\nIds=1\nFilters=0:assets\n");
    ImGui::ClearIniSettings();
    CHECK(host.Instances().size() == 1);
    CHECK(host.Find(0)->filter.IsAll());
}

TEST_CASE("InspectorHost filters: a Scene instance keeps the scene while a document selects", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();                        // 1
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "A");
    w.Select(w.scene, "B");
    w.Select(w.doc, "Player/Jump");
    CHECK(w.host.SourceFor(0) == &w.doc);                          // All follows the binding
    CHECK(w.host.SourceFor(scene) == &w.scene);                    // Scene keeps B
    CHECK(w.scene.key == "B");
}

TEST_CASE("InspectorHost filters: a clear in one source never moves an instance routed elsewhere", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "Player/Jump");
    w.Select(w.scene, "7");
    w.Select(w.scene, "");                                         // empty-space click: a clear
    CHECK(w.host.SourceFor(input) == &w.doc);
    CHECK(w.doc.key == "Player/Jump");
    // An All instance stays on the scene (a clear is not an event) and a
    // document clear does not move it either.
    CHECK(w.host.SourceFor(0) == &w.scene);
    w.Select(w.doc, "");
    CHECK(w.host.SourceFor(0) == &w.scene);
}

TEST_CASE("InspectorHost filters: Current wins when admitted, else the latest admitted stamp", "[editor][inspector]")
{
    World w;
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    w.Select(w.doc, "a");
    w.Select(w.doc2, "b");
    w.Select(w.other, "brick");                                    // an asset click
    CHECK(w.host.SourceFor(0) == &w.other);
    CHECK(w.host.SourceFor(noAssets) == &w.doc2);                  // the latest admitted, never the asset
    w.host.RemoveSource(w.doc2);                                   // closing it falls back to the other
    CHECK(w.host.SourceFor(noAssets) == &w.doc);
    w.Select(w.scene, "7");
    CHECK(w.host.SourceFor(noAssets) == &w.scene);                 // Current() admitted: agrees with All
}

TEST_CASE("InspectorHost filters: closing the last admitted source leaves null, never a dangling pointer", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "a");
    w.host.RemoveSource(w.doc);
    w.host.RemoveSource(w.doc2);
    CHECK(w.host.SourceFor(input) == nullptr);
}

TEST_CASE("InspectorHost filters: nothing stamped falls back to the scene, else the newest admitted source", "[editor][inspector]")
{
    World w;
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    CHECK(w.host.SourceFor(noAssets) == &w.scene);
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    CHECK(w.host.SourceFor(input) == &w.doc2);                     // most recently added
}

TEST_CASE("InspectorHost filters: an empty-kind source is admitted only by All", "[editor][inspector]")
{
    World w;
    FakeSource mesh{ "rock.arcmesh", "" };
    w.host.AddSource(mesh);
    const int noAssets = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(noAssets, InspectorFilter::AllBut("assets")));
    w.Select(mesh, "m");                                           // (a real mesh doc never selects; the rule still holds)
    CHECK(w.host.SourceFor(0) == &mesh);
    CHECK(w.host.SourceFor(noAssets) == &w.scene);
}

TEST_CASE("InspectorHost filters: SetFilter refuses an all-excluded set and an unknown id", "[editor][inspector]")
{
    World w;
    InspectorFilter none;
    none.excluded = { "scene", "assets", "input-actions", "material", "sprite", "mesh" };
    CHECK_FALSE(w.host.SetFilter(0, none));
    CHECK(w.host.Find(0)->filter.IsAll());
    CHECK_FALSE(w.host.SetFilter(5, InspectorFilter::Only("scene")));
}

TEST_CASE("InspectorHost filters: a pin captures THIS instance's page; the pin wins over a filter change", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "B");
    w.Select(w.doc, "Player/Jump");                                // Current() is the document
    REQUIRE(w.host.CanPin(scene));
    w.host.SetPinned(scene, true);
    CHECK(w.host.Find(scene)->pinnedSource == &w.scene);           // not the binding
    CHECK(w.host.Find(scene)->pinnedKey == "B");
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("input-actions")));
    CHECK(w.host.SourceFor(scene) == &w.scene);                    // pinned: unchanged
    w.host.SetPinned(scene, false);
    CHECK(w.host.SourceFor(scene) == &w.doc);                      // follows the new filter
}

TEST_CASE("InspectorHost filters: ReleaseAll keeps a permanent source and drops its history and pins", "[editor][inspector]")
{
    FakeSource scene{ "Scene", "scene" }, assets{ "Assets", "assets" }, doc{ "P.arcinput", "input-actions" };
    InspectorHost host{ scene };
    host.AddSource(assets, /*permanent*/ true);
    host.AddSource(doc);
    const int onlyAssets = host.AddInstance();
    REQUIRE(host.SetFilter(onlyAssets, InspectorFilter::Only("assets")));
    assets.key = "g1"; host.NotifySelected(assets);
    host.SetPinned(onlyAssets, true);
    host.ReleaseAll();
    CHECK(host.SourceFor(onlyAssets) == &assets);                  // still registered, pin released
    CHECK_FALSE(host.Find(onlyAssets)->pinned);
    CHECK(host.History().empty());
    CHECK(host.Find(onlyAssets)->filter == InspectorFilter::Only("assets"));   // filters are layout
    doc.key = "x"; host.NotifySelected(doc);                       // doc was dropped: ignored
    CHECK(&host.Current() == &scene);
    assets.key = "g2"; host.NotifySelected(assets);                // assets still live
    CHECK(&host.Current() == &assets);
}

TEST_CASE("InspectorHost filters: Back in a Scene instance skips document entries and re-selects in the scene", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.scene, "A");        // 0
    w.Select(w.doc, "d1");         // 1
    w.Select(w.scene, "B");        // 2
    w.Select(w.doc, "d2");         // 3  <- cursor
    CHECK(w.host.PositionFor(scene) == std::optional<std::size_t>{ 2 });   // the scene's live "B"
    REQUIRE(w.host.CanGoBack(scene));
    CHECK(w.host.BackEntry(scene)->key == "A");                     // position = 2 (B); back = 0 (A)
    CHECK(w.host.BackIndices(scene) == std::vector<std::size_t>{ 0 });
    CHECK_FALSE(w.host.CanGoForward(scene));
    REQUIRE(w.host.GoBack(scene));
    CHECK(w.scene.key == "A");
    CHECK(&w.host.Current() == &w.scene);                           // a landing IS a selection: All follows
    CHECK(w.host.HistoryCursor() == 0);
    CHECK(w.host.ForwardEntry(scene)->key == "B");                  // skips d1
    CHECK(w.host.ForwardIndices(scene) == std::vector<std::size_t>{ 2 });
}

TEST_CASE("InspectorHost filters: an All instance matches the unfiltered history exactly", "[editor][inspector]")
{
    World w;
    constexpr int all = 0;
    w.Select(w.scene, "A");
    w.Select(w.doc, "d1");
    w.Select(w.scene, "B");
    CHECK(w.host.BackIndex(all) == std::optional<std::size_t>{ 1 });
    CHECK(w.host.BackEntry(all) == w.host.BackEntry());
    REQUIRE(w.host.GoBack(all));
    CHECK(w.host.ForwardIndex(all) == std::optional<std::size_t>{ 2 });
}

TEST_CASE("InspectorHost filters: a filtered GoBack prunes a stale admitted entry and keeps walking", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc2, "old");       // 0 -- will not restore
    w.Select(w.doc, "d1");         // 1
    w.Select(w.scene, "S");        // 2
    w.Select(w.doc, "d2");         // 3
    w.doc2.restoreOk = false;
    REQUIRE(w.host.GoBack(input)); // position 3 -> back 1 (d1) restores fine
    CHECK(w.doc.key == "d1");
    CHECK_FALSE(w.host.GoBack(input));   // only the stale doc2 entry is left: pruned, no landing
    CHECK(w.host.History().size() == 3);
}

namespace
{
    // The default two-Inspector layout over one shared history: instance 0 =
    // All but Assets, instance 1 = Assets only (spec s6).
    struct DefaultLayoutWorld
    {
        FakeSource scene{ "Scene", "scene" }, assets{ "Assets", "assets" };
        InspectorHost host{ scene };
        DefaultLayoutWorld() { host.AddSource(assets, /*permanent*/ true); host.ApplyDefaultInspectorLayout(); }
        void Select(FakeSource& s, std::string k) { s.key = std::move(k); host.NotifySelected(s); }
    };
}

TEST_CASE("InspectorHost history: Back in the main Inspector leaves the Assets instance's arrows on the asset it shows", "[editor][inspector]")
{
    // Final review I2/F1: [S1,A1,S2,A2], Back in the main Inspector lands S1;
    // Inspector 2 still SHOWS A2, so its Back is A1 and it has no Forward.
    DefaultLayoutWorld w;
    constexpr int kMain = 0, kAssets = InspectorHost::kAssetsInstanceId;
    w.Select(w.scene, "S1"); w.Select(w.assets, "A1"); w.Select(w.scene, "S2"); w.Select(w.assets, "A2");
    REQUIRE(w.host.GoBack(kMain));
    CHECK(w.scene.key == "S1");
    CHECK(w.assets.key == "A2");                                    // untouched: the asset model did not move
    REQUIRE(w.host.SourceFor(kAssets) == &w.assets);
    REQUIRE(w.host.BackEntry(kAssets) != nullptr);
    CHECK(w.host.BackEntry(kAssets)->key == "A1");
    CHECK(w.host.BackIndices(kAssets) == std::vector<std::size_t>{ 1 });
    CHECK_FALSE(w.host.CanGoForward(kAssets));
    CHECK(w.host.ForwardEntry(kAssets) == nullptr);
    CHECK(w.host.ForwardIndices(kAssets).empty());
    // The main Inspector is truthful too: it shows S1, Forward is S2.
    CHECK_FALSE(w.host.CanGoBack(kMain));
    REQUIRE(w.host.ForwardEntry(kMain) != nullptr);
    CHECK(w.host.ForwardEntry(kMain)->key == "S2");
}

TEST_CASE("InspectorHost history: Back in the Assets Inspector leaves the main Inspector's arrows on the entity it shows", "[editor][inspector]")
{
    // Final review I4: [a1,A,a2,B], Back in Inspector 2 lands a1; the main
    // Inspector still SHOWS B, so its Back is A and it has no Forward.
    DefaultLayoutWorld w;
    constexpr int kMain = 0, kAssets = InspectorHost::kAssetsInstanceId;
    w.Select(w.assets, "a1"); w.Select(w.scene, "A"); w.Select(w.assets, "a2"); w.Select(w.scene, "B");
    REQUIRE(w.host.GoBack(kAssets));
    CHECK(w.assets.key == "a1");
    CHECK(w.scene.key == "B");
    REQUIRE(w.host.SourceFor(kMain) == &w.scene);
    REQUIRE(w.host.BackEntry(kMain) != nullptr);
    CHECK(w.host.BackEntry(kMain)->key == "A");
    CHECK_FALSE(w.host.CanGoForward(kMain));
    CHECK(w.host.ForwardEntry(kMain) == nullptr);
    CHECK_FALSE(w.host.CanGoBack(kAssets));
    REQUIRE(w.host.ForwardEntry(kAssets) != nullptr);
    CHECK(w.host.ForwardEntry(kAssets)->key == "a2");
    // Back in the main Inspector now walks from B, not from the cursor.
    REQUIRE(w.host.GoBack(kMain));
    CHECK(w.scene.key == "A");
}

TEST_CASE("InspectorHost history: a filtered GoForward prunes a stale admitted entry and keeps walking", "[editor][inspector]")
{
    World w;
    const int input = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(input, InspectorFilter::Only("input-actions")));
    w.Select(w.doc, "d1");         // 0
    w.Select(w.scene, "S");        // 1
    w.Select(w.doc2, "gone");      // 2 -- will not restore
    w.Select(w.doc, "d3");         // 3
    REQUIRE(w.host.GoBack(input)); // d3 -> doc2 (still restorable here)
    CHECK(w.doc2.key == "gone");
    REQUIRE(w.host.GoBack(input)); // -> d1
    CHECK(w.doc.key == "d1");
    w.doc2.restoreOk = false;
    REQUIRE(w.host.GoForward(input));   // the stale doc2 entry is pruned on the way; lands d3
    CHECK(w.doc.key == "d3");
    CHECK(w.host.History().size() == 3);
    CHECK_FALSE(w.host.CanGoForward(input));
}

TEST_CASE("InspectorHost history: nothing admitted before the cursor means no Back (the sentinel case)", "[editor][inspector]")
{
    World w;
    const int scene = w.host.AddInstance();
    REQUIRE(w.host.SetFilter(scene, InspectorFilter::Only("scene")));
    w.Select(w.doc, "d1");
    w.Select(w.doc, "d2");
    CHECK(w.host.SourceFor(scene) == &w.scene);    // the fallback, nothing selected in it
    CHECK_FALSE(w.host.CanGoBack(scene));
    CHECK(w.host.BackEntry(scene) == nullptr);
    CHECK(w.host.BackIndices(scene).empty());
    CHECK_FALSE(w.host.CanGoForward(scene));
    CHECK_FALSE(w.host.GoBack(scene));
    CHECK(w.host.HistoryCursor() == 1);
}

TEST_CASE("Inspector header layout: one row when it fits; the breadcrumb wraps, then the combo collapses, the pin never clips", "[editor][inspector]")
{
    InspectorHeaderMetrics m;
    m.arrows = 56.0f; m.comboFull = 122.0f; m.comboIcon = 42.0f; m.pin = 24.0f; m.spacing = 8.0f;

    m.avail = 800.0f;                       // wide: arrows | combo | crumbs | pin
    InspectorHeaderLayout l = LayoutInspectorHeader(m);
    CHECK_FALSE(l.crumbsOwnRow);
    CHECK_FALSE(l.iconCombo);
    CHECK_FALSE(l.pinOnCrumbRow);
    CHECK(l.comboWidth == 122.0f);

    m.avail = 250.0f;                       // no room for 120 px of crumbs: own row, full combo
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK_FALSE(l.iconCombo);
    CHECK_FALSE(l.pinOnCrumbRow);
    CHECK(l.comboWidth == 122.0f);

    m.avail = 210.0f;                       // the labelled combo shrinks (ellipsized) so the pin still fits row 1
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK_FALSE(l.iconCombo);
    CHECK_FALSE(l.pinOnCrumbRow);
    CHECK(l.comboWidth <= 210.0f - 56.0f - 24.0f - 2.0f * 8.0f);

    m.avail = 180.0f;                       // below 200: icon-only combo
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK(l.iconCombo);
    CHECK(l.comboWidth == 42.0f);
    CHECK_FALSE(l.pinOnCrumbRow);

    m.avail = 120.0f;                       // even arrows + icon + pin overflow: the pin leads the crumb row
    l = LayoutInspectorHeader(m);
    CHECK(l.crumbsOwnRow);
    CHECK(l.iconCombo);
    CHECK(l.pinOnCrumbRow);
}

namespace
{
    // One device-less Inspector frame loop at a forced window width; returns
    // the primary window (after `frames` frames) for geometry probes.
    ImGuiWindow* DrawPrimaryAtWidth(InspectorHost& host, InspectorWindowsState& state, float width, int frames = 3)
    {
        for (int f = 0; f < frames; ++f)
        {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            (void)DrawInspectorWindows(host, state, nullptr);
            if (ImGuiWindow* w = ImGui::FindWindowByName(kPrimaryInspectorWindowId))
                ImGui::SetWindowSize(w, ImVec2(width, 400.0f), ImGuiCond_Always);
            ImGui::Render();
        }
        return ImGui::FindWindowByName(kPrimaryInspectorWindowId);
    }
    ImGuiWindow* CrumbsChildOf(ImGuiWindow* parent)
    {
        for (ImGuiWindow* c : ImGui::GetCurrentContext()->Windows)
            if (c->ParentWindow == parent && std::string(c->Name).find("##crumbs") != std::string::npos) return c;
        return nullptr;
    }
}

TEST_CASE("DrawInspectorWindows: a narrow Inspector wraps the breadcrumb to its own row and never overflows; a wide one keeps one row", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    FakeSource scene{ "Scene", "scene" };
    scene.page.crumbs = { { "Scene", {}, {} }, { "Player", {}, {} }, { "Camera", {}, {} } };
    scene.key = "7";
    InspectorHost host{ scene };
    REQUIRE(host.SetFilter(0, InspectorFilter::AllBut("assets")));   // the longest default label
    host.NotifySelected(scene);
    InspectorWindowsState state;

    SECTION("narrow (180 px)")
    {
        ImGuiWindow* w = DrawPrimaryAtWidth(host, state, 180.0f);
        REQUIRE(w != nullptr);
        ImGuiWindow* crumbs = CrumbsChildOf(w);
        REQUIRE(crumbs != nullptr);
        INFO("maxX " << w->DC.CursorMaxPos.x << " workMaxX " << w->WorkRect.Max.x);
        CHECK(w->DC.CursorMaxPos.x <= w->WorkRect.Max.x + 0.5f);          // nothing (the pin included) past the edge
        CHECK(crumbs->Pos.y > w->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f);   // row 2
        CHECK(crumbs->Size.x >= w->WorkRect.GetWidth() - ImGui::GetStyle().ItemSpacing.x - 1.0f);   // full width
    }
    SECTION("wide (800 px)")
    {
        ImGuiWindow* w = DrawPrimaryAtWidth(host, state, 800.0f);
        REQUIRE(w != nullptr);
        ImGuiWindow* crumbs = CrumbsChildOf(w);
        REQUIRE(crumbs != nullptr);
        CHECK(w->DC.CursorMaxPos.x <= w->WorkRect.Max.x + 0.5f);
        CHECK(crumbs->Pos.y < w->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f);   // row 1
    }
}

TEST_CASE("DrawInspectorWindows: a single-kind empty state names the kind in the singular", "[editor][inspector]")
{
    IniContext ic;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    REQUIRE(host.SetFilter(0, InspectorFilter::Only("material")));
    InspectorWindowsState state;
    ImGui::NewFrame();
    ImGui::LogToBuffer();
    (void)DrawInspectorWindows(host, state, nullptr);
    const std::string logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();
    INFO(logged);
    CHECK(logged.find("No Material document open") != std::string::npos);
    CHECK(logged.find("Materials document") == std::string::npos);
}

TEST_CASE("Default layout geometry: the user's ReferenceProject layout in pixels, clamped so the central node keeps 40%", "[editor][inspector]")
{
    // USER DECISION 2026-09-30: Inspector 380 w (full height, right), Outliner
    // 270 w, the bottom band 350 h (under the Outliner, left of the
    // Inspector). These three splits each have the central node on one side,
    // so ImGui keeps the OTHER side at its pixel size on resize.
    const DefaultLayoutPixels full = ComputeDefaultLayoutPixels(1920.0f, 954.0f);   // 1920x1080 maximized
    CHECK(full.inspector == 380.0f);
    CHECK(full.outliner == 270.0f);
    CHECK(full.bottomBand == 350.0f);

    const DefaultLayoutPixels boot = ComputeDefaultLayoutPixels(1280.0f, 647.0f);   // the hidden 1280x720 boot window
    CHECK(boot.inspector == 380.0f);                                  // 650 of 1280: the central node keeps 49%
    CHECK(boot.outliner == 270.0f);
    CHECK(boot.bottomBand == 350.0f);                                 // 297 of 647 left: 46%

    const DefaultLayoutPixels small = ComputeDefaultLayoutPixels(800.0f, 500.0f);
    CHECK(small.inspector + small.outliner <= 800.0f * 0.6f + 0.01f);   // central >= 40% of the width
    CHECK(std::abs(small.inspector / small.outliner - 380.0f / 270.0f) < 1e-5f);   // both shrink by one factor
    CHECK(small.bottomBand == 500.0f * 0.6f);                           // central >= 40% of the height
}

TEST_CASE("Default layout geometry: Inspector 2 splits the band by the user's 1920-scale PROPORTION, not a pixel target", "[editor][inspector]")
{
    // Integration residual 2a: the band's browser | Inspector 2 split has NO
    // central node on either side, so ImGui re-divides it by the children's
    // SizeRef RATIO on every resize (imgui.cpp DockNodeTreeUpdatePosSize,
    // rule 4). A 390 px target built at the 1280x720 boot size (506:390)
    // grew Inspector 2 to ~668 px maximized. The split is the user's own
    // saved proportion instead -- browser 1144 : Inspector 2 392 -- the same
    // at every build size, and exactly 392 px on a 1536 px band (1920x1080).
    CHECK(kDefaultBandBrowserRefPx == 1144.0f);
    CHECK(kDefaultAssetsInspectorRefPx == 392.0f);
    CHECK(kDefaultAssetsInspectorBandFraction == 392.0f / 1536.0f);
    CHECK(std::abs(1536.0f * kDefaultAssetsInspectorBandFraction - 392.0f) < 1e-3f);
    // Well inside the split clamps at any size (it used to need a 45% cap).
    CHECK(kDefaultAssetsInspectorBandFraction > 0.05f);
    CHECK(kDefaultAssetsInspectorBandFraction < 0.45f);
}

TEST_CASE("InspectorHost: a closed slot's filter comes back when Window > New Inspector reuses the slot", "[editor][inspector]")
{
    // Final review m2: closing "Inspector 2 - Assets" and opening a New
    // Inspector reused slot 1 UNFILTERED -- in the Assets dock slot its
    // [Window][inspector_1] entry still names -- so it followed every scene
    // and document selection. The host remembers each slot's last filter for
    // the session.
    FakeSource scene{ "Scene", "scene" };
    InspectorHost host{ scene };
    host.ApplyDefaultInspectorLayout();
    REQUIRE(host.Find(InspectorHost::kAssetsInstanceId)->filter == InspectorFilter::Only("assets"));
    host.RemoveInstance(InspectorHost::kAssetsInstanceId);
    const int reopened = host.AddInstance();
    REQUIRE(reopened == InspectorHost::kAssetsInstanceId);
    CHECK(host.Find(reopened)->filter == InspectorFilter::Only("assets"));
    // A slot never used this session opens All, as before.
    const int fresh = host.AddInstance();
    CHECK(host.Find(fresh)->filter.IsAll());
    // A filter change made before the close is what comes back.
    REQUIRE(host.SetFilter(fresh, InspectorFilter::Only("scene")));
    host.RemoveInstance(fresh);
    CHECK(host.Find(host.AddInstance())->filter == InspectorFilter::Only("scene"));
}
