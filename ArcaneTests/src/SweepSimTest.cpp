// Settings arc S6-8: the fixed-step clock (sim.*), the dedicated server's tick
// (server.tickHz) and the two remaining job-system sizes (jobs.externalThreads,
// jobs.shaderCompileThreads). Every default is the pre-sweep literal, bit for
// bit; the frame clamp and the step cap are read live by both host frames.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Config/Bindings/JobsBinding.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SimSettings.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include "ServerConfig.hpp"

#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    // Drops this test's Code-rung value on one cvar however the case exits,
    // and leaves every other cvar's Code rung alone.
    struct ClearCodeRung
    {
        CVarHandle handle;
        ~ClearCodeRung()
        {
            CVarRegistry::Get().ClearRung(handle, SetBy::Code);
            CVarRegistry::Get().PublishImmediate();
        }
    };

    Server::ServerConfig::ParseOutcome ParseServer(std::vector<std::string> args)
    {
        args.insert(args.begin(), "ArcaneServer");
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        return Server::ServerConfig::Parse(static_cast<int>(argv.size()), argv.data());
    }
}

TEST_CASE("sweep: sim/server defaults are the pre-sweep literals", "[sweep][sim]")
{
    CHECK(Test::SameBits(SimSettings{}.fixedHz, 60.0));
    CHECK(SimSettings{}.maxStepsPerFrame == 5);
    CHECK(Test::SameBits(SimSettings{}.maxFrameDeltaSeconds, 0.25));
    CHECK(Test::SameBits(ServerSettings{}.tickHz, 60.0));
    CHECK(Test::SameBits(HostConfig{}.fixedDtSeconds, 1.0 / 60.0));        // the old literal, bit for bit
    CHECK(Test::SameBits(1.0 / (1.0 / SimSettings{}.fixedHz), 60.0));      // ServerApp's round trip stays exact
    CHECK(RunLoop::Config{}.maxStepsPerFrame == 5);
    Test::RequireDefault("sim.fixedHz", CVarValue::Float64(60.0));
    Test::RequireDefault("sim.maxStepsPerFrame", CVarValue::Int32(5));
    Test::RequireDefault("sim.maxFrameDeltaSeconds", CVarValue::Float64(0.25));
    Test::RequireDefault("server.tickHz", CVarValue::Float64(60.0));
    const auto e = CVarRegistry::Get().Explain("sim.maxFrameDeltaSeconds");
    REQUIRE(e);
    CHECK(HasFlag(e->flags, CVarFlags::Deterministic));

    CVarRegistry& reg = CVarRegistry::Get();
    const auto hz = reg.Explain("sim.fixedHz");
    REQUIRE(hz);
    CHECK(hz->apply == ApplyMode::NextWorld);
    CHECK(hz->audience == Audience::Game);
    const auto steps = reg.Explain("sim.maxStepsPerFrame");
    REQUIRE(steps);
    CHECK(steps->apply == ApplyMode::Live);
    CHECK(HasFlag(steps->flags, CVarFlags::Deterministic));
    CHECK(e->apply == ApplyMode::Live);
    const auto tick = reg.Explain("server.tickHz");
    REQUIRE(tick);
    CHECK(tick->audience == Audience::Server);
    CHECK(tick->scope == SettingScope::Project);
    CHECK(tick->apply == ApplyMode::Restart);
    CHECK(HasFlag(tick->flags, CVarFlags::Deterministic));
}

TEST_CASE("sweep: ClampFrameDelta reads sim.maxFrameDeltaSeconds", "[sweep][sim]")
{
    CHECK(ClampFrameDelta(1.0) == 0.25);
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("sim.maxFrameDeltaSeconds");
    ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Float64(0.5), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();
    CHECK(ClampFrameDelta(1.0) == 0.5);
    CHECK(ClampFrameDelta(0.1) == 0.1);
}

TEST_CASE("sweep: ApplySimStepCap is Live -- a RunLoop takes sim.maxStepsPerFrame on the next frame", "[sweep][sim]")
{
    Astra::Registry registry;
    SystemSchedulers schedulers(nullptr);   // sequential executor
    RunLoop loop(registry, schedulers, RunLoop::Config{ .fixedHz = 60.0, .maxStepsPerFrame = 2 });
    CHECK(loop.MaxStepsPerFrame() == 2);
    ApplySimStepCap(loop);
    CHECK(loop.MaxStepsPerFrame() == 5);   // the published default

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("sim.maxStepsPerFrame");
    ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();
    ApplySimStepCap(loop);
    CHECK(loop.MaxStepsPerFrame() == 1);
    int fixed = 0;
    loop.Advance(0.25, [&](double) { ++fixed; }, [](double, double) {});
    CHECK(fixed == 1);                     // the cap holds a 15-step hitch to one step
}

TEST_CASE("sweep: server.tickHz paces the dedicated server unless --fixed-dt is supplied", "[sweep][sim][server]")
{
    const auto byDefault = ParseServer({ "--project", "P" });
    REQUIRE(byDefault.config);
    CHECK_FALSE(byDefault.config->fixedDtSupplied);
    CHECK(Test::SameBits(byDefault.config->FixedHz(), 60.0));

    const auto flag = ParseServer({ "--project", "P", "--fixed-dt", "0.05" });
    REQUIRE(flag.config);
    CHECK(flag.config->fixedDtSupplied);
    CHECK(flag.config->FixedHz() == 1.0 / 0.05);

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("server.tickHz");
    ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Float64(30.0), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();
    CHECK(byDefault.config->FixedHz() == 30.0);
    CHECK(flag.config->FixedHz() == 1.0 / 0.05);   // the flag still wins
}

TEST_CASE("sweep: jobs.externalThreads / jobs.shaderCompileThreads defaults are today's", "[sweep][sim]")
{
    CHECK(JobsSettings{}.externalThreads == 0u);
    CHECK(JobsSettings{}.shaderCompileThreads == 1u);
    Test::RequireDefault("jobs.externalThreads", CVarValue::UInt32(0));
    Test::RequireDefault("jobs.shaderCompileThreads", CVarValue::UInt32(1));
    for (const char* name : { "jobs.externalThreads", "jobs.shaderCompileThreads" })
    {
        INFO(name);
        const auto e = CVarRegistry::Get().Explain(name);
        REQUIRE(e);
        CHECK(e->apply == ApplyMode::Restart);
        CHECK(HasFlag(e->flags, CVarFlags::Dev));
    }
}
