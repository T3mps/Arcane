#include <Arcane/Util/LogServerSettings.hpp>

ARC_SETTINGS(Arcane::LogServerSettings);
ARC_SETTINGS(Arcane::LogServerFileSettings);

namespace Arcane
{
    LogServerSettings PublishedLogServerSettings()
    {
        return Settings<LogServerSettings>();
    }

    LogServerFileSettings PublishedLogServerFileSettings()
    {
        return Settings<LogServerFileSettings>();
    }
}
