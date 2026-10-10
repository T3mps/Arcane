// Settings arc S3-14: Restart editor's pure half -- the relaunch argv
// reopens the same project and nothing else; the current exe resolves.
// Spawn is desk/automation-verified only (it starts a windowed editor).
#include <catch2/catch_test_macros.hpp>
#include <Settings/EditorRestart.hpp>

#include <filesystem>

TEST_CASE("EditorRestart::Args reopens the same project and nothing else", "[settings-ui]")
{
    const auto args = Arcane::Editor::EditorRestart::Args("D:/work/My Game");
    REQUIRE(args.size() == 2);
    CHECK(args[0] == L"--project");
    CHECK(std::filesystem::path(args[1]) == std::filesystem::path("D:/work/My Game"));
    CHECK(Arcane::Editor::EditorRestart::Args({}).empty());
    CHECK(Arcane::Editor::EditorRestart::CurrentExe().filename() == "ArcaneTests.exe");
}
