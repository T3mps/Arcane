// InspectorHost (inspector-ownership spec s5): the PURE routing -- which
// source's page an Inspector instance shows -- driven with fake sources, no
// ImGui. Every rule in spec s3.1/s3.3/s6 has a case here. The one ImGui case
// (last) is the instance list's ini section, on a bare context, no device.
#include <catch2/catch_test_macros.hpp>
#include <Panels/InspectorHost.hpp>
#include <Panels/InspectorWindows.hpp>   // RegisterInspectorInstancesSettings
#include <imgui.h>
#include <imgui_internal.h>   // ClearIniSettings (the windowed switch's reset)
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
        std::string normalizeTo;            // non-empty: RestoreSelection lands on THIS key, not the asked one (a member died in between)
        std::vector<std::string> restored;
        FakePage page;
        explicit FakeSource(std::string n, std::string k = "scene") : name(std::move(n)), kind(std::move(k)) {}
        std::string SourceName() const override { return name; }
        std::string_view Kind() const override { return kind; }
        InspectorPage* Page() override { return &page; }
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
