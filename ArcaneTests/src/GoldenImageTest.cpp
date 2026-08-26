// Golden-image / golden-layout verification (Task 10, plan-b comparator).
// This file is created HERE, ahead of Task 12 -- see that task's brief for
// what it appends.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("golden: the committed layout seed names windows this editor actually submits",
          "[golden]")
{
    // The placeholder failed here for weeks: every entry named a window that
    // does not exist, so LoadIniSettingsFromDisk applied nothing and the
    // "pinned layout" was really just BuildDefaultLayout every time. This is a
    // CHEAP, no-GPU guard against that exact regression.
    const std::filesystem::path seed =
        std::filesystem::path("ReferenceProject") / "Saved" / "verify-layout.ini";
    REQUIRE(std::filesystem::exists(seed));

    std::ifstream in(seed);
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    CHECK(text.find("[Window][EditorDockHost]") != std::string::npos);
    CHECK(text.find("DockSpaceViewport") == std::string::npos);   // the placeholder's ghost
}
