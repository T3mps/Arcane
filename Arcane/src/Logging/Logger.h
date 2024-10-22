#pragma once

#include "Container/ConcurrentQueue.h"
#include "Format.h"
#include "LoggingService.h"

namespace ARC
{
   class Logger : public LoggingService
   {
   public:
      static constexpr int32_t DEFAULT_FLUSH_INTERVAL = 16;

      Logger(const std::string& name);
      virtual ~Logger();

      void Start();
      void Stop();

      void Log(Level level, const std::string& message, const std::source_location& location = std::source_location::current()) override;
      void SetLevel(Level level) override { m_level = level; }

   private:
      std::string BuildMessage(Level level, const std::string& message, const std::source_location& location);
      void ProcessLogs();
      void OutputLog(const std::string& message) const;
      void FlushStream() const;

      std::string m_name;
      Level m_level;
      Format m_format;
      bool m_colorizeMessages;

      ConcurrentQueue<std::string> m_logQueue;
      std::atomic<bool> m_running;
      std::thread m_workerThread;
      int32_t m_flushInterval;
      int32_t m_messageCount;
      std::mutex m_flushMutex;
   };
} // namespace ARC
