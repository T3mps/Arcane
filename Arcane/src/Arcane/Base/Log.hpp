#pragma once

// Engine logger (Base module): console-sink spdlog logger named "Arcane".
// Deliberately separate from Core's server-flavored Logger (which writes
// logs/gacha_server.log with Auth/Gacha/... categories). File sinks and
// per-module categories can grow here when the engine needs them.
// spdlog is header-only in this workspace: each module has its OWN spdlog
// registry. Consumers must reach this logger via Engine() / the ARC_*
// macros -- spdlog::get("Arcane") in another module returns null.

#include <Arcane/Base/Api.hpp>

#include <spdlog/spdlog.h>

namespace Arcane::Log
{
    ARCANE_API void Init(spdlog::level::level_enum level = spdlog::level::info);
    ARCANE_API void Shutdown();

    // Never returns null: lazily calls Init() with defaults if needed.
    ARCANE_API spdlog::logger* Engine();
}

#define ARC_TRACE(...)    ::Arcane::Log::Engine()->trace(__VA_ARGS__)
#define ARC_DEBUG(...)    ::Arcane::Log::Engine()->debug(__VA_ARGS__)
#define ARC_INFO(...)     ::Arcane::Log::Engine()->info(__VA_ARGS__)
#define ARC_WARN(...)     ::Arcane::Log::Engine()->warn(__VA_ARGS__)
#define ARC_ERROR(...)    ::Arcane::Log::Engine()->error(__VA_ARGS__)
#define ARC_CRITICAL(...) ::Arcane::Log::Engine()->critical(__VA_ARGS__)
