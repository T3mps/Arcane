#pragma once

// Engine logger (Base module): console-sink spdlog logger named "Arcane".
// Deliberately separate from Core's Logger (Util/Logger.hpp), which serves
// engine-agnostic named-category logging for library code; this is the
// engine runtime's own console logger. File sinks and per-module categories
// can grow here when the engine needs them.
// spdlog is header-only in this workspace: each module has its OWN spdlog
// registry. Consumers must reach this logger via Engine() / the ARC_*
// macros -- spdlog::get("Arcane") in another module returns null.

#include <Arcane/Base/Api.hpp>

#include <spdlog/spdlog.h>

#include <Mosaic/Log.hpp>

namespace Arcane::Log
{
    ARCANE_API void Init(spdlog::level::level_enum level = spdlog::level::info);
    ARCANE_API void Shutdown();

    // Never returns null: lazily calls Init() with defaults if needed.
    ARCANE_API spdlog::logger* Engine();

    // Mosaic diagnostics: the log SINK that forwards Mosaic/Manifold2D/Astra
    // records into the engine logger (Engine()). Defined in Log.cpp so it lives
    // once, in Arcane.dll, routing every module's records to one spdlog instance.
    ARCANE_API Mosaic::LogSink MosaicSink() noexcept;

    // Install the sink into the CALLING module's Mosaic storage. Inline on
    // purpose: Mosaic's g_logSink is a per-module inline atomic, so each module
    // (Arcane.dll, Loom.exe, the plugin, tests) installs into its own copy.
    inline void InstallMosaicSink() noexcept { Mosaic::SetLogSink(MosaicSink(), nullptr); }
}

#define ARC_TRACE(...)    ::Arcane::Log::Engine()->trace(__VA_ARGS__)
#define ARC_DEBUG(...)    ::Arcane::Log::Engine()->debug(__VA_ARGS__)
#define ARC_INFO(...)     ::Arcane::Log::Engine()->info(__VA_ARGS__)
#define ARC_WARN(...)     ::Arcane::Log::Engine()->warn(__VA_ARGS__)
#define ARC_ERROR(...)    ::Arcane::Log::Engine()->error(__VA_ARGS__)
#define ARC_CRITICAL(...) ::Arcane::Log::Engine()->critical(__VA_ARGS__)
