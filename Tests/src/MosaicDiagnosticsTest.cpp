// Epic: Mosaic diagnostics integration. Verifies the Arcane log sink + assert
// handler route Mosaic records into the engine logger. CPU-only ([diag]).

#include <algorithm>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <spdlog/sinks/callback_sink.h>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Mosaic/Assert.hpp>
#include <Mosaic/Log.hpp>

namespace
{
    // Attach a capturing callback sink to the engine logger; returns it so the
    // caller can remove it afterward (keeps other tests clean).
    std::shared_ptr<spdlog::sinks::callback_sink_mt> AttachCapture(std::string& out)
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [&out](const spdlog::details::log_msg& m) { out.assign(m.payload.data(), m.payload.size()); });
        Arcane::Log::Engine()->sinks().push_back(cb);
        return cb;
    }
    void DetachCapture(const std::shared_ptr<spdlog::sinks::callback_sink_mt>& cb)
    {
        auto& sinks = Arcane::Log::Engine()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), cb), sinks.end());
    }
}

TEST_CASE("Mosaic log records route to the Arcane engine logger", "[diag]")
{
    std::string captured;
    auto cb = AttachCapture(captured);
    Arcane::Log::InstallMosaicSink();          // install into THIS module
    MOSAIC_LOG_WARN("diag-log-message");
    DetachCapture(cb);

    CHECK(captured.find("diag-log-message") != std::string::npos);
    CHECK(captured.find("Mosaic") != std::string::npos);   // default category rides in the line
}

TEST_CASE("Mosaic assert failures route through the Arcane handler (ENSURE, non-fatal)", "[diag]")
{
    std::string captured;
    auto cb = AttachCapture(captured);
    Arcane::Assert::InstallMosaicHandler();     // install into THIS module
    const bool ok = MOSAIC_ENSURE(false, "diag-ensure-msg");   // non-fatal, returns false
    DetachCapture(cb);

    CHECK_FALSE(ok);
    CHECK(captured.find("diag-ensure-msg") != std::string::npos);
    CHECK(captured.find("assertion failed") != std::string::npos);
}
