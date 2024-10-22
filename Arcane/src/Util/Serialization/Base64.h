#pragma once

namespace ARC::Base64
{
   std::string Encode(const std::string& string);
   std::string Encode(std::span<const unsigned char> bytes);
   std::string Decode(const std::string& string);
} // namespace ARC::Base64
