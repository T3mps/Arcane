#pragma once

#include "Level.h"

namespace ARC
{
   class LoggingService
   {
   public:
      virtual ~LoggingService() = default;
      virtual void Log(Level level, const std::wstring& message, const std::source_location& location = std::source_location::current()) = 0;
      virtual void SetLevel(Level level) = 0;
   };

   struct LoggerDeleter
   {
      void operator()(LoggingService* ptr) const { delete ptr; }
   };
}
