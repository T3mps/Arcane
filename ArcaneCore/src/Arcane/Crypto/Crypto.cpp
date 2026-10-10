#include <Arcane/Crypto/Crypto.hpp>

namespace Arcane
{
#if !defined(ARC_BUILD_DIST)
    Crypto::Detail::PlatformFillFn& Crypto::Detail::PlatformFillOverrideSlot()
    {
        // One slot for the process. Header-only static storage would be a
        // separate copy in every module, and Guid::Generate (this DLL)
        // would not observe a test's override.
        static PlatformFillFn slot = nullptr;
        return slot;
    }
#endif
}
