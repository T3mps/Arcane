#include "arcpch.h"
#include "Hash.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// FNV1a
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

uint64_t ARC::Hash::FNV1aRuntime(const std::string& string) noexcept
{
   uint64_t hash = FNV_OFFSET_BASIS;
   for (char c : string)
   {
      hash ^= static_cast<uint64_t>(c);
      hash *= FNV_PRIME;
   }
   return hash;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// xxHash64
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

constexpr uint64_t ARC::Hash::xxHash64Round(uint64_t accumulator, uint64_t input)
{
   accumulator += input * XXH_PRIME64_2;
   accumulator = RotateLeft64(accumulator, 31);
   accumulator *= XXH_PRIME64_1;
   return accumulator;
}

constexpr uint64_t ARC::Hash::xxHash64Avalanche(uint64_t hash)
{
   hash ^= hash >> 33;
   hash *= XXH_PRIME64_2;
   hash ^= hash >> 29;
   hash *= XXH_PRIME64_3;
   hash ^= hash >> 32;
   return hash;
}

constexpr uint64_t ARC::Hash::xxHash64Finalize(uint64_t hash, const char* data, size_t length)
{
   while (length >= 8)
   {
      uint64_t k1 = 0;
      std::memcpy(&k1, data, sizeof(k1));
      hash ^= xxHash64Round(0, k1);
      hash = RotateLeft64(hash, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
      data += 8;
      length -= 8;
   }

   if (length >= 4)
   {
      uint32_t k1 = 0;
      std::memcpy(&k1, data, sizeof(k1));
      hash ^= static_cast<uint64_t>(k1) * XXH_PRIME64_1;
      hash = RotateLeft64(hash, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
      data += 4;
      length -= 4;
   }

   while (length > 0)
   {
      hash ^= static_cast<uint64_t>(*data) * XXH_PRIME64_5;
      hash = RotateLeft64(hash, 11) * XXH_PRIME64_1;
      ++data;
      --length;
   }

   return xxHash64Avalanche(hash);
}

constexpr uint64_t ARC::Hash::xxHash64(const char* data, size_t length, uint64_t seed)
{
   uint64_t hash = seed + XXH_PRIME64_5 + length;

   if (length >= 32)
   {
      uint64_t v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
      uint64_t v2 = seed + XXH_PRIME64_2;
      uint64_t v3 = seed;
      uint64_t v4 = seed - XXH_PRIME64_1;

      while (length >= 32)
      {
         v1 = xxHash64Round(v1, *reinterpret_cast<const uint64_t*>(data));
         data += 8;
         v2 = xxHash64Round(v2, *reinterpret_cast<const uint64_t*>(data));
         data += 8;
         v3 = xxHash64Round(v3, *reinterpret_cast<const uint64_t*>(data));
         data += 8;
         v4 = xxHash64Round(v4, *reinterpret_cast<const uint64_t*>(data));
         data += 8;
         length -= 32;
      }

      hash = RotateLeft64(v1, 1) + RotateLeft64(v2, 7) + RotateLeft64(v3, 12) + RotateLeft64(v4, 18);
   }

   return xxHash64Finalize(hash, data, length);
}
