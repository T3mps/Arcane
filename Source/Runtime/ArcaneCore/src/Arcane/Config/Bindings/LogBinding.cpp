#include <Arcane/Config/Bindings/LogBinding.hpp>

#include <Arcane/Base/Log.hpp>

#include <algorithm>

ARC_SETTINGS(Arcane::LogSettings);

namespace Arcane
{
    void ApplyLogSettings(const LogSettings& settings)
    {
        const int level = std::clamp(settings.level, 0, 6);
        Log::SetLevel(static_cast<spdlog::level::level_enum>(level));
        Log::SetMosaicLevelEverywhere(static_cast<Mosaic::LogLevel>(level));
    }

    namespace
    {
        void OnLogLevelPublished(CVarHandle, void*)
        {
            ApplyLogSettings(Settings<LogSettings>());   // the block is swapped in before callbacks run
        }

        // ARC_SETTINGS above registered log.level in this TU (ordered), so it
        // exists here. Dist refuses the Dev cvar: no callback, and the logger
        // keeps Init's level, as before.
        const bool s_logLevelCallback = [] {
            CVarRegistry& reg = CVarRegistry::Get();
            const CVarHandle h = reg.Find("log.level");
            if (!h.IsStale())
                reg.AddCallback(h, &OnLogLevelPublished, nullptr);
            return true;
        }();
    }
}
