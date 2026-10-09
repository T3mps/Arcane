// Fuzz-only stand-in for the one Diagnostics entry point the scene loader
// calls (ArcaneCore/src/Arcane/Base/Diagnostics.cpp is Windows-first on `main`
// and pulls in the crash/minidump machinery). The harness only needs the call
// to exist; what was published does not matter to it.

#include <Arcane/Base/Diagnostics.hpp>

namespace Arcane::Diagnostics
{
    void Publish(std::string_view, std::span<const Diagnostic>) {}
}
