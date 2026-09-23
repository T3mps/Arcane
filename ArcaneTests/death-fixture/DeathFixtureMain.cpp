// ArcaneTests/death-fixture/DeathFixtureMain.cpp -- dies on request so the
// crash path is tested as a real process (spec S10). No engine beyond Core.
//
// R18 (controller notes): Mosaic is header-only with per-module statics, so
// this exe has its OWN assert handler / log sink slots -- installing them
// right after Log::Init mirrors the hosts (ArcaneRuntime/src/main.cpp:26-28)
// exactly. Without them a fixture ARC_ASSERT would fall through Mosaic's
// default handler straight to abort(), and a later task's "assert" case
// would observe kind `terminate` instead of `assert`.
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
    struct Base { virtual ~Base() { Call(); } virtual void Pure() = 0; void Call() { Pure(); } };
    struct Derived : Base { void Pure() override {} };
    volatile int g_sink = 0;
    int Recurse(int depth) { volatile char pad[4096]; pad[0] = static_cast<char>(depth); g_sink += pad[0]; return Recurse(depth + 1) + 1; }
}

int main(int argc, char** argv)
{
    std::string dir, die; int hangSeconds = 0; bool hangAtExit = false; unsigned exitSeconds = 3, hangThreshold = 2;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == "--dir") next(dir); else if (a == "--die") next(die);
        else if (a == "--hang") { std::string v; next(v); hangSeconds = std::atoi(v.c_str()); }
        else if (a == "--hang-at-exit") hangAtExit = true;
        else if (a == "--exit-seconds") { std::string v; next(v); exitSeconds = static_cast<unsigned>(std::atoi(v.c_str())); }
        else if (a == "--hang-seconds") { std::string v; next(v); hangThreshold = static_cast<unsigned>(std::atoi(v.c_str())); }
    }
    Arcane::Log::Init(spdlog::level::info);
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    Arcane::Diagnostics::Config cfg;
    cfg.appName = "DeathFixture"; cfg.dumpDir = dir; cfg.unattended = true; cfg.spawnReporter = false;
    cfg.hangSeconds = hangThreshold; cfg.exitSeconds = exitSeconds;
    Arcane::Diagnostics::Install(cfg);
    ARC_INFO("death fixture: mode {}", die);

    if (die == "av")                { int* p = nullptr; *p = 1; }
    else if (die == "assert")       { ARC_ASSERT(false, "fixture assert"); }
    // NOT `return 0` here: an ensure is the one mode that SURVIVES, so it must
    // leave by the ordinary exit below -- which calls Diagnostics::Shutdown().
    // Returning straight out of main skipped it and left the watchdog's
    // std::thread joinable at static destruction, whose destructor calls
    // std::terminate: the "survivable" mode died (and, before this task's
    // handlers existed, wedged unkillably on the CRT's abort box).
    else if (die == "ensure")       { (void)ARC_ENSURE(false, "fixture ensure"); }
    else if (die == "terminate")    { throw std::runtime_error("fixture terminate"); }
    else if (die == "abort")        { std::abort(); }
    else if (die == "invalid-parameter") { char buf[4]; strcpy_s(buf, 4, "toolong"); }
    else if (die == "purecall")     { Derived d; (void)d; }   // the dtor's virtual call is pure
    else if (die == "stack-overflow") { return Recurse(0); }
    else if (die == "oom")
    {
        // Controller ruling R19: ONE impossible allocation, not a loop. A
        // bounded loop commits real memory per iteration (Windows reserves
        // commit charge at allocation time, not on first touch), climbing
        // toward the machine's RAM+pagefile limit before failing -- slow,
        // unpredictable, and hostile to the desk/CI machine under a 30s test
        // cap. This size can never be satisfied, so operator new fails
        // immediately with std::bad_alloc (or std::bad_array_new_length,
        // which derives from it) -- deterministic and instant.
        //
        // The size is computed into a local first rather than written
        // directly as `new char[std::numeric_limits<std::size_t>::max() / 2]`
        // -- tested empirically on this toolchain (MSVC /MDd): with the size
        // as a manifest compile-time constant, the compiler silently elides
        // the whole allocation (the C++14 new-expression elision rule) since
        // `p` is never dereferenced -- no throw, no crash, exit 0 after a
        // ~1.4s stall. Routing the same value through a runtime local
        // defeats that elision and reliably throws in ~100-200ms; volatile
        // plus the use below additionally stop the read of `p` itself from
        // being optimized away.
        std::size_t n = std::numeric_limits<std::size_t>::max() / 2;
        volatile char* p = new char[n];
        g_sink += p ? 1 : 0;
    }
    if (hangSeconds > 0)
    {
        // Stop beating: the watchdog must report a hang and the process must stay alive.
        std::this_thread::sleep_for(std::chrono::seconds(hangSeconds));
        return 0;
    }
    // task 8: RequestCleanExit does not exist yet (controller notes R8).
    // if (hangAtExit)
    // {
    //     Arcane::Diagnostics::RequestCleanExit();
    //     for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));   // never exits on its own
    // }
    (void)hangAtExit;
    Arcane::Diagnostics::Shutdown();
    return 0;
}
