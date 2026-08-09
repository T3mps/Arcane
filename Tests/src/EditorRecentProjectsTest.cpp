// File -> Open Recent: the pure half of RecentProjects. CPU-only ([editor]).
//
// This unit reads AND WRITES a file the Arcane Hub owns, so the tests that
// matter most are the refusals -- the cases where the editor must leave the
// document alone rather than helpfully rewriting it. A bug here does not break
// a menu, it eats the user's project list.

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "RecentProjects.hpp"

using Arcane::Editor::RecentProject;
using Arcane::Editor::RecentSelection;
namespace Recents = Arcane::Editor::Recents;

namespace
{
    // Minimal stand-in for the Hub's document. Field names are the camelCase
    // serde writes (state.rs #[serde(rename_all = "camelCase")]).
    std::string Doc(const std::string& itemsJson, int version = 1)
    {
        std::ostringstream ss;
        ss << R"({"version":)" << version << R"(,"items":[)" << itemsJson << "]}";
        return ss.str();
    }

    std::string Entry(const std::string& path, const std::string& name, unsigned abi)
    {
        std::ostringstream ss;
        ss << R"({"path":")" << path << R"(","name":")" << name
           << R"(","lastOpenedUtc":"100","engineAbi":)" << abi
           << R"(,"engineId":null,"args":"","favorite":false,"missing":false,"guid":null})";
        return ss.str();
    }

    // Everything exists unless a test says otherwise.
    auto AlwaysExists() { return [](const std::string&) { return true; }; }

    std::string ReadWhole(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

TEST_CASE("Recents path keys mirror the Hub's normalisation", "[editor]")
{
    // Mirror of state.rs normalise_path / project_dir_key. If these drift, the
    // editor writes a SECOND row for a project the Hub already has -- a visible
    // duplicate in the user's Hub, which is why they are pinned here.
    CHECK(Recents::NormalisePath("C:\\Dev\\Proj\\") == "c:/dev/proj");
    CHECK(Recents::NormalisePath("C:/Dev/Proj") == "c:/dev/proj");

    // Both shapes a project can be recorded as collapse to the folder.
    CHECK(Recents::ProjectDirKey("D:/games/Aphelyon/Aphelyon.arcproj") == "d:/games/aphelyon");
    CHECK(Recents::ProjectDirKey("D:\\games\\Aphelyon") == "d:/games/aphelyon");
    CHECK(Recents::ProjectDirKey("D:/games/Aphelyon/Aphelyon.arcproj") ==
          Recents::ProjectDirKey("D:\\games\\Aphelyon\\"));
}

TEST_CASE("Recents parse reads entries in file order", "[editor]")
{
    const std::string doc = Doc(Entry("D:/a", "A", 9) + "," + Entry("D:/b", "B", 9));
    const std::vector<RecentProject> all = Recents::Parse(doc);

    REQUIRE(all.size() == 2);
    // The Hub keeps its list newest-first BY POSITION (touch_recent inserts at
    // the front), so file order IS the order to display. Re-sorting by
    // lastOpenedUtc would be wrong -- it is a decimal seconds STRING.
    CHECK(all[0].path == "D:/a");
    CHECK(all[1].path == "D:/b");
    CHECK(all[0].engineAbi == 9u);
}

TEST_CASE("Recents refuses documents it must not own", "[editor]")
{
    SECTION("a newer format reads empty")
    {
        // The Hub's own rule from the other side: a file from a newer Hub is
        // recognised, not misread as corruption.
        CHECK(Recents::Parse(Doc(Entry("D:/a", "A", 9), /*version*/ 2)).empty());
    }
    SECTION("malformed reads empty")
    {
        CHECK(Recents::Parse("{not json").empty());
        CHECK(Recents::Parse("[]").empty());       // envelope required
    }

    // The half that protects the user's data: Touch must return EMPTY (meaning
    // "do not write") rather than replacing a document it could not understand.
    // Quarantine is the Hub's recovery policy and it needs the original bytes.
    CHECK(Recents::Touch("{not json", "D:/a", "A", 9, 500).empty());
    CHECK(Recents::Touch(Doc(Entry("D:/a", "A", 9), 2), "D:/a", "A", 9, 500).empty());
}

TEST_CASE("Recents selection hides what this editor cannot open", "[editor]")
{
    const std::string doc = Doc(
        Entry("D:/match1", "M1", 9) + "," +
        Entry("D:/other",  "O",  8) + "," +      // another engine version
        Entry("D:/match2", "M2", 9));

    const RecentSelection sel =
        Recents::Select(Recents::Parse(doc), 9, /*current*/ "", AlwaysExists());

    REQUIRE(sel.visible.size() == 2);
    CHECK(sel.visible[0].path == "D:/match1");
    CHECK(sel.visible[1].path == "D:/match2");
    // Counted, and surfaced as a disabled menu line: the Hub lists every
    // project across every engine version, so a silently short list is
    // indistinguishable from a broken feature.
    CHECK(sel.hiddenForAbi == 1);
}

TEST_CASE("Recents selection drops the current project and missing paths", "[editor]")
{
    const std::string doc = Doc(
        Entry("D:/open/Open.arcproj", "Open", 9) + "," +
        Entry("D:/gone", "Gone", 9) + "," +
        Entry("D:/here", "Here", 9));

    const std::set<std::string> onDisk{"D:/open/Open.arcproj", "D:/here"};
    const RecentSelection sel = Recents::Select(
        Recents::Parse(doc), 9,
        "D:/open",                                    // folder form; entry is the .arcproj
        [&](const std::string& p) { return onDisk.count(p) != 0; });

    REQUIRE(sel.visible.size() == 1);
    CHECK(sel.visible[0].path == "D:/here");
    // Neither exclusion is reported: both are expected, and Unreal hides both
    // without comment (FRecentProjectsMenu::MakeMenu).
    CHECK(sel.hiddenForAbi == 0);
}

TEST_CASE("Recents selection caps the list", "[editor]")
{
    std::string items;
    for (int i = 0; i < 25; ++i)
    {
        if (i) items += ",";
        items += Entry("D:/p" + std::to_string(i), "P" + std::to_string(i), 9);
    }
    const RecentSelection sel =
        Recents::Select(Recents::Parse(Doc(items)), 9, "", AlwaysExists(), /*cap*/ 10);

    CHECK(sel.visible.size() == 10);
    CHECK(sel.visible.front().path == "D:/p0");   // newest-first survives the cap
}

TEST_CASE("Recents touch moves an existing entry to the front", "[editor]")
{
    const std::string doc = Doc(Entry("D:/a", "A", 9) + "," + Entry("D:/b", "B", 9));

    // Touch b via its .arcproj spelling: the dedup key must collapse it onto
    // the existing folder-shaped row instead of adding a second one.
    const std::string after = Recents::Touch(doc, "D:/b/B.arcproj", "B", 9, 777);
    REQUIRE_FALSE(after.empty());

    const std::vector<RecentProject> all = Recents::Parse(after);
    REQUIRE(all.size() == 2);          // moved, NOT duplicated
    CHECK(all[0].path == "D:/b");      // and the original spelling is kept
    CHECK(all[1].path == "D:/a");
    CHECK(after.find("\"777\"") != std::string::npos);   // lastOpenedUtc refreshed
}

TEST_CASE("Recents touch preserves fields the editor does not model", "[editor]")
{
    // The reason Touch mutates GENERIC json: engineId/args/favorite/guid are the
    // user's own settings, and a typed round-trip would silently reset every one
    // of them on the next project open.
    const std::string doc = Doc(
        R"({"path":"D:/a","name":"A","lastOpenedUtc":"1","engineAbi":9,)"
        R"("engineId":"eng-1","args":"--vulkan","favorite":true,"missing":false,)"
        R"("guid":"G-123","futureHubField":42})");

    const std::string after = Recents::Touch(doc, "D:/a", "A", 9, 900);
    REQUIRE_FALSE(after.empty());

    CHECK(after.find("eng-1") != std::string::npos);
    CHECK(after.find("--vulkan") != std::string::npos);
    CHECK(after.find("G-123") != std::string::npos);
    CHECK(after.find("\"favorite\": true") != std::string::npos);
    // Including one this build has never heard of -- a future Hub's field must
    // survive a round-trip through an older editor.
    CHECK(after.find("futureHubField") != std::string::npos);
}

TEST_CASE("Recents touch inserts a full row for an unknown project", "[editor]")
{
    // A row missing a non-defaulted field fails the Hub's typed parse, and ONE
    // bad row quarantines the user's whole list -- so a new entry must carry
    // the complete field set, not just what the editor cares about.
    const std::string after = Recents::Touch(Doc(""), "D:/new/New.arcproj", "New", 9, 55);
    REQUIRE_FALSE(after.empty());

    for (const char* field : {"path", "name", "lastOpenedUtc", "engineAbi",
                              "engineId", "args", "favorite", "missing", "guid"})
        CHECK(after.find(field) != std::string::npos);

    const std::vector<RecentProject> all = Recents::Parse(after);
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "New");
    CHECK(all[0].engineAbi == 9u);
}

TEST_CASE("Recents touch on an empty document starts a valid one", "[editor]")
{
    // First run: no file yet. That must produce a well-formed envelope rather
    // than a bare array the Hub would quarantine on its next read.
    const std::string after = Recents::Touch("", "D:/first", "First", 9, 1);
    REQUIRE_FALSE(after.empty());
    CHECK(after.find("\"version\": 1") != std::string::npos);

    const std::vector<RecentProject> all = Recents::Parse(after);
    REQUIRE(all.size() == 1);
    CHECK(all[0].path == "D:/first");
}

TEST_CASE("Recents atomic write round-trips and leaves no temp file", "[editor]")
{
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "arcane-recents-test";
    std::filesystem::remove_all(dir, ec);
    const std::filesystem::path file = dir / "recents.archub";

    const std::string text = Recents::Touch("", "D:/w", "W", 9, 42);
    REQUIRE_FALSE(text.empty());

    // Creates the directory on the way (first run has neither file nor folder).
    REQUIRE(Recents::WriteAtomic(file, text));
    REQUIRE(std::filesystem::exists(file));
    CHECK(ReadWhole(file) == text);
    CHECK(Recents::Load(file).size() == 1);

    // Overwrite in place must also leave the directory clean -- a stray
    // <name>.<pid>.tmp beside the Hub's own state file is litter in a folder
    // the editor does not own.
    const std::string text2 = Recents::Touch(text, "D:/w2", "W2", 9, 43);
    REQUIRE(Recents::WriteAtomic(file, text2));
    CHECK(Recents::Load(file).size() == 2);

    int strays = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".tmp") ++strays;
    CHECK(strays == 0);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("Recents load of a missing file is empty, not an error", "[editor]")
{
    std::error_code ec;
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path(ec) / "arcane-recents-does-not-exist.archub";
    std::filesystem::remove(missing, ec);

    CHECK(Recents::Load(missing).empty());
    CHECK(Recents::Load(std::filesystem::path()).empty());
}
