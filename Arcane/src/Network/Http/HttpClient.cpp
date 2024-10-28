#include "arcpch.h"
#include "HttpClient.h"

#include "Util/StringUtil.h"

#ifdef ARC_PLATFORM_WINDOWS
   #include <winhttp.h>
   #pragma comment(lib, "winhttp.lib")
#endif

namespace ARC::Network
{
   HttpClient::HttpClient()
   {
#ifdef ARC_PLATFORM_WINDOWS
      DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
      DWORD proxySettings = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;

      DWORD accessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;

      if (GetLastError() == ERROR_INVALID_PARAMETER)
         accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;

      m_session = WinHttpOpen
      (
         L"Arcane/1.0",
         accessType,
         WINHTTP_NO_PROXY_NAME,
         WINHTTP_NO_PROXY_BYPASS,
         0
      );

      if (!m_session)
      {
         DWORD error = GetLastError();
         ARC_CORE_ERROR("Failed to initialize WinHTTP session. Error: 0x{:08X}", error);
         return;
      }

      if (!WinHttpSetOption(m_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
      {
         DWORD error = GetLastError();
         ARC_CORE_WARN("Failed to set session TLS protocols. Error: 0x{:08X}", error);
      }

      DWORD enableDecompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
      WinHttpSetOption(m_session, WINHTTP_OPTION_DECOMPRESSION, &enableDecompression, sizeof(enableDecompression));

      DWORD timeoutMs = 60000;
      WinHttpSetTimeouts(m_session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

      m_connectionPool = std::make_unique<HttpConnectionPool>(m_session);

      m_pipeline.AddRequestInterceptor([](HttpRequest& request) -> Result<void>
      {
         if (!std::any_of(request.GetHeaders().begin(), request.GetHeaders().end(), [](const HttpHeader& h)
            { return StringUtil::ToLower(h.name) == "user-agent"; }))
         {
            request.AddHeader("User-Agent", "Arcane/1.0");
         }
         return {};
      });
#endif
   }

   HttpClient::~HttpClient()
   {
#ifdef ARC_PLATFORM_WINDOWS
      if (m_session)
         WinHttpCloseHandle(static_cast<HINTERNET>(m_session));
#endif
   }

   Result<void> HttpClient::ParseURL(const std::string& url, std::string& host, uint16_t& port, bool& isSecure, std::wstring& path) const
   {
#ifdef ARC_PLATFORM_WINDOWS
      URL_COMPONENTS urlComps = { sizeof(URL_COMPONENTS) };
      urlComps.dwSchemeLength = (DWORD)-1;
      urlComps.dwHostNameLength = (DWORD)-1;
      urlComps.dwUrlPathLength = (DWORD)-1;
      urlComps.dwExtraInfoLength = (DWORD)-1;

      std::wstring wideUrl = StringUtil::ToWString(url);
      if (!WinHttpCrackUrl(wideUrl.c_str(), (DWORD)wideUrl.length(), 0, &urlComps))
         return ARC_MAKE_ERROR(ErrorCode::InvalidArgument, "Invalid URL format");

      host = StringUtil::ToString(std::wstring(urlComps.lpszHostName, urlComps.dwHostNameLength));
      port = urlComps.nPort;
      isSecure = urlComps.nScheme == INTERNET_SCHEME_HTTPS;

      path = std::wstring(urlComps.lpszUrlPath, urlComps.dwUrlPathLength);
      if (urlComps.dwExtraInfoLength)
         path += std::wstring(urlComps.lpszExtraInfo, urlComps.dwExtraInfoLength);

      return {};
#else
      return ARC_MAKE_ERROR(ErrorCode::NotImplemented, "Platform not supported");
#endif
   }

   Result<HttpResponse> HttpClient::Send(const HttpRequest& request, const HttpOptions& options,
      HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
#ifdef ARC_PLATFORM_WINDOWS
      if (!m_session)
         return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "HTTP client not initialized");

      HttpRequest processedRequest = request;
      auto pipelineResult = m_pipeline.ProcessRequest(processedRequest);
      if (!pipelineResult)
         return pipelineResult.GetError();

      std::string host;
      uint16_t port;
      bool isSecure;
      std::wstring path;
      auto parseResult = ParseURL(request.GetUrl(), host, port, isSecure, path);
      if (!parseResult)
         return parseResult.GetError();

      auto connection = m_connectionPool->AcquireConnection(host, port, isSecure, options);
      if (!connection)
         return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Failed to acquire connection");

      auto response = connection->SendRequest(processedRequest, options, progressCallback, cancelCallback);

      m_connectionPool->ReleaseConnection(connection);

      if (!response)
         return response;

      auto responseResult = m_pipeline.ProcessResponse(processedRequest, *response);
      if (!responseResult)
         return responseResult.GetError();

      return response;
#else
      return ARC_MAKE_ERROR(ErrorCode::NotImplemented, "Platform not supported");
#endif
   }

   Result<HttpResponse> HttpClient::Get(std::string_view url, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
      HttpRequest request(url);
      request.SetMethod(HttpMethod::Get);
      return Send(request, options, progressCallback, cancelCallback);
   }

   Result<HttpResponse> HttpClient::Post(std::string_view url, const std::vector<uint8_t>& data, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
      HttpRequest request(url);
      request.SetMethod(HttpMethod::Post);
      request.SetBody(data);
      return Send(request, options, progressCallback, cancelCallback);
   }

   Result<void> HttpClient::Download(std::string_view url, const std::filesystem::path& destination, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
      std::error_code ec;
      std::filesystem::create_directories(destination.parent_path(), ec);
      if (ec)
         return ARC_MAKE_ERROR(ErrorCode::OperationFailed, std::format("Failed to create directories: {}", ec.message()));

      auto response = Get(url, options, progressCallback, cancelCallback);
      if (!response)
         return response.GetError();

      std::ofstream file(destination, std::ios::binary);
      if (!file)
         return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Failed to create output file");

      file.write(reinterpret_cast<const char*>((*response).GetBody().data()), 
         response.GetValue().GetBody().size());

      return {};
   }

   std::future<Result<HttpResponse>> HttpClient::SendAsync(const HttpRequest& request, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
      return std::async(std::launch::async, [this, request, options, progressCallback, cancelCallback]()
      {
         return Send(request, options, progressCallback, cancelCallback);
      });
   }

   std::future<Result<HttpResponse>> HttpClient::GetAsync(std::string_view url, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
      return std::async(std::launch::async, [this, url = std::string(url), options, progressCallback, cancelCallback]()
      {
         return Get(url, options, progressCallback, cancelCallback);
      });
   }

   std::future<Result<HttpResponse>> HttpClient::PostAsync(std::string_view url, const std::vector<uint8_t>& data, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
      return std::async(std::launch::async, [this, url = std::string(url), data, options, progressCallback, cancelCallback]()
      {
         return Post(url, data, options, progressCallback, cancelCallback);
      });
   }

   std::future<Result<void>> HttpClient::DownloadAsync(std::string_view url, const std::filesystem::path& destination, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
   {
      return std::async(std::launch::async, [this, url = std::string(url), destination, options, progressCallback, cancelCallback]()
      {
         return Download(url, destination, options, progressCallback, cancelCallback);
      });
   }
} // namespace ARC::Network
