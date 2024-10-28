#pragma once

namespace ARC::Network
{
   struct HttpHeader
   {
      std::string name;
      std::string value;

      HttpHeader() = default;
      HttpHeader(std::string_view n, std::string_view v) : name(n), value(v) {}
   };

   using HttpHeaders = std::vector<HttpHeader>;
} // namespace ARC::Network
