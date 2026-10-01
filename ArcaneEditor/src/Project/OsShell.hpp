#pragma once

// OsShell: the editor's ONE route to "open this path with the OS" (node-page
// phase s4.6) -- the Asset Browser's Show in Explorer / Open as text, the crash
// document's sibling buttons, and later the crash viewer's Open buttons and the
// Problems routes. Same split as IdeLaunch/RuntimeLaunch: the PURE halves
// (argument quoting, return-code table, wording) are [editor]-tested; the
// ShellExecuteW calls are desk-verify. Every call refuses a missing path with
// NotFound BEFORE touching the shell; non-Windows builds return Unsupported.
// Callers own their WARN text and take the reason wording from Describe.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Arcane::Editor::OsShell
{
    enum class ShellResult : std::uint8_t { Ok, NotFound, NoHandler, Failed, Unsupported };

    ShellResult ShellOpen(const std::filesystem::path& path);        // the OS default handler ("open")
    ShellResult ShowInExplorer(const std::filesystem::path& path);   // explorer /select: the folder, item selected (files AND directories)
    ShellResult OpenAsText(const std::filesystem::path& path);       // "open"; no handler -> Open With ("openas")

    // ---- pure halves ([editor]-tested) ----
    [[nodiscard]] std::wstring ExplorerSelectArgs(const std::filesystem::path& path);   // /select,"<native path>"
    [[nodiscard]] ShellResult  ClassifyShellExecute(std::intptr_t code);                 // >32 Ok; 2,3 NotFound; 31 NoHandler; else Failed
    [[nodiscard]] std::string_view Describe(ShellResult r);                              // WARN / tooltip wording
}
