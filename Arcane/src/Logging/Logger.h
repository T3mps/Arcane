#pragma once

#include "Format.h"
#include "LoggingService.h"

namespace ARC
{
   class Logger : public LoggingService
   {
   public:
      static constexpr int DEFAULT_FLUSH_INTERVAL = 16;

      Logger(const std::wstring& name);
      virtual ~Logger() = default;

      void Log(Level level, const std::wstring& message, const std::source_location& location = std::source_location::current()) override;
      void SetLevel(Level level) override { m_level = level; }

   private:
      void OutputLog(const std::wstring& message) const;

      std::wstring m_name;
      Level m_level;
      Format m_format;
      int m_messageCount;
      const int m_flushInterval;
      bool m_colorizeMessages;
      mutable std::mutex m_mutex;
   };
}
