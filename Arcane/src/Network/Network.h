#pragma once

#include "Http/Http.h"

namespace ARC::Network 
{
   struct RequestConfig 
   {
      bool async = false;
      HttpOptions options{};
      HttpProgressCallback progress{};
      HttpCancelCallback cancel{};
      HttpHeaders headers{};
   };

   inline std::unique_ptr<HttpClient> s_client;
   inline HttpOptions s_defaultOptions;
   inline HttpHeaders s_defaultHeaders;
   inline std::mutex s_mutex;

   void Initialize();
   void Shutdown();

   Result<HttpResponse> Get(std::string_view url, const RequestConfig& config = {});
   Result<HttpResponse> Post(std::string_view url, const std::vector<uint8_t>& data, const RequestConfig& config = {});
   Result<HttpResponse> Put(std::string_view url, const std::vector<uint8_t>& data, const RequestConfig& config = {});
   Result<HttpResponse> Delete(std::string_view url, const RequestConfig& config = {});

   Result<std::string> GetString(std::string_view url, const RequestConfig& config = {});
   Result<HttpResponse> PostString(std::string_view url, std::string_view data, const RequestConfig& config = {});
   Result<HttpResponse> PutString(std::string_view url, std::string_view data, const RequestConfig& config = {});

   Result<void> Download(std::string_view url, const std::filesystem::path& destination,const RequestConfig& config = {});
   Result<std::vector<uint8_t>> DownloadToMemory(std::string_view url, const RequestConfig& config = {});

   Result<HttpResponse> SendRequest(const HttpRequest& request, const RequestConfig& config = {});

   static std::future<Result<HttpResponse>> GetAsync(std::string_view url, const RequestConfig& config = {});
   static std::future<Result<HttpResponse>> PostAsync(std::string_view url, const std::vector<uint8_t>& data, const RequestConfig& config = {});
   static std::future<Result<void>> DownloadAsync(std::string_view url, const std::filesystem::path& destination, const RequestConfig& config = {});

   void SetDefaultOptions(const HttpOptions& options);
   void SetDefaultHeaders(const HttpHeaders& headers);
} // namespace ARC::Network
