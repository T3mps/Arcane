#pragma once

// WHERE THE REPO'S REAL ReferenceProject/ LIVES, from inside a test exe.
//
// Lifted verbatim out of HostBootTest.cpp's anonymous namespace (F2c debts arc,
// Task E) so the thumbnail golden set resolves SOURCE content through the exact
// same walk the [host] cases already use, rather than a second, independently
// drifting copy of the same search. The reasoning below is that file's own,
// unchanged:
//
// No other test in this suite reaches into source-tree content (no SOURCE_DIR-
// style define, no fixture-copy convention to follow), so rather than hardcoding
// a fixed "../../.." depth this walks UP from the exe's own directory looking
// for the "ReferenceProject/ReferenceProject.arcproj" landmark. The premake
// layout (Arcane/bin/<cfg>-<os>-<arch>-md/<project>/) makes 3 levels the
// expected answer today, but verifying-by-search survives a future bin/ layout
// change instead of silently opening the wrong directory (or none) with no
// diagnostic. Bounded to 8 levels; empty on failure.
//
// NOTE WHICH TREE THIS IS. ArcaneTests' postbuild stages only Verify/,
// Intermediate/Artifacts and Saved/verify-layout.ini beside the exe -- it never
// stages ReferenceProject.arcproj -- so the landmark this looks for exists ONLY
// in the SOURCE tree, and the answer is therefore always the source checkout,
// never the staged copy. That is exactly what a Content/-reading case wants
// (the staged tree has no Content/ at all), and exactly what a golden BLESS
// wants (references are committed files).

#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8 (the argv[0] replacement)

#include <filesystem>
#include <system_error>

namespace Arcane::Test
{
    inline std::filesystem::path FindReferenceProjectDir()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = fs::path(Arcane::ExecutablePathUtf8()).parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            const fs::path candidate = dir / "ReferenceProject";
            if (fs::is_regular_file(candidate / "ReferenceProject.arcproj", ec))
                return candidate;
            const fs::path parent = dir.parent_path();
            if (parent == dir)
                break;
            dir = parent;
        }
        return {};
    }
}
