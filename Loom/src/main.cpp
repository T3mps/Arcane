// Loom -- the thin entry point. Parses argv into a LoomConfig, constructs the
// Loom application object, and returns its Run() exit code. All engine boot,
// the frame loop, and the load-bearing teardown order now live in the Loom
// class (Loom.hpp/.cpp); main is just the wire-up.

#include <Arcane/Base/Log.hpp>
#include "LoomConfig.hpp"
#include "Loom.hpp"

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2
    Loom loom(*parsed.config);
    return loom.Run();
}
