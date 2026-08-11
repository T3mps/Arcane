// Structured diagnostic seam: the exported publish/sink slot every engine
// producer routes through (Arcane::Diagnostics::SetSink/Publish/Clear).
// Sibling to DiagnosticsTest.cpp, which covers the crash/hang post-mortem
// capture module in the same header -- this file covers only the seam.
// CPU-only ([diagnostics]).

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>

namespace
{
    struct Capture
    {
        std::mutex                                                   mu;
        std::vector<std::pair<std::string, std::vector<Arcane::Diagnostic>>> calls;
    };

    void CaptureSink(std::string_view key, std::span<const Arcane::Diagnostic> diags, void* user)
    {
        auto* c = static_cast<Capture*>(user);
        std::lock_guard<std::mutex> lock(c->mu);
        c->calls.emplace_back(std::string(key),
                              std::vector<Arcane::Diagnostic>(diags.begin(), diags.end()));
    }

    Arcane::Diagnostic MakeDiag(std::string code, Arcane::DiagSeverity sev)
    {
        Arcane::Diagnostic d;
        d.severity = sev;
        d.scope    = Arcane::DiagScope::Scene;
        d.code     = std::move(code);
        d.message  = "message";
        return d;
    }
}

TEST_CASE("Diagnostics forwards a published set to the installed sink", "[diagnostics]")
{
    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    const Arcane::Diagnostic diags[] = { MakeDiag("a.b", Arcane::DiagSeverity::Error) };
    Arcane::Diagnostics::Publish("scene:test", diags);

    REQUIRE(cap.calls.size() == 1);
    CHECK(cap.calls[0].first == "scene:test");
    REQUIRE(cap.calls[0].second.size() == 1);
    CHECK(cap.calls[0].second[0].code == "a.b");
    CHECK(cap.calls[0].second[0].severity == Arcane::DiagSeverity::Error);

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
}

TEST_CASE("Diagnostics::Clear publishes an empty set for the key", "[diagnostics]")
{
    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    Arcane::Diagnostics::Clear("scene:test");

    REQUIRE(cap.calls.size() == 1);
    CHECK(cap.calls[0].first == "scene:test");
    CHECK(cap.calls[0].second.empty());

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
}

TEST_CASE("Diagnostics with no sink installed is a silent no-op", "[diagnostics]")
{
    Arcane::Diagnostics::SetSink(nullptr, nullptr);
    const Arcane::Diagnostic diags[] = { MakeDiag("a.b", Arcane::DiagSeverity::Warning) };
    CHECK_NOTHROW(Arcane::Diagnostics::Publish("k", diags));
}

TEST_CASE("Diagnostics::Publish is safe from multiple threads", "[diagnostics]")
{
    Capture cap;
    Arcane::Diagnostics::SetSink(&CaptureSink, &cap);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 50;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([t]
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                const Arcane::Diagnostic d[] = { MakeDiag("thread.diag", Arcane::DiagSeverity::Info) };
                Arcane::Diagnostics::Publish("worker:" + std::to_string(t), d);
            }
        });
    }
    for (std::thread& w : workers) w.join();

    CHECK(cap.calls.size() == static_cast<std::size_t>(kThreads * kPerThread));

    Arcane::Diagnostics::SetSink(nullptr, nullptr);
}
