#pragma once

#include "tomlplusplus/toml.hpp"

namespace ARC
{
   namespace TOML
   {
      using Result = toml::parse_result;
      using Table = toml::table;

      using ParseError = toml::parse_error;

      inline auto Parse = [](const std::string_view fileName) -> Result { return toml::parse_file(fileName); };
   }
} // namespace ARC