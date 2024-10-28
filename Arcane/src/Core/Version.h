#pragma once

#include "Common.h"

#define ARC_VERSION_MAJOR 0
#define ARC_VERSION_MINOR 4
#define ARC_VERSION_PATCH 2

#define ARC_VERSION ARC_EXPAND_MACRO(ARC_STRINGIFY_MACRO(ARC_VERSION_MAJOR) "." \
                    ARC_EXPAND_MACRO(ARC_STRINGIFY_MACRO(ARC_VERSION_MINOR) "." \
                    ARC_EXPAND_MACRO(ARC_STRINGIFY_MACRO(ARC_VERSION_PATCH)

namespace ARC
{
   struct Version
   {
      uint32_t major;
      uint32_t minor;
      uint32_t patch;

      constexpr Version(uint32_t maj = 0, uint32_t min = 0, uint32_t ptch = 0) :
         major(maj),
         minor(min),
         patch(ptch)
      {}

      constexpr auto operator<=>(const Version&) const = default;

      std::string ToString() const { return std::format("{}.{}.{}", major, minor, patch); }

      static constexpr Version Engine()
      {
         return Version(ARC_VERSION_MAJOR, ARC_VERSION_MINOR, ARC_VERSION_PATCH);
      }

      static Version FromFileVersion(uint32_t ms, uint32_t ls)
      {
         return Version
         (
            HIWORD(ms), // Major
            LOWORD(ms), // Minor
            HIWORD(ls)  // Patch
         );
      }
   };
}
