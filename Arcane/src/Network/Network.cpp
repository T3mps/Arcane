#include "arcpch.h"
#include "Network.h"

void ARC::Network::Initialize()
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      s_client = std::make_unique<HttpClient>();
}

void ARC::Network::Shutdown()
{
   std::lock_guard<std::mutex> lock(s_mutex);
   s_client.reset();
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::Get(std::string_view url, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   HttpRequest request(url);
   request.SetMethod(HttpMethod::Get);

   for (const auto& header : s_defaultHeaders)
   {
      request.AddHeader(header.name, header.value);
   }

   for (const auto& header : config.headers)
   {
      request.AddHeader(header.name, header.value);
   }

   auto options = s_defaultOptions;
   options.bufferSize = config.options.bufferSize ? config.options.bufferSize : options.bufferSize;
   options.timeoutSeconds = config.options.timeoutSeconds ? config.options.timeoutSeconds : options.timeoutSeconds;
   options.retryCount = config.options.retryCount ? config.options.retryCount : options.retryCount;

   return config.async ? 
      s_client->SendAsync(request, options, config.progress, config.cancel).get() :
      s_client->Send(request, options, config.progress, config.cancel);
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::Post(std::string_view url, const std::vector<uint8_t>& data, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   HttpRequest request(url);
   request.SetMethod(HttpMethod::Post);
   request.SetBody(data);

   for (const auto& header : s_defaultHeaders)
   {
      request.AddHeader(header.name, header.value);
   }
   for (const auto& header : config.headers)
   {
      request.AddHeader(header.name, header.value);
   }

   auto options = s_defaultOptions;
   options.bufferSize = config.options.bufferSize ? config.options.bufferSize : options.bufferSize;
   options.timeoutSeconds = config.options.timeoutSeconds ? config.options.timeoutSeconds : options.timeoutSeconds;
   options.retryCount = config.options.retryCount ? config.options.retryCount : options.retryCount;

   return config.async ?
      s_client->SendAsync(request, options, config.progress, config.cancel).get() :
      s_client->Send(request, options, config.progress, config.cancel);
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::Put(std::string_view url, const std::vector<uint8_t>& data, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   HttpRequest request(url);
   request.SetMethod(HttpMethod::Put);
   request.SetBody(data);

   for (const auto& header : s_defaultHeaders)
   {
      request.AddHeader(header.name, header.value);
   }
   for (const auto& header : config.headers)
   {
      request.AddHeader(header.name, header.value);
   }

   auto options = s_defaultOptions;
   options.bufferSize = config.options.bufferSize ? config.options.bufferSize : options.bufferSize;
   options.timeoutSeconds = config.options.timeoutSeconds ? config.options.timeoutSeconds : options.timeoutSeconds;
   options.retryCount = config.options.retryCount ? config.options.retryCount : options.retryCount;

   return config.async ?
      s_client->SendAsync(request, options, config.progress, config.cancel).get() :
      s_client->Send(request, options, config.progress, config.cancel);
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::Delete(std::string_view url, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   HttpRequest request(url);
   request.SetMethod(HttpMethod::Delete);

   for (const auto& header : s_defaultHeaders)
   {
      request.AddHeader(header.name, header.value);
   }
   for (const auto& header : config.headers)
   {
      request.AddHeader(header.name, header.value);
   }

   auto options = s_defaultOptions;
   options.bufferSize = config.options.bufferSize ? config.options.bufferSize : options.bufferSize;
   options.timeoutSeconds = config.options.timeoutSeconds ? config.options.timeoutSeconds : options.timeoutSeconds;
   options.retryCount = config.options.retryCount ? config.options.retryCount : options.retryCount;

   return config.async ?
      s_client->SendAsync(request, options, config.progress, config.cancel).get() :
      s_client->Send(request, options, config.progress, config.cancel);
}

ARC::Result<std::string> ARC::Network::GetString(std::string_view url, const RequestConfig& config)
{
   auto response = Get(url, config);
   if (!response)
      return response.GetError();

   return (*response).GetBodyAsString();
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::PostString(std::string_view url, std::string_view data, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   HttpRequest request(url);
   request.SetMethod(HttpMethod::Post);
   request.SetBody(std::vector<uint8_t>(data.begin(), data.end()));

   for (const auto& header : s_defaultHeaders)
   {
      request.AddHeader(header.name, header.value);
   }
   for (const auto& header : config.headers)
   {
      request.AddHeader(header.name, header.value);
   }

   auto options = s_defaultOptions;
   options.bufferSize = config.options.bufferSize ? config.options.bufferSize : options.bufferSize;
   options.timeoutSeconds = config.options.timeoutSeconds ? config.options.timeoutSeconds : options.timeoutSeconds;
   options.retryCount = config.options.retryCount ? config.options.retryCount : options.retryCount;

   return config.async ?
      s_client->SendAsync(request, options, config.progress, config.cancel).get() :
      s_client->Send(request, options, config.progress, config.cancel);
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::PutString(std::string_view url, std::string_view data, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   HttpRequest request(url);
   request.SetMethod(HttpMethod::Put);
   request.SetBody(std::vector<uint8_t>(data.begin(), data.end()));

   for (const auto& header : s_defaultHeaders)
   {
      request.AddHeader(header.name, header.value);
   }
   for (const auto& header : config.headers)
   {
      request.AddHeader(header.name, header.value);
   }

   auto options = s_defaultOptions;
   options.bufferSize = config.options.bufferSize ? config.options.bufferSize : options.bufferSize;
   options.timeoutSeconds = config.options.timeoutSeconds ? config.options.timeoutSeconds : options.timeoutSeconds;
   options.retryCount = config.options.retryCount ? config.options.retryCount : options.retryCount;

   return config.async ?
      s_client->SendAsync(request, options, config.progress, config.cancel).get() :
      s_client->Send(request, options, config.progress, config.cancel);
}

ARC::Result<void> ARC::Network::Download(std::string_view url, const std::filesystem::path& destination, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   return config.async ?
      s_client->DownloadAsync(url, destination, config.options, config.progress, config.cancel).get() :
      s_client->Download(url, destination, config.options, config.progress, config.cancel);
}

ARC::Result<std::vector<uint8_t>> ARC::Network::DownloadToMemory(std::string_view url, const RequestConfig& config)
{
   auto response = Get(url, config);
   if (!response)
      return response.GetError();

   return (*response).GetBody();
}

ARC::Result<ARC::Network::HttpResponse> ARC::Network::SendRequest(const HttpRequest& request, const RequestConfig& config)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   if (!s_client)
      return ARC_MAKE_ERROR(ErrorCode::OperationFailed, "Network not initialized");

   auto options = s_defaultOptions;
   options.bufferSize = config.options.bufferSize ? config.options.bufferSize : options.bufferSize;
   options.timeoutSeconds = config.options.timeoutSeconds ? config.options.timeoutSeconds : options.timeoutSeconds;
   options.retryCount = config.options.retryCount ? config.options.retryCount : options.retryCount;

   return config.async ?
      s_client->SendAsync(request, options, config.progress, config.cancel).get() :
      s_client->Send(request, options, config.progress, config.cancel);
}

std::future<ARC::Result<ARC::Network::HttpResponse>> ARC::Network::GetAsync(std::string_view url, const RequestConfig& config)
{
   return std::async(std::launch::async, [url = std::string(url), config]()
   {
      return Get(url, config);
   });
}

std::future<ARC::Result<ARC::Network::HttpResponse>> ARC::Network::PostAsync(std::string_view url, const std::vector<uint8_t>& data, const RequestConfig& config)
{
   return std::async(std::launch::async, [url = std::string(url), data, config]()
   {
      return Post(url, data, config);
   });
}

std::future<ARC::Result<void>> ARC::Network::DownloadAsync(std::string_view url, const std::filesystem::path& destination, const RequestConfig& config)
{
   return std::async(std::launch::async, [url = std::string(url), destination, config]()
   {
      return Download(url, destination, config);
   });
}

void ARC::Network::SetDefaultOptions(const HttpOptions& options)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   s_defaultOptions = options;
}

void ARC::Network::SetDefaultHeaders(const HttpHeaders& headers)
{
   std::lock_guard<std::mutex> lock(s_mutex);
   s_defaultHeaders = headers;
}
