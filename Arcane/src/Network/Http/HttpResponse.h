#pragma once

#include "HttpHeader.h"

namespace ARC::Network
{
   class HttpConnection;
   class HttpClient;
   
   class HttpResponse
   {
   public:
      HttpResponse() = default;

      int32_t GetStatusCode() const { return m_statusCode; }
      const HttpHeaders& GetHeaders() const { return m_headers; }
      const std::vector<uint8_t>& GetBody() const { return m_body; }
      std::string GetBodyAsString() const { return std::string(m_body.begin(), m_body.end()); }

      bool IsSuccessful() const { return m_statusCode >= 200 && m_statusCode < 300; }

   private:
      int32_t m_statusCode = 0;
      HttpHeaders m_headers;
      std::vector<uint8_t> m_body;

      friend class HttpConnection;
      friend class HttpClient;
   };
} // namespace ARC::Network
