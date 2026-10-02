// Start page (spec 2026-09-30 s8.4): the pure model the window draws.
#include <catch2/catch_test_macros.hpp>
#include "Project/StartPageModel.hpp"
#include <ctime>

using namespace Arcane::Editor;

namespace
{
    constexpr std::uint64_t kNow = 1'700'000'000;
    std::string LocalDate(std::uint64_t unix)
    {
        std::tm tm{};
        const std::time_t t = static_cast<std::time_t>(unix);
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[16];
        std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
        return buf;
    }
    RecentProject P(std::string path, std::string name, std::uint64_t when)
    {
        RecentProject r; r.path = std::move(path); r.name = std::move(name); r.lastOpenedUnix = when; return r;
    }
    RecentSelection Sel(std::vector<RecentProject> visible, std::size_t hidden = 0)
    {
        RecentSelection s; s.visible = std::move(visible); s.hiddenForAbi = hidden; return s;
    }
}

TEST_CASE("start page: RelativeOpened bands, including 0 and a future stamp", "[editor]")
{
    CHECK(RelativeOpened(0, kNow).empty());
    CHECK(RelativeOpened(kNow + 500, kNow) == "opened just now");   // clock skew: never "in N minutes"
    CHECK(RelativeOpened(kNow, kNow) == "opened just now");
    CHECK(RelativeOpened(kNow - 59, kNow) == "opened just now");
    CHECK(RelativeOpened(kNow - 60, kNow) == "opened 1 minute ago");
    CHECK(RelativeOpened(kNow - 59 * 60, kNow) == "opened 59 minutes ago");
    CHECK(RelativeOpened(kNow - 3600, kNow) == "opened 1 hour ago");
    CHECK(RelativeOpened(kNow - 23 * 3600, kNow) == "opened 23 hours ago");
    CHECK(RelativeOpened(kNow - 24 * 3600, kNow) == "opened yesterday");
    CHECK(RelativeOpened(kNow - 47 * 3600, kNow) == "opened yesterday");
    CHECK(RelativeOpened(kNow - 48 * 3600, kNow) == "opened 2 days ago");
    CHECK(RelativeOpened(kNow - 29 * 86400, kNow) == "opened 29 days ago");
    const std::uint64_t old = kNow - 30 * 86400;
    CHECK(RelativeOpened(old, kNow) == "opened on " + LocalDate(old));
}

TEST_CASE("start page: the rows mirror the selection in order, and the hidden line has both plural forms", "[editor]")
{
    const StartPageModel m = BuildStartPage(Sel({ P("C:\\Games\\Alpha", "Alpha", kNow - 120), P("C:\\Games\\Beta\\Beta.arcproj", "Beta", 0) }, 1), kNow);
    REQUIRE(m.rows.size() == 2);
    CHECK(m.rows[0].name == "Alpha"); CHECK(m.rows[0].path == "C:\\Games\\Alpha"); CHECK(m.rows[0].opened == "opened 2 minutes ago");
    CHECK(m.rows[1].name == "Beta");  CHECK(m.rows[1].opened.empty());
    CHECK(m.hiddenLine == "1 project hidden (built for another engine version)");
    CHECK(BuildStartPage(Sel({}, 3), kNow).hiddenLine == "3 projects hidden (built for another engine version)");
    CHECK(BuildStartPage(Sel({}), kNow).hiddenLine.empty());
    CHECK(BuildStartPage(Sel({}), kNow).rows.empty());
}

TEST_CASE("start page: DialogStartDir is the parent of the first row's project folder", "[editor]")
{
    CHECK(DialogStartDir(Sel({})).empty());
    CHECK(DialogStartDir(Sel({ P("C:\\Games\\Alpha", "Alpha", 0) })) == "C:\\Games");                    // folder-shaped
    CHECK(DialogStartDir(Sel({ P("C:\\Games\\Alpha\\", "Alpha", 0) })) == "C:\\Games");                  // trailing separator
    CHECK(DialogStartDir(Sel({ P("C:\\Games\\Beta\\Beta.arcproj", "Beta", 0), P("D:\\x\\y", "y", 0) })) == "C:\\Games");   // .arcproj-shaped, first row wins
}
