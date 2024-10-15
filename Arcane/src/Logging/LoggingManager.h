#pragma once

#include "LoggingService.h"
#include "Util/Singleton.h"
#include "Util/StringUtil.h"

namespace ARC
{
   class LoggingManager : public Singleton<LoggingManager>
   {
   public:
      static constexpr const wchar_t* DEFAULT_CORE_LOGGER_NAME = L"ARCANE";
      static constexpr const wchar_t* DEFAULT_APPLICATION_LOGGER_NAME = L"APP";

      using LoggerPtr = std::unique_ptr<LoggingService, std::function<void(LoggingService*)>>;

      static LoggingService& GetCoreLogger() { return *(GetInstance().m_coreLogger); }

      template <typename Deleter = LoggerDeleter>
      static void SetCoreLogger(LoggingService* newLogger, Deleter deleter = Deleter())
      {
         GetInstance().m_coreLogger = LoggerPtr(newLogger, deleter);
      }

      static bool HasCoreLogger() { return GetInstance().m_coreLogger != nullptr; }

      static LoggingService& GetApplicationLogger() { return *(GetInstance().m_applicationLogger); }

      template <typename Deleter = LoggerDeleter>
      static void SetApplicationLogger(LoggingService* newLogger, Deleter deleter = Deleter())
      {
         GetInstance().m_applicationLogger = LoggerPtr(newLogger, deleter);
      }

      static bool HasApplicationLogger() { return GetInstance().m_applicationLogger != nullptr; }

      static void Shutdown();

   private:
      LoggingManager();
      virtual ~LoggingManager() = default;

      LoggingManager(const LoggingManager&) = delete;
      LoggingManager& operator=(const LoggingManager&) = delete;

      LoggerPtr m_coreLogger;
      LoggerPtr m_applicationLogger;

      friend class Singleton<LoggingManager>;
   };
}
