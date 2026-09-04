#include "Arcane/AssetPipeline/CookKey.hpp"

namespace Arcane::AssetPipeline
{
    namespace
    {
        // FNV-1a 64-bit -- a small, deterministic-across-runs/platforms/toolchains hash. Chosen
        // over std::hash (implementation-defined, MUST NOT be used for anything that lands on
        // disk or is compared cross-process) and over reusing a struct memcpy (padding-RNG).
        constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
        constexpr std::uint64_t kFnvPrime       = 1099511628211ULL;

        // Feeds explicit fields one byte at a time, same discipline as ArtifactFormat's
        // ByteWriter -- never a struct/scalar memcpy wider than a byte.
        class Fnv1a64
        {
        public:
            void Update(std::span<const std::byte> data) noexcept
            {
                for (std::byte b : data)
                {
                    m_hash ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b));
                    m_hash *= kFnvPrime;
                }
            }

            void U8(std::uint8_t v) noexcept
            {
                const std::byte b = static_cast<std::byte>(v);
                Update(std::span<const std::byte>(&b, 1));
            }

            void U32(std::uint32_t v) noexcept
            {
                for (int i = 0; i < 4; ++i)
                    U8(static_cast<std::uint8_t>(v >> (8 * i)));
            }

            [[nodiscard]] std::uint64_t Digest() const noexcept { return m_hash; }

        private:
            std::uint64_t m_hash = kFnvOffsetBasis;
        };
    }

    std::uint64_t ComputeCookKey(std::span<const std::byte> sourceBytes, const TextureMetaSettings& settings,
                                  std::uint32_t importerVersion)
    {
        Fnv1a64 hasher;
        hasher.Update(sourceBytes);

        // Explicit fields only, in declaration order -- never a struct memcpy/reinterpret_cast.
        // When Task 3 grows TextureMetaSettings, every new field must be added HERE explicitly
        // too; the struct's memory layout is never a shortcut.
        hasher.U8(settings.srgb ? 1 : 0);
        hasher.U32(settings.maxSize);

        hasher.U32(importerVersion);

        return hasher.Digest();
    }
}
