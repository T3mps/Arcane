// InspectorHost (inspector-ownership spec s5): the PURE routing -- which
// source's page an Inspector instance shows -- driven with fake sources, no
// ImGui. Every rule in spec s3.1/s3.3/s6 has a case here.
#include <catch2/catch_test_macros.hpp>
#include <Panels/InspectorHost.hpp>
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
        std::string key;
        bool restoreOk = true;
        std::string normalizeTo;            // non-empty: RestoreSelection lands on THIS key, not the asked one (a member died in between)
        std::vector<std::string> restored;
        FakePage page;
        explicit FakeSource(std::string n) : name(std::move(n)) {}
        std::string SourceName() const override { return name; }
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
        FakeSource scene{ "Scene" }, doc{ "Player.arcinput" }, other{ "mat.arcshader" };
        InspectorHost host{ scene };
        World() { host.AddSource(doc); host.AddSource(other); }
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
