// Crash window plan 1 (Task 3): the module table snapshot and the
// RtlVirtualUnwind portable stack, the two pieces the crash thread (Task 5)
// will use to print a stack without loading symbols in process. These cases
// run off the crash path -- Refresh/EnumerateProcessModules take the loader
// lock, which the crash filter itself never may -- proving the snapshot
// resolves an address to its module + offset, that a real capture of this
// thread names this process's own modules, and (fix round 1, review
// finding: a live Find() result can outlive TWO subsequent flips of the
// double buffer) that a frozen table refuses every Refresh -- no write, no
// flip -- exactly the way a live crash report needs it to.
//
// ModuleTable is process-global state and Catch2 runs these cases in random
// order alongside every other [diag] test in the binary, so each case here
// republishes whatever snapshot it needs at its own start and, where it
// changes the frozen flag, restores the live, unfrozen table before it
// returns -- never assume another case left the table the way you want it.

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

TEST_CASE("module table: Refresh is a no-op -- no write, no flip -- while frozen, and resumes once unfrozen", "[diag]")
{
    using namespace Arcane::Diagnostics;

    std::vector<Arcane::ForeignModules::LoadedModule> two(2);
    two[0].name = "A.dll"; two[0].path = "C:/x/A.dll"; two[0].base = 0x1000; two[0].size = 0x1000;
    two[1].name = "B.dll"; two[1].path = "C:/x/B.dll"; two[1].base = 0x5000; two[1].size = 0x100;
    ModuleTable::Refresh(two);
    REQUIRE(ModuleTable::Count() == 2);

    ModuleTable::SetFrozen(true);
    REQUIRE(ModuleTable::Frozen());

    // A report is "in progress": nothing published here may land while frozen.
    std::vector<Arcane::ForeignModules::LoadedModule> one(1);
    one[0].name = "C.dll"; one[0].path = "C:/x/C.dll"; one[0].base = 0x9000; one[0].size = 0x10;
    ModuleTable::Refresh(one);

    CHECK(ModuleTable::Count() == 2);              // unchanged: the Refresh above was a no-op
    CHECK(ModuleTable::Find(0x1234) != nullptr);   // A.dll's entry still resolves
    CHECK(ModuleTable::Find(0x50FF) != nullptr);   // B.dll's entry still resolves
    CHECK(ModuleTable::Find(0x9000) == nullptr);   // C.dll never got published

    ModuleTable::SetFrozen(false);
    REQUIRE_FALSE(ModuleTable::Frozen());

    ModuleTable::Refresh(one);
    CHECK(ModuleTable::Count() == 1);

    // Leave the table live and unfrozen: other [diag] cases run in random
    // order and expect the real process snapshot (see this file's header).
    ModuleTable::Refresh(Arcane::ForeignModules::EnumerateProcessModules());
}
