#include <Arcane/Platform/SecureRandom.hpp>
#include <Arcane/Util/Logger.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace Arcane
{
    namespace SecureRandomDetail
    {
#if !defined(ARC_BUILD_DIST)
        PlatformFillFn& PlatformFillOverrideSlot()
        {
            static PlatformFillFn slot = nullptr;
            return slot;
        }
#endif
        bool ReadExactFromStream(std::istream& source, std::uint8_t* out, std::size_t count)
        {
            if (!source) return false;
            if (count == 0) return true;
            source.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count));
            return source.gcount() == static_cast<std::streamsize>(count);
        }

        static bool PlatformFill(std::uint8_t* dst, std::size_t n)
        {
#if !defined(ARC_BUILD_DIST)
            if (const PlatformFillFn overrideFill = PlatformFillOverrideSlot())
                return overrideFill(dst, n);
#endif
#ifdef _WIN32
            const NTSTATUS status = BCryptGenRandom(nullptr, dst, static_cast<ULONG>(n),
                                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            return BCRYPT_SUCCESS(status);
#else
            std::ifstream urandom("/dev/urandom", std::ios::binary);
            return ReadExactFromStream(urandom, dst, n);
#endif
        }

        std::vector<std::uint8_t> DrawUnchecked(std::size_t count)
        {
            if (count == 0) return {};
            std::vector<std::uint8_t> bytes(count);
#ifdef _WIN32
            const std::size_t maxChunk = std::numeric_limits<ULONG>::max();
            const char* failure = "Crypto: BCryptGenRandom failed";
#else
            const std::size_t maxChunk = std::numeric_limits<std::size_t>::max();
            const char* failure = "Crypto: /dev/urandom unavailable or short read";
#endif
            for (std::size_t offset = 0; offset < count;)
            {
                const std::size_t n = std::min(maxChunk, count - offset);
                if (!PlatformFill(bytes.data() + offset, n))
                {
                    LOG_CORE_CRITICAL("{} -- refusing to generate secrets", failure);
                    throw std::runtime_error(failure);
                }
                offset += n;
            }
            return bytes;
        }

        bool EntropySelfTest()
        {
            const auto a = DrawUnchecked(32);
            const auto b = DrawUnchecked(32);
            return !EntropySamplesDegenerate(a, b);
        }

        void EnsureEntropySelfTest()
        {
            static const bool passed = EntropySelfTest();
            if (!passed)
            {
                LOG_CORE_CRITICAL("Crypto: RNG entropy self-test failed -- refusing to generate secrets");
                throw std::runtime_error("Crypto: RNG entropy self-test failed");
            }
        }
    }

    std::vector<std::uint8_t> SecureRandomBytes(std::size_t count)
    {
        if (count == 0) return {};
        SecureRandomDetail::EnsureEntropySelfTest();
        return SecureRandomDetail::DrawUnchecked(count);
    }
}
