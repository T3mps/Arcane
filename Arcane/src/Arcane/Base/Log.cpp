#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>

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
            s_engine = existing ? existing : spdlog::stdout_color_mt("Arcane");
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
}
