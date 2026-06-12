#include <Arcane/Base/Engine.hpp>
#include <Arcane/Version.hpp>

namespace Arcane
{
    const char* BuildInfo()
    {
#if defined(ARCANE_DEBUG)
        return "Arcane 0.1 (M1) [Debug]";
#elif defined(ARCANE_RELEASE)
        return "Arcane 0.1 (M1) [Release]";
#else
        return "Arcane 0.1 (M1) [Dist]";
#endif
    }
}
