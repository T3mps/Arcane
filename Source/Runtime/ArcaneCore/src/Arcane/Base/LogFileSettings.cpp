#include <Arcane/Base/LogFileSettings.hpp>

#include <Arcane/Base/Log.hpp>

#include <algorithm>

ARC_SETTINGS(Arcane::LogFileSettings);

namespace Arcane
{
    namespace
    {
        // log.file.flushLevel is Live. Before a file sink is attached the
        // engine logger keeps spdlog's default (no flush), as before the sweep;
        // AttachFileSink applies the setting when it attaches one.
        void OnFlushLevelPublished(CVarHandle, void*)
        {
            if (Log::FileSinkPath().empty())
                return;
            const int level = std::clamp(Settings<LogFileSettings>().flushLevel, 0, 6);
            Log::Engine()->flush_on(static_cast<spdlog::level::level_enum>(level));
        }

        // ARC_SETTINGS above registered the cvar in this TU (ordered). Dist
        // compiles the Dev cvar out: no callback, and the default applies.
        const bool s_flushLevelCallback = [] {
            CVarRegistry& reg = CVarRegistry::Get();
            const CVarHandle h = reg.Find("log.file.flushLevel");
            if (!h.IsStale())
                reg.AddCallback(h, &OnFlushLevelPublished, nullptr);
            return true;
        }();
    }
}
