#pragma once

// Restart editor (settings arc S3-14, spec s6.5): reopen the same project in
// a fresh editor once this one has shut down and released the project's
// lock. The relaunch carries --project only: passing --set again would pin
// over settings that should take effect on restart.

#include <filesystem>
#include <string>
#include <vector>

namespace Arcane::Editor::EditorRestart
{
    [[nodiscard]] std::vector<std::wstring> Args(const std::filesystem::path& projectRoot);
    [[nodiscard]] std::filesystem::path CurrentExe();
    [[nodiscard]] bool Spawn(const std::filesystem::path& exe, const std::vector<std::wstring>& args);
}
