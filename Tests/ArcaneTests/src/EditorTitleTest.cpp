// Node page phase s6.4: the editor title text, composed once. FormatOsTitle
// is today's EditorTitle byte for byte (literals traced from
// EditorApp.cpp:97-135 before the move); FormatStripStatus is the toolbar
// strip's plain text. Pure -- std only.
#include <catch2/catch_test_macros.hpp>
#include <App/EditorTitle.hpp>

using namespace Arcane::Editor;

TEST_CASE("FormatOsTitle reproduces the pre-s6.4 OS title exactly", "[editor][title]")
{
    CHECK(FormatOsTitle({ "Ref", "main", true },  "B", "dx12") == "Ref - main* - B <dx12>");
    CHECK(FormatOsTitle({ "", "Untitled", false }, "B", "dx12") == "Untitled - B <dx12>");
    CHECK(FormatOsTitle({ "Ref", "main", false }, "B", "")     == "Ref - main - B");
    CHECK(FormatOsTitle({ "Ref", "", true },      "B", "dx12") == "Ref - B <dx12>");
    CHECK(FormatOsTitle({ "", "", false },        "B", "dx12") == "B <dx12>");
}

TEST_CASE("FormatStripStatus: project > scene, a dirty star, and No project", "[editor][title]")
{
    CHECK(FormatStripStatus({ "Ref", "main", false })     == "Ref > main");
    CHECK(FormatStripStatus({ "Ref", "main", true })      == "Ref > main *");
    CHECK(FormatStripStatus({ "Ref", "Untitled", false }) == "Ref > Untitled");
    CHECK(FormatStripStatus({ "", "Untitled", false })    == "No project");
    CHECK(FormatStripStatus({ "", "Untitled", true })     == "No project");
}
