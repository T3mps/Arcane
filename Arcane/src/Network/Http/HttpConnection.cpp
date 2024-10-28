#include "arcpch.h"
#include "HttpConnection.h"

#ifdef ARC_PLATFORM_WINDOWS
   #define SECURITY_WIN32
   #include <winhttp.h>
   #include <security.h>
   #include <schannel.h>
   #pragma comment(lib, "winhttp.lib")
   #pragma comment(lib, "crypt32.lib")
   #pragma comment(lib, "secur32.lib")
#endif

ARC::Network::HttpConnection::HttpConnection(void* session, const std::string& host, uint16_t port, bool isSecure) :
   m_session(session),
   m_host(host),
   m_port(port),
   m_isSecure(isSecure)
{
   UpdateLastUsed();
}

ARC::Network::HttpConnection::~HttpConnection()
{
   Disconnect();
}

ARC::Result<void> ARC::Network::HttpConnection::Connect(const HttpOptions& options)
{
#ifdef ARC_PLATFORM_WINDOWS
   if (m_connected)
      return {};

   if (!m_session)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Invalid session handle");

   m_connection = WinHttpConnect(static_cast<HINTERNET>(m_session), StringUtil::ToWString(m_host).c_str(), m_port, 0);

   if (!m_connection)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Failed to create connection");

   m_connected = true;
   return {};
#else
   return ARC_MAKE_ERROR(ErrorCode::NotImplemented, "Platform not supported");
#endif
}

ARC::Result<void> ARC::Network::HttpConnection::Disconnect()
{
#ifdef ARC_PLATFORM_WINDOWS
   if (m_connection)
   {
      WinHttpCloseHandle(static_cast<HINTERNET>(m_connection));
      m_connection = nullptr;
   }
#endif
   m_connected = false;
   return {};
}

bool ARC::Network::HttpConnection::IsConnected() const
{
   return m_connected;
}

bool ARC::Network::HttpConnection::IsReusable() const
{
   return m_connected && (std::chrono::steady_clock::now() - m_lastUsed) < std::chrono::seconds(60);
}
void ARC::Network::HttpConnection::UpdateProgress(HttpProgress& progress, uint64_t bytesTransferred, uint64_t totalBytes, const std::chrono::steady_clock::time_point& startTime)
{
   auto now = std::chrono::steady_clock::now();
   progress.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
   progress.bytesTransferred = bytesTransferred;
   progress.totalBytes = totalBytes;

   if (totalBytes > 0)
      progress.progressPercentage = (static_cast<float>(bytesTransferred) / totalBytes) * 100.0f;

   if (progress.elapsed.count() > 0)
      progress.bytesPerSecond = static_cast<float>(bytesTransferred) / (static_cast<float>(progress.elapsed.count()) / 1000.0f);
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::HttpConnection::SendRequest(const HttpRequest& request, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback)
{
#ifdef ARC_PLATFORM_WINDOWS
   if (!m_connected || !m_connection)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Connection not established");

   UpdateLastUsed();

   std::wstring path;
   size_t pathStart = request.GetUrl().find(m_host);
   if (pathStart != std::string::npos)
   {
      pathStart += m_host.length();
      if (pathStart < request.GetUrl().length())
         path = StringUtil::ToWString(request.GetUrl().substr(pathStart));
   }

   if (path.empty())
      path = L"/";

   HINTERNET hRequest = WinHttpOpenRequest
   (
      static_cast<HINTERNET>(m_connection),
      StringUtil::ToWString(ToString(request.GetMethod())).c_str(),
      path.c_str(),
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      m_isSecure ? WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH : 0
   );

   if (!hRequest)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Failed to create request");

   std::unique_ptr<void, decltype(&WinHttpCloseHandle)> requestGuard(hRequest, WinHttpCloseHandle);

   DWORD timeout = options.timeoutSeconds * 1000;
   WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

   for (const auto& header : request.GetHeaders())
   {
      std::wstring headerStr = StringUtil::ToWString(header.name + ": " + header.value);
      WinHttpAddRequestHeaders(hRequest, headerStr.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
   }

   const auto& body = request.GetBody();
   if (!WinHttpSendRequest
   (
      hRequest,
      WINHTTP_NO_ADDITIONAL_HEADERS,
      0,
      body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
      body.empty() ? 0 : static_cast<DWORD>(body.size()),
      body.empty() ? 0 : static_cast<DWORD>(body.size()),
      0
   ))
   {
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Failed to send request");
   }

   if (!WinHttpReceiveResponse(hRequest, NULL))
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Failed to receive response");

   HttpResponse response;

   DWORD statusCode = 0;
   DWORD statusCodeSize = sizeof(statusCode);
   WinHttpQueryHeaders
   (
      hRequest,
      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX,
      &statusCode,
      &statusCodeSize,
      WINHTTP_NO_HEADER_INDEX
   );

   response.m_statusCode = statusCode;

   DWORD headerSize = 0;
   WinHttpQueryHeaders
   (
      hRequest,
      WINHTTP_QUERY_RAW_HEADERS_CRLF,
      WINHTTP_HEADER_NAME_BY_INDEX,
      NULL,
      &headerSize,
      WINHTTP_NO_HEADER_INDEX
   );

   if (headerSize > 0)
   {
      std::vector<wchar_t> headerBuffer(headerSize / sizeof(wchar_t));
      WinHttpQueryHeaders
      (
         hRequest,
         WINHTTP_QUERY_RAW_HEADERS_CRLF,
         WINHTTP_HEADER_NAME_BY_INDEX,
         headerBuffer.data(),
         &headerSize,
         WINHTTP_NO_HEADER_INDEX
      );

      std::wstring headers(headerBuffer.data());
      std::wstringstream stream(headers);
      std::wstring line;
      
      while (std::getline(stream, line))
      {
         if (line.empty() || line == L"\r")
            continue;

         size_t colonPos = line.find(L':');
         if (colonPos != std::wstring::npos)
         {
            std::wstring name = line.substr(0, colonPos);
            std::wstring value = line.substr(colonPos + 2);
            if (!value.empty() && value.back() == L'\r')
               value.pop_back();

            response.m_headers.emplace_back(StringUtil::ToString(name), StringUtil::ToString(value));
         }
      }
   }

   // Response body
   std::vector<uint8_t> buffer(options.bufferSize);
   DWORD bytesRead = 0;
   uint64_t totalBytesRead = 0;
   HttpProgress progress;

   auto startTime = std::chrono::steady_clock::now();
   do
   {
      if (cancelCallback && cancelCallback())
         return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Request cancelled");

      DWORD bytesAvailable = 0;
      if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable))
         break;

      if (bytesAvailable == 0)
         break;

      if (bytesAvailable > buffer.size())
         bytesAvailable = static_cast<DWORD>(buffer.size());

      if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead))
      {
         if (bytesRead > 0)
         {
            response.m_body.insert(response.m_body.end(), buffer.data(), buffer.data() + bytesRead);
            totalBytesRead += bytesRead;

            if (progressCallback)
            {
               UpdateProgress(progress, totalBytesRead, 0, startTime);
               progressCallback(progress);
            }
         }
      }
      else
      {
         break;
      }
   } while (bytesRead > 0);

   return response;
#else
   return ARC_MAKE_ERROR(ErrorCode::NotImplemented, "Platform not supported");
#endif
}

ARC::Network::HttpConnectionPool::HttpConnectionPool(void* session, size_t maxConnectionsPerHost, std::chrono::seconds maxIdleTime) :
   m_session(session),
   m_maxConnectionsPerHost(maxConnectionsPerHost),
   m_maxIdleTime(maxIdleTime)
{}

ARC::Network::HttpConnectionPool::~HttpConnectionPool()
{
   std::lock_guard<std::mutex> lock(m_mutex);
   m_connections.clear();
}

std::shared_ptr<ARC::Network::HttpConnection> ARC::Network::HttpConnectionPool::AcquireConnection(const std::string& host, uint16_t port, bool isSecure, const HttpOptions& options)
{
   std::lock_guard<std::mutex> lock(m_mutex);

   PoolKey key{host, port, isSecure};
   auto& connections = m_connections[key];

   RemoveIdleConnections(connections);

   for (auto& connection : connections)
   {
      if (connection.use_count() == 1 && connection->IsReusable())
      {
         connection->UpdateLastUsed();
         return connection;
      }
   }

   if (connections.size() < m_maxConnectionsPerHost)
   {
      auto newConn = std::make_shared<HttpConnection>(m_session, host, port, isSecure);
      auto result = newConn->Connect(options);
      if (!result)
         return nullptr;

      connections.push_back(newConn);
      return newConn;
   }

   return nullptr;
}

void ARC::Network::HttpConnectionPool::ReleaseConnection(std::shared_ptr<HttpConnection> connection)
{
   if (!connection)
      return;

   connection->UpdateLastUsed();
}

void ARC::Network::HttpConnectionPool::CleanupIdleConnections()
{
   std::lock_guard<std::mutex> lock(m_mutex);

   for (auto& [key, connections] : m_connections)
   {
      RemoveIdleConnections(connections);
   }

   for (auto it = m_connections.begin(); it != m_connections.end();)
   {
      if (it->second.empty())
      {
         it = m_connections.erase(it);
         continue;
      }
      ++it;
   }
}

void ARC::Network::HttpConnectionPool::RemoveIdleConnections(std::vector<std::shared_ptr<HttpConnection>>& connections)
{
   auto now = std::chrono::steady_clock::now();

   connections.erase(std::remove_if(connections.begin(), connections.end(), [this, now](const auto& conn)
   {
      return conn.use_count() == 1 && (now - conn->GetLastUsed() > m_maxIdleTime || !conn->IsReusable());
   }), connections.end());
}

void ARC::Network::HttpRequestPipeline::AddRequestInterceptor(RequestInterceptor interceptor)
{
   m_requestInterceptors.push_back(std::move(interceptor));
}

void ARC::Network::HttpRequestPipeline::AddResponseInterceptor(ResponseInterceptor interceptor)
{
   m_responseInterceptors.push_back(std::move(interceptor));
}

ARC::Result<void> ARC::Network::HttpRequestPipeline::ProcessRequest(HttpRequest& request) const
{
   for (const auto& interceptor : m_requestInterceptors)
   {
      auto result = interceptor(request);
      if (!result)
         return result;
   }
   return {};
}

ARC::Result<void> ARC::Network::HttpRequestPipeline::ProcessResponse(const HttpRequest& request, HttpResponse& response) const
{
   for (const auto& interceptor : m_responseInterceptors)
   {
      auto result = interceptor(request, response);
      if (!result)
         return result;
   }
   return {};
}
