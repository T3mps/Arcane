#pragma once

// Arcane::Guid -- a 128-bit globally-unique identifier (RFC-4122 UUID layout). The
// stable identity for assets (the AssetRegistry keys content by Guid). Trivially
// copyable + hashable + comparable; canonical lowercase 8-4-4-4-12 hex string form.
// Generation is authoring/import-time only (random v4 by default, or a deterministic
// name-based variant for stable engine-built-in ids) -- NEVER on the deterministic sim
// path, so its non-determinism can't affect physics. Core primitive (zero game deps;
// promotable to Mosaic later). Generate()/FromName() live in Guid.cpp so this header
// stays free of the Crypto/CSPRNG include.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane
{
    struct Guid
    {
        std::uint64_t hi = 0;   // bytes 0..7  (big-endian in the string form)
        std::uint64_t lo = 0;   // bytes 8..15

        // All-zero == the nil GUID (an "unset" id).
        [[nodiscard]] constexpr bool IsNil()   const noexcept { return hi == 0 && lo == 0; }
        [[nodiscard]] constexpr bool IsValid() const noexcept { return !IsNil(); }
        [[nodiscard]] static constexpr Guid Nil() noexcept { return Guid{0, 0}; }

        // Canonical lowercase 8-4-4-4-12 hex (36 chars, no braces).
        [[nodiscard]] std::string ToString() const;
        // Parse the canonical form (case-insensitive; dashes/braces tolerated). nullopt
        // unless the string carries exactly 32 hex digits.
        [[nodiscard]] static std::optional<Guid> FromString(std::string_view s);

        // Fresh random identifier (RFC-4122 v4). Draws from the Core CSPRNG. Thread-safe.
        [[nodiscard]] static Guid Generate();
        // Deterministic name-based identifier: the same (namespace, name) always yields
        // the same GUID -- for stable engine-built-in-content ids / reproducible import.
        [[nodiscard]] static Guid FromName(const Guid& ns, std::string_view name);

        constexpr bool operator==(const Guid& o) const noexcept { return hi == o.hi && lo == o.lo; }
        constexpr bool operator!=(const Guid& o) const noexcept { return !(*this == o); }
        constexpr bool operator<(const Guid& o) const noexcept
        { return hi != o.hi ? hi < o.hi : lo < o.lo; }
    };
}

template <>
struct std::hash<Arcane::Guid>
{
    std::size_t operator()(const Arcane::Guid& g) const noexcept
    {
        std::size_t h = std::hash<std::uint64_t>{}(g.hi);
        h ^= std::hash<std::uint64_t>{}(g.lo) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};
