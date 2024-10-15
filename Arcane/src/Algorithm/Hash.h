#pragma once

namespace ARC
{
   namespace Hash
   {
      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
      // FNV1a
      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
      constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325;
      constexpr uint64_t FNV_PRIME        = 0x100000001b3;

      constexpr uint64_t FNV1a(const char* string, size_t length, uint64_t hash = FNV_OFFSET_BASIS) noexcept
      {
         return (length == 0) ? hash : FNV1a(string + 1, length - 1, (hash ^ static_cast<uint64_t>(*string)) * FNV_PRIME);
      }

      constexpr uint64_t FNV1a(std::string_view string) noexcept
      {
         return FNV1a(string.data(), string.size());
      }

      uint64_t FNV1aRuntime(const std::string& string) noexcept;

      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
      // xxHash64
      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
      constexpr uint64_t XXH_PRIME64_1 = 0x9E3779B185EBCA87ULL;
      constexpr uint64_t XXH_PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
      constexpr uint64_t XXH_PRIME64_3 = 0x165667B19E3779F9ULL;
      constexpr uint64_t XXH_PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
      constexpr uint64_t XXH_PRIME64_5 = 0x27D4EB2F165667C5ULL;

      constexpr uint64_t RotateLeft64(uint64_t value, int shift) { return (value << shift) | (value >> (64 - shift)); }

      constexpr uint64_t xxHash64Round(uint64_t accumulator, uint64_t input);

      constexpr uint64_t xxHash64Avalanche(uint64_t hash);

      constexpr uint64_t xxHash64Finalize(uint64_t hash, const char* data, size_t length);

      constexpr uint64_t xxHash64(const char* data, size_t length, uint64_t seed = 0);

      uint64_t xxHash64Runtime(const std::string& data, uint64_t seed = 0) { return xxHash64(data.data(), data.size(), seed); }

      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
      // HashID
      ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

      struct HashID
      {
         uint64_t value;

         constexpr explicit HashID(std::string_view str, uint64_t seed = 0) : value(xxHash64(str.data(), str.size(), seed)) {}

         explicit HashID(const std::string& str, uint64_t seed = 0) : value(xxHash64Runtime(str, seed)) {}

         constexpr bool operator==(const HashID& other) const { return value == other.value; }

         friend std::ostream& operator<<(std::ostream& os, const HashID& id)
         {
            os << std::hex << id.value;
            return os;
         }
      };
   } // namespace Hash
} // namespace ARC
