// OsShell's PURE halves ([editor]): the explorer /select argument, the
// ShellExecute return-code table, and the missing-path refusal every call
// makes before touching the shell. The ShellExecuteW calls themselves are
// desk-verify territory (IdeLaunch/RuntimeLaunch split).

#include <catch2/catch_test_macros.hpp>

#include <Project/OsShell.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <span>

using namespace Arcane::Editor;
using OsShell::ShellResult;

TEST_CASE("OsShell::ExplorerSelectArgs quotes the native path", "[editor]")
{
    CHECK(OsShell::ExplorerSelectArgs(std::filesystem::path(L"C:\\Game Projects\\My Game\\brick.png"))
          == L"/select,\"C:\\Game Projects\\My Game\\brick.png\"");
#ifdef _WIN32
    // Forward (and mixed) slashes become native backslashes.
    CHECK(OsShell::ExplorerSelectArgs(std::filesystem::path(L"C:/Game Projects/My Game/Content\\brick.png"))
          == L"/select,\"C:\\Game Projects\\My Game\\Content\\brick.png\"");
#endif
}

TEST_CASE("OsShell::ClassifyShellExecute maps ShellExecute's return codes", "[editor]")
{
    CHECK(OsShell::ClassifyShellExecute(33) == ShellResult::Ok);
    CHECK(OsShell::ClassifyShellExecute(42) == ShellResult::Ok);
    CHECK(OsShell::ClassifyShellExecute(2) == ShellResult::NotFound);    // SE_ERR_FNF
    CHECK(OsShell::ClassifyShellExecute(3) == ShellResult::NotFound);    // SE_ERR_PNF
    CHECK(OsShell::ClassifyShellExecute(31) == ShellResult::NoHandler);  // SE_ERR_NOASSOC
    CHECK(OsShell::ClassifyShellExecute(0) == ShellResult::Failed);
    CHECK(OsShell::ClassifyShellExecute(5) == ShellResult::Failed);      // SE_ERR_ACCESSDENIED
    CHECK(OsShell::ClassifyShellExecute(32) == ShellResult::Failed);     // "> 32" is success, 32 is not
}

TEST_CASE("OsShell refuses a missing path without touching the shell", "[editor]")
{
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "arcane-osshell-missing-7f3a" / "nope.txt";
    REQUIRE_FALSE(std::filesystem::exists(missing));
    CHECK(OsShell::ShellOpen(missing) == ShellResult::NotFound);
    CHECK(OsShell::ShowInExplorer(missing) == ShellResult::NotFound);
    CHECK(OsShell::OpenAsText(missing) == ShellResult::NotFound);
    CHECK(OsShell::ShellOpen({}) == ShellResult::NotFound);
}

TEST_CASE("OsShell::Describe words every result", "[editor]")
{
    for (const ShellResult r : { ShellResult::Ok, ShellResult::NotFound, ShellResult::NoHandler,
                                 ShellResult::Failed, ShellResult::Unsupported })
        CHECK_FALSE(OsShell::Describe(r).empty());
    CHECK(OsShell::Describe(ShellResult::NotFound) == "File not found on this machine");
}

TEST_CASE("OsShell::ShellRecycle refuses a batch with a missing file and recycles nothing", "[editor]")
{
    namespace fs = std::filesystem;
    const fs::path present = fs::temp_directory_path() / "arcane_osshell_recycle_present.txt";
    const fs::path missing = fs::temp_directory_path() / "arcane_osshell_recycle_missing.txt";
    std::error_code ec;
    fs::remove(missing, ec);
    std::ofstream(present, std::ios::binary) << "x";
    const std::array<fs::path, 2> files{ present, missing };
    const auto r = Arcane::Editor::OsShell::ShellRecycle(files, nullptr);
    CHECK_FALSE(r.ok);
    CHECK(fs::exists(present));
    CHECK(r.notRecycled.size() == 2);
    CHECK(r.message.find("arcane_osshell_recycle_missing.txt") != std::string::npos);
    fs::remove(present, ec);
}

// Touches the real Recycle Bin, so it is hidden ("[.]") and carries no [editor]
// tag: Catch2 still selects a hidden case when a positive filter names one of
// its tags, and "[editor]" is a routine run. No unfiltered, "[editor]" or
// "~[gpu]" run selects it; it runs on an explicit "[shell]" (the one-time desk
// run) or a name pattern that matches this case's own name.
TEST_CASE("OsShell::ShellRecycle moves a file to the Recycle Bin", "[shell][.]")
{
#ifdef _WIN32
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "arcane_osshell_recycle_me.txt";
    std::ofstream(file, std::ios::binary) << "recycle me";
    const auto r = Arcane::Editor::OsShell::ShellRecycle(std::span<const fs::path>(&file, 1), nullptr);
    CHECK(r.ok);
    CHECK(r.notRecycled.empty());
    CHECK(r.permanentlyDeleted.empty());   // %TEMP%'s volume has a bin
    CHECK_FALSE(fs::exists(file));
#else
    SKIP("the Recycle Bin is Windows-only");
#endif
}
