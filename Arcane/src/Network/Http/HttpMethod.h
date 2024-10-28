#pragma once

namespace ARC::Network
{
   enum class HttpMethod
   {
      Get,
      Post,
      Put,
      Delete,
      Head,
      Options,
      Patch
   };

   constexpr const char* ToString(HttpMethod method)
   {
      switch (method)
      {
         case HttpMethod::Get:     return "GET";
         case HttpMethod::Post:    return "POST";
         case HttpMethod::Put:     return "PUT";
         case HttpMethod::Delete:  return "DELETE";
         case HttpMethod::Head:    return "HEAD";
         case HttpMethod::Options: return "OPTIONS";
         case HttpMethod::Patch:   return "PATCH";
         default:                  return "GET";
      }
   }
} // namespace ARC::Network
