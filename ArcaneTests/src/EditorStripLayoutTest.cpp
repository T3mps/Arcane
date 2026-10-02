// Node page phase s6.5: the toolbar strip's right cluster -- status rightmost,
// the Problems chip (s8.2) left of it -- as pure arithmetic. Window-local x.
#include <catch2/catch_test_macros.hpp>
#include <Widgets/EditorWidgets.hpp>

using namespace Arcane::Editor;

TEST_CASE("LayoutStripCluster: wide -- status ends at the right edge, chip left of it at the gap", "[editor][strip]")
{
    const StripClusterLayout l = LayoutStripCluster(1000.0f, 1912.0f, 80.0f, 200.0f, 60.0f, 16.0f);
    CHECK(l.drawStatus);
    CHECK(l.statusBudget == 200.0f);
    CHECK(l.statusX + l.statusBudget == 1912.0f);
    CHECK(l.chipX == 1712.0f - 16.0f - 80.0f);
}

TEST_CASE("LayoutStripCluster: no chip reserves no gap", "[editor][strip]")
{
    const StripClusterLayout l = LayoutStripCluster(1000.0f, 1200.0f, 0.0f, 200.0f, 60.0f, 16.0f);
    CHECK(l.drawStatus);
    CHECK(l.statusBudget == 200.0f);   // a reserved gap would have left 184
    CHECK(l.statusX == 1000.0f);
}

TEST_CASE("LayoutStripCluster: narrow -- the chip keeps its width, the status shrinks, then drops", "[editor][strip]")
{
    const StripClusterLayout shrunk = LayoutStripCluster(1000.0f, 1250.0f, 80.0f, 200.0f, 60.0f, 16.0f);
    CHECK(shrunk.drawStatus);
    CHECK(shrunk.statusBudget == 154.0f);
    CHECK(shrunk.statusX == 1096.0f);
    CHECK(shrunk.chipX + 80.0f == shrunk.statusX - 16.0f);   // chip width intact

    const StripClusterLayout dropped = LayoutStripCluster(1000.0f, 1150.0f, 80.0f, 200.0f, 60.0f, 16.0f);
    CHECK_FALSE(dropped.drawStatus);                          // 54 px left < statusMinW 60
    CHECK(dropped.statusBudget == 0.0f);
    CHECK(dropped.chipX == 1150.0f - 80.0f);                  // the chip takes the edge
}

TEST_CASE("LayoutStripCluster: no x is ever below minX", "[editor][strip]")
{
    for (float right : { 900.0f, 1000.0f, 1050.0f, 1100.0f, 1300.0f })
    {
        INFO(right);
        const StripClusterLayout l = LayoutStripCluster(1000.0f, right, 80.0f, 200.0f, 60.0f, 16.0f);
        CHECK(l.chipX >= 1000.0f);
        CHECK(l.statusX >= 1000.0f);
        if (l.drawStatus) CHECK(l.statusBudget >= 60.0f);
    }
}

TEST_CASE("LayoutStripCluster: a status narrower than its elision floor still draws whole", "[editor][strip]")
{
    // "UI > 1": natural 40 px, under the 60 px "..." + chevron + "..." floor.
    const StripClusterLayout l = LayoutStripCluster(1000.0f, 1912.0f, 0.0f, 40.0f, 60.0f, 16.0f);
    CHECK(l.drawStatus);
    CHECK(l.statusBudget == 40.0f);
    CHECK(l.statusX == 1872.0f);

    // ...and is still dropped when even its natural width does not fit.
    const StripClusterLayout tight = LayoutStripCluster(1000.0f, 1030.0f, 0.0f, 40.0f, 60.0f, 16.0f);
    CHECK_FALSE(tight.drawStatus);
    CHECK(tight.statusBudget == 0.0f);
}
