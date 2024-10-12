#pragma once

#include <source_location>
#include <string>

#include "Level.h"

namespace Arcane
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
