#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpOptions.h"
#include "HttpProgress.h"
#include "HttpConnection.h"

namespace ARC::Network
{
   class HttpClient
   {
   public:
      HttpClient();
      ~HttpClient();

      Result<HttpResponse> Send(const HttpRequest& request, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);
      Result<HttpResponse> Get(std::string_view url, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);
      Result<HttpResponse> Post(std::string_view url, const std::vector<uint8_t>& data, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);
      Result<void> Download(std::string_view url, const std::filesystem::path& destination, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);

      std::future<Result<HttpResponse>> SendAsync(const HttpRequest& request, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);
      std::future<Result<HttpResponse>> GetAsync(std::string_view url, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);
      std::future<Result<HttpResponse>> PostAsync(std::string_view url, const std::vector<uint8_t>& data, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);
      std::future<Result<void>> DownloadAsync(std::string_view url, const std::filesystem::path& destination, const HttpOptions& options = {}, HttpProgressCallback progressCallback = nullptr, HttpCancelCallback cancelCallback = nullptr);

   private:
      Result<void> ParseURL(const std::string& url, std::string& host, uint16_t& port, bool& isSecure, std::wstring& path) const;

#ifdef ARC_PLATFORM_WINDOWS
      void* /* HINTERNET */ m_session{nullptr};
      std::unique_ptr<HttpConnectionPool> m_connectionPool;
      HttpRequestPipeline m_pipeline;
#endif
   };
} // namespace ARC::Network
