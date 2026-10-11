#pragma once

#include <Arcane/Core/Api.hpp>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <vector>

namespace Arcane
{
    // OS CSPRNG with a once-per-process entropy self-test. Zero bytes do not
    // touch the platform generator or its test latch.
    [[nodiscard]] ARC_CORE_API std::vector<std::uint8_t> SecureRandomBytes(std::size_t count);

    namespace SecureRandomDetail
    {
        using PlatformFillFn = bool (*)(std::uint8_t*, std::size_t);
#if !defined(ARC_BUILD_DIST)
        ARC_CORE_API PlatformFillFn& PlatformFillOverrideSlot();
#endif
        ARC_CORE_API bool ReadExactFromStream(std::istream& source, std::uint8_t* out, std::size_t count);
        ARC_CORE_API std::vector<std::uint8_t> DrawUnchecked(std::size_t count);
        ARC_CORE_API bool EntropySelfTest();
        ARC_CORE_API void EnsureEntropySelfTest();
    }
}
