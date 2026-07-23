#include <Arcane/Guid.hpp>

#include <Arcane/Crypto/Crypto.hpp>   // Crypto::GenerateRandomBytes (Core CSPRNG)

#include "picosha2.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace Arcane
{
    namespace
    {
        // 16 big-endian bytes (byte 0 = MSB of hi, byte 8 = MSB of lo).
        std::array<std::uint8_t, 16> ToBytes(const Guid& g)
        {
            std::array<std::uint8_t, 16> b{};
            for (int i = 0; i < 8; ++i)
            {
                b[i]     = static_cast<std::uint8_t>((g.hi >> (56 - 8 * i)) & 0xFF);
                b[8 + i] = static_cast<std::uint8_t>((g.lo >> (56 - 8 * i)) & 0xFF);
            }
            return b;
        }

        Guid FromBytes(const std::array<std::uint8_t, 16>& b)
        {
            Guid g;
            for (int i = 0; i < 8; ++i)
            {
                g.hi = (g.hi << 8) | b[i];
                g.lo = (g.lo << 8) | b[8 + i];
            }
            return g;
        }

        // RFC-4122 version (high nibble of byte 6) + variant (top two bits of byte 8).
        void StampVersionVariant(std::array<std::uint8_t, 16>& b, std::uint8_t version)
        {
            b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | (version << 4));
            b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
        }

        int HexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }

    std::string Guid::ToString() const
    {
        static const char kHex[] = "0123456789abcdef";
        const auto b = ToBytes(*this);
        std::string s;
        s.reserve(36);
        auto emit = [&](int i)
        {
            s += kHex[(b[i] >> 4) & 0xF];
            s += kHex[b[i] & 0xF];
        };
        for (int i = 0;  i < 4;  ++i) emit(i);  s += '-';
        for (int i = 4;  i < 6;  ++i) emit(i);  s += '-';
        for (int i = 6;  i < 8;  ++i) emit(i);  s += '-';
        for (int i = 8;  i < 10; ++i) emit(i);  s += '-';
        for (int i = 10; i < 16; ++i) emit(i);
        return s;
    }

    std::optional<Guid> Guid::FromString(std::string_view s)
    {
        std::array<std::uint8_t, 16> b{};
        int nib = 0;   // nibble index 0..31
        for (char c : s)
        {
            if (c == '-' || c == '{' || c == '}')
                continue;
            const int v = HexVal(c);
            if (v < 0 || nib >= 32)
                return std::nullopt;   // non-hex char, or too many digits
            if ((nib & 1) == 0)
                b[nib / 2] = static_cast<std::uint8_t>(v << 4);
            else
                b[nib / 2] = static_cast<std::uint8_t>(b[nib / 2] | v);
            ++nib;
        }
        if (nib != 32)
            return std::nullopt;   // too few digits
        return FromBytes(b);
    }

    Guid Guid::Generate()
    {
        const std::vector<std::uint8_t> r = Crypto::GenerateRandomBytes(16);
        std::array<std::uint8_t, 16> b{};
        std::copy(r.begin(), r.end(), b.begin());
        StampVersionVariant(b, 4);   // RFC-4122 v4 (random)
        return FromBytes(b);
    }

    Guid Guid::FromName(const Guid& ns, std::string_view name)
    {
        // Deterministic: hash the namespace's 16 bytes || name. RFC-4122 v5 specifies
        // SHA-1; we use SHA-256 (already vendored via picosha2) truncated to 16 bytes --
        // internal-only, so the stronger-but-non-standard hash is fine. Stamp v5 so the
        // layout is well-formed.
        const auto nsb = ToBytes(ns);
        std::vector<std::uint8_t> msg(nsb.begin(), nsb.end());
        msg.insert(msg.end(), name.begin(), name.end());

        std::array<std::uint8_t, picosha2::k_digest_size> digest{};
        picosha2::hash256(msg.begin(), msg.end(), digest.begin(), digest.end());

        std::array<std::uint8_t, 16> b{};
        std::copy(digest.begin(), digest.begin() + 16, b.begin());
        StampVersionVariant(b, 5);   // name-based
        return FromBytes(b);
    }
}
