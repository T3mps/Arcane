#include <Arcane/Crypto/Crypto.hpp>

namespace Arcane
{
#if !defined(ARC_BUILD_DIST)
    Crypto::Detail::PlatformFillFn& Crypto::Detail::PlatformFillOverrideSlot()
    {
        return SecureRandomDetail::PlatformFillOverrideSlot();
    }
#endif
}
