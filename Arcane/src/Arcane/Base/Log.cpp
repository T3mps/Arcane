#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>

#include <Mosaic/Log.hpp>

#include <mutex>

namespace
{
    spdlog::level::level_enum ToSpd(Mosaic::LogLevel l) noexcept
    {
        switch (l)
        {
            case Mosaic::LogLevel::Trace:    return spdlog::level::trace;
            case Mosaic::LogLevel::Debug:    return spdlog::level::debug;
            case Mosaic::LogLevel::Info:     return spdlog::level::info;
            case Mosaic::LogLevel::Warn:     return spdlog::level::warn;
            case Mosaic::LogLevel::Error:    return spdlog::level::err;
            case Mosaic::LogLevel::Critical: return spdlog::level::critical;
            case Mosaic::LogLevel::Off:      return spdlog::level::off;
        }
        return spdlog::level::info;
    }

    // noexcept sink: forward into the engine logger. category + message are fmt
    // ARGUMENTS (literal format) so a stray {} in a message cannot fmt-inject.
    void MosaicLogSinkImpl(const Mosaic::LogRecord& r, void* /*user*/) noexcept
    {
        try
        {
            Arcane::Log::Engine()->log(
                spdlog::source_loc{r.location.file_name(),
                                   static_cast<int>(r.location.line()),
                                   r.location.function_name()},
                ToSpd(r.level), "[{}] {}", r.category, r.message);
        }
        catch (...) {}
    }
}

namespace Arcane::Log
{
    namespace
    {
        std::shared_ptr<spdlog::logger> s_engine;
        std::once_flag s_initOnce;
    }

    void Init(spdlog::level::level_enum level)
    {
        // call_once: safe under concurrent first-use via the ARC_* macros.
        // The first caller's level wins; later Init() calls are no-ops.
        std::call_once(s_initOnce, [level] {
            auto existing = spdlog::get("Arcane");
            // STDERR, not stdout. Hosts use stdout as a DATA channel --
            // `--print-engine-info` prints one line of JSON the Arcane Hub parses
            // to learn the plugin ABI. Sharing the stream meant one stray ARC_WARN
            // ahead of that line would break the Hub's "read one line" contract.
            // Diagnostics belong on stderr anyway; the editor's Console panel reads
            // the Mosaic sink, so it is unaffected by which stream this uses.
            s_engine = existing ? existing : spdlog::stderr_color_mt("Arcane");
            s_engine->set_level(level);
            s_engine->set_pattern("%^[%H:%M:%S.%e] [%n] [%l]%$ %v");
        });
    }

    void Shutdown()
    {
        if (s_engine)
        {
            spdlog::drop("Arcane");
            s_engine.reset();
        }
    }

    spdlog::logger* Engine()
    {
        Init();
        return s_engine.get();
    }

    Mosaic::LogSink MosaicSink() noexcept { return &MosaicLogSinkImpl; }
}
