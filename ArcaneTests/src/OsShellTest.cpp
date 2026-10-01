// OsShell's PURE halves ([editor]): the explorer /select argument, the
// ShellExecute return-code table, and the missing-path refusal every call
// makes before touching the shell. The ShellExecuteW calls themselves are
// desk-verify territory (IdeLaunch/RuntimeLaunch split).

#include <catch2/catch_test_macros.hpp>

#include <Project/OsShell.hpp>

#include <filesystem>

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
