#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpOptions.h"
#include "HttpProgress.h"

namespace ARC::Network
{
   class HttpConnection
   {
   public:
      HttpConnection(void* session, const std::string& host, uint16_t port, bool isSecure);
      ~HttpConnection();

      Result<void> Connect(const HttpOptions& options);
      Result<void> Disconnect();
      bool IsConnected() const;
      bool IsReusable() const;

      const std::string& GetHost() const { return m_host; }
      uint16_t GetPort() const { return m_port; }

      Result<HttpResponse> SendRequest(const HttpRequest& request, const HttpOptions& options, HttpProgressCallback progressCallback, HttpCancelCallback cancelCallback);

      std::chrono::steady_clock::time_point GetLastUsed() const { return m_lastUsed; }
      void UpdateLastUsed() { m_lastUsed = std::chrono::steady_clock::now(); }

   private:
      static void UpdateProgress(HttpProgress& progress, uint64_t bytesTransferred, uint64_t totalBytes, const std::chrono::steady_clock::time_point& startTime);
      
      void* m_session; // HINTERNET session handle
      std::string m_host;
      uint16_t m_port;
      bool m_isSecure;
      bool m_connected = false;
      std::chrono::steady_clock::time_point m_lastUsed;

#ifdef ARC_PLATFORM_WINDOWS
      void* /* HINTERNET */ m_connection = nullptr;
#endif
   };

   class HttpConnectionPool
   {
   public:
      explicit HttpConnectionPool(void* session, size_t maxConnectionsPerHost = 8, std::chrono::seconds maxIdleTime = std::chrono::seconds(60));
      ~HttpConnectionPool();

      std::shared_ptr<HttpConnection> AcquireConnection(const std::string& host, uint16_t port, bool isSecure, const HttpOptions& options);
      void ReleaseConnection(std::shared_ptr<HttpConnection> connection);

      void SetMaxConnectionsPerHost(size_t count) { m_maxConnectionsPerHost = count; }
      void SetMaxIdleTime(std::chrono::seconds time) { m_maxIdleTime = time; }

      void CleanupIdleConnections();

   private:
      struct PoolKey 
      {
         std::string host;
         uint16_t port;
         bool isSecure;

         bool operator==(const PoolKey& other) const { return host == other.host && port == other.port && isSecure == other.isSecure; }
      };

      struct PoolKeyHash 
      {
         size_t operator()(const PoolKey& key) const { return std::hash<std::string>()(key.host) ^ std::hash<uint16_t>()(key.port) ^ std::hash<bool>()(key.isSecure); }
      };

      void* m_session = nullptr;
      std::mutex m_mutex;
      std::unordered_map<PoolKey, std::vector<std::shared_ptr<HttpConnection>>, PoolKeyHash> m_connections;
      size_t m_maxConnectionsPerHost;
      std::chrono::seconds m_maxIdleTime;

      void RemoveIdleConnections(std::vector<std::shared_ptr<HttpConnection>>& connections);
   };

   class HttpRequestPipeline
   {
   public:
      using RequestInterceptor = std::function<Result<void>(HttpRequest&)>;
      using ResponseInterceptor = std::function<Result<void>(const HttpRequest&, HttpResponse&)>;

      void AddRequestInterceptor(RequestInterceptor interceptor);
      void AddResponseInterceptor(ResponseInterceptor interceptor);

      Result<void> ProcessRequest(HttpRequest& request) const;
      Result<void> ProcessResponse(const HttpRequest& request, HttpResponse& response) const;

   private:
      std::vector<RequestInterceptor> m_requestInterceptors;
      std::vector<ResponseInterceptor> m_responseInterceptors;
   };
} // namespace ARC::Network
