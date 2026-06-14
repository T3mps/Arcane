#pragma once

// Test-only helper: locate + load a physics oracle fixture by name.
//
// The fixtures live at Arcane/Tests/data/physics_oracle/<name>.json and are
// copied beside ArcaneTests.exe at build time (premake postbuild {COPYDIR}).
// They hold full-precision reference outputs captured from the Lua physics
// engine (Client/src/tests/physics_oracle_capture). The C++ port asserts
// parity against these numbers within an f32 tolerance.
//
// Resolution order (so tests pass whether run from the exe dir or elsewhere):
//   1. data/physics_oracle/<name>   relative to the current working dir
//   2. <exe-dir>/data/physics_oracle/<name>
// nlohmann/json only -- this is a test TU, not the presentation-free Physics
// module.

#include <filesystem>
#include <fstream>
#include <string>

#include <Json.hpp>

namespace Arcane
{
    namespace Test
    {
        // Returns the directory containing the running test executable, or an
        // empty path if it cannot be determined. Uses the std::filesystem
        // canonical-of-argv fallback via current path heuristics; on Windows
        // the CWD is the exe dir for the harness anyway, so this is belt-and-
        // braces for ad-hoc runs.
        inline std::filesystem::path OracleDir()
        {
            namespace fs = std::filesystem;
            const fs::path rel = fs::path("data") / "physics_oracle";
            if (fs::exists(rel))
            {
                return rel;
            }
            // Fall back to the directory of the current path's parent search:
            // walk up from CWD looking for the data dir (covers running from a
            // build subfolder). Bounded to a few levels.
            fs::path probe = fs::current_path();
            for (int i = 0; i < 6; ++i)
            {
                const fs::path cand = probe / "data" / "physics_oracle";
                if (fs::exists(cand))
                {
                    return cand;
                }
                if (!probe.has_parent_path())
                {
                    break;
                }
                probe = probe.parent_path();
            }
            return rel; // best effort; loader will report the open failure
        }

        // Loads a named oracle fixture (e.g. "shapes" or "shapes.json").
        // Throws std::runtime_error if the file cannot be opened.
        inline nlohmann::json LoadOracle(const std::string& name)
        {
            std::string file = name;
            if (file.size() < 5 ||
                file.compare(file.size() - 5, 5, ".json") != 0)
            {
                file += ".json";
            }
            const std::filesystem::path path = OracleDir() / file;
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
            {
                throw std::runtime_error(
                    "physics oracle fixture not found: " + path.string());
            }
            nlohmann::json j;
            in >> j;
            return j;
        }

    } // namespace Test
} // namespace Arcane
