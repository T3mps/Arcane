// The dbgeng session: a minidump in, a Symbolized out (crash window plan 2,
// task 5; spec §6 "Symbolization").
//
// OUT OF PROCESS is the whole point. Plan 1's in-process walk is module+offset
// only, deliberately -- resolving names on the crash thread means dbghelp,
// which means the loader lock and a heap the fault may already have corrupted.
// This runs in a separate, healthy process against a file, so it may allocate,
// block and fail freely.
//
// BLOCKING, and not boundedly so: symbol loading can sit on a symbol server or
// a dead network path for as long as that path takes. The caller runs it on a
// worker under the unattended deadline (ReporterMain.cpp) -- that deadline is
// the only thing bounding this function.
//
// Symbolizer.cpp is NOT source-compiled into ArcaneTests: it is the Win32/
// dbgeng half of the seam SymbolizedText.hpp describes.
#pragma once

#include "SymbolizedText.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace Arcane::Reporter
{
    struct SymbolizeOptions
    {
        std::string   symbolPath;                   // empty = every module's own directory + <reporter dir> + %_NT_SYMBOL_PATH%
        // D5, set iff symbolPath was given. R82: it means "consult the
        // caller's path AND NOTHING ELSE", which is wider than the embedded
        // PDB path alone -- both that (SYMOPT_IGNORE_CVREC) and dbghelp's
        // fallback to the module's own recorded directory
        // (SYMOPT_NO_IMAGE_SEARCH) are shut off. The second is not decoration:
        // without it the search walks back to the folder the PDB is sitting
        // in, and "PDBs hidden" hides nothing on the desk that built them.
        bool          ignoreCvRecord     = false;
        // R78: the walk stops at these, and the report SAYS SO when it does.
        std::uint32_t maxFramesPerThread = 64;
        std::uint32_t maxThreads         = 64;
        std::uint32_t waitForEventMs     = 30000;
    };

    [[nodiscard]] Symbolized SymbolizeDump(const std::filesystem::path& dmp, const SymbolizeOptions& opt);
}
