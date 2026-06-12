#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>

namespace Arcane::Log
{
    namespace
    {
        std::shared_ptr<spdlog::logger> s_engine;
    }

    void Init(spdlog::level::level_enum level)
    {
        if (s_engine)
            return;
        s_engine = spdlog::stdout_color_mt("Arcane");
        s_engine->set_level(level);
        s_engine->set_pattern("%^[%H:%M:%S.%e] [%n] [%l]%$ %v");
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
        if (!s_engine)
            Init();
        return s_engine.get();
    }
}
