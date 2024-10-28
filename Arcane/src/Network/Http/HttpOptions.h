#pragma once

namespace ARC::Network
{
   struct HttpOptions
   {
      bool followRedirects = true;
      uint32_t timeoutSeconds = 30;
      uint32_t retryCount = 3;
      uint32_t retryDelayMs = 1000;
      bool verifySSL = true;
      size_t bufferSize = 8192;
   };
} // ARC::Networking
