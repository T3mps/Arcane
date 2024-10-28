#pragma once

#include "HttpHeader.h"
#include "HttpMethod.h"

namespace ARC::Network
{
   class HttpRequest
   {
   public:
      HttpRequest() = default;
      explicit HttpRequest(std::string_view url) : m_url(url) {}

      void SetMethod(HttpMethod method) { m_method = method; }
      void SetUrl(std::string_view url) { m_url = url; }
      void AddHeader(std::string_view name, std::string_view value) { m_headers.emplace_back(name, value); }
      void SetBody(const std::vector<uint8_t>& body) { m_body = body; }
      void SetBody(std::string_view body) { m_body.assign(body.begin(), body.end()); }

      HttpMethod GetMethod() const { return m_method; }
      const std::string& GetUrl() const { return m_url; }
      const HttpHeaders& GetHeaders() const { return m_headers; }
      const std::vector<uint8_t>& GetBody() const { return m_body; }

   private:
      HttpMethod m_method = HttpMethod::Get;
      std::string m_url;
      HttpHeaders m_headers;
      std::vector<uint8_t> m_body;
   };
} // namespace ARC::Network
