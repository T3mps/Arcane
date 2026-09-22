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
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
    else if (die == "ensure")       { (void)ARC_ENSURE(false, "fixture ensure"); return 0; }
    else if (die == "terminate")    { throw std::runtime_error("fixture terminate"); }
    else if (die == "abort")        { std::abort(); }
    else if (die == "invalid-parameter") { char buf[4]; strcpy_s(buf, 4, "toolong"); }
    else if (die == "purecall")     { Derived d; (void)d; }   // the dtor's virtual call is pure
    else if (die == "stack-overflow") { return Recurse(0); }
    else if (die == "oom")          { std::vector<char*> keep; for (;;) keep.push_back(new char[1u << 30]); }
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
