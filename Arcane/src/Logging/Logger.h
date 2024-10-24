#pragma once

#include "Container/ConcurrentQueue.h"
#include "Format.h"
#include "LoggingService.h"

namespace ARC
{
   class Logger : public LoggingService
   {
   public:
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
      std::condition_variable m_conditionVar;
      std::mutex m_queueMutex;
      std::atomic<bool> m_running;
      std::thread m_workerThread;
      std::mutex m_flushMutex;
   };
} // namespace ARC
