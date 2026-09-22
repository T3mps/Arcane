// Crash window plan 1 (Task 3): the module table snapshot and the
// RtlVirtualUnwind portable stack, the two pieces the crash thread (Task 5)
// will use to print a stack without loading symbols in process. Both cases
// run off the crash path -- Refresh/EnumerateProcessModules take the loader
// lock, which the crash filter itself never may -- proving the snapshot
// resolves an address to its module + offset, and that a real capture of
// this thread names this process's own modules.

#include <Arcane/Base/ModuleTable.hpp>
#include <Arcane/Base/PortableStack.hpp>
#include <Arcane/Base/ForeignModules.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <string>

TEST_CASE("module table: resolves an address to its module and offset from a snapshot, unknown addresses to null", "[diag]")
{
    using namespace Arcane::Diagnostics;
    std::vector<Arcane::ForeignModules::LoadedModule> mods(2);
    mods[0].name = "A.dll"; mods[0].path = "C:/x/A.dll"; mods[0].base = 0x1000; mods[0].size = 0x1000;
    mods[1].name = "B.dll"; mods[1].path = "C:/x/B.dll"; mods[1].base = 0x5000; mods[1].size = 0x100;
    ModuleTable::Refresh(mods);
    REQUIRE(ModuleTable::Count() == 2);
    const ModuleEntry* a = ModuleTable::Find(0x1234);
    REQUIRE(a != nullptr);
    CHECK(std::string(a->name) == "A.dll");
    CHECK(ModuleTable::Find(0x2000) == nullptr);   // one past A
    CHECK(ModuleTable::Find(0x50FF) != nullptr);
    CHECK(ModuleTable::Find(0x5100) == nullptr);

    StackFrame f{ 0x1234, a };
    std::array<char, 160> buf{};
    CHECK(FormatStackFrame(3, f, buf) == "  03  A.dll + 0x234 (base 0x0000000000001000)");
    StackFrame unknown{ 0x9999, nullptr };
    CHECK(FormatStackFrame(0, unknown, buf) == "  00  0x0000000000009999 <unloaded or unknown module>");
}

TEST_CASE("portable stack: captures this thread without DbgHelp and names this process's own modules", "[diag]")
{
    using namespace Arcane::Diagnostics;
    // The live table from the real process (off the crash path).
    ModuleTable::Refresh(Arcane::ForeignModules::EnumerateProcessModules());
    std::array<StackFrame, 64> frames{};
    const std::size_t n = CaptureCurrentStack(frames);
    REQUIRE(n >= 3);
    // At least one frame lives in ArcaneCore.dll (CaptureCurrentStack itself) or ArcaneTests.exe.
    bool named = false;
    for (std::size_t i = 0; i < n; ++i)
        if (frames[i].module && (std::string(frames[i].module->name).find("Arcane") != std::string::npos)) named = true;
    CHECK(named);
}
