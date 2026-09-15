#include <Arcane/Base/Assert.hpp>

#include <Arcane/Base/Log.hpp>   // Arcane::Log::Engine()

#include <spdlog/spdlog.h>

namespace
{
    // noexcept: log the failure Critical through the engine logger with the
    // stringized condition + file:line, then ask Mosaic to Break (FailFatal still
    // breaks-if-debugger then aborts). No "Mosaic" logger -- the engine logger owns it.
    Mosaic::AssertAction MosaicAssertHandlerImpl(const Mosaic::AssertContext& c, void* /*user*/) noexcept
    {
        try
        {
            Arcane::Log::Engine()->log(
                spdlog::source_loc{c.location.file_name(),
                                   static_cast<int>(c.location.line()),
                                   c.location.function_name()},
                spdlog::level::critical, "assertion failed: {}{}",
                c.expression ? c.expression : "<expr>",
                c.message ? std::string(" - ") + c.message : std::string());
        }
        catch (...) {}
        return Mosaic::AssertAction::Break;
    }
}

namespace Arcane::Assert
{
    Mosaic::AssertHandler MosaicHandler() noexcept { return &MosaicAssertHandlerImpl; }
}
