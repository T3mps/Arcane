#include <Arcane/Base/Engine.hpp>
#include <Arcane/Version.hpp>

#include <string>

namespace Arcane
{
    const char* BuildInfo()
    {
        // Composed from Core's VersionString so the two can never diverge.
        static const std::string s_info = std::string(VersionString())
#if defined(ARCANE_DEBUG)
            + " [Debug]";
#elif defined(ARCANE_RELEASE)
            + " [Release]";
#else
            + " [Dist]";
#endif
        return s_info.c_str();
    }
}
