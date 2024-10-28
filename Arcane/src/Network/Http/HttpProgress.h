#pragma once

namespace ARC::Network
{
   struct HttpProgress
   {
      uint64_t bytesTransferred = 0;
      uint64_t totalBytes = 0;
      float progressPercentage = 0.0f;
      float bytesPerSecond = 0.0f;
      std::chrono::milliseconds elapsed{0};
   };

   using HttpProgressCallback = std::function<void(const HttpProgress&)>;
   using HttpCancelCallback = std::function<bool(void)>;
} // namespace ARC::Network
