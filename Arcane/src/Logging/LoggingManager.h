#pragma once

#include "LoggingService.h"
#include "Util/Singleton.h"
#include "Util/StringUtil.h"

namespace ARC
{
   class LoggingManager : public Singleton<LoggingManager>
   {
   public:
      static constexpr std::string_view DEFAULT_CORE_LOGGER_NAME = "ARCANE";
      static constexpr std::string_view DEFAULT_APPLICATION_LOGGER_NAME = "APP";
      
      using LoggerPtr = std::unique_ptr<LoggingService, std::function<void(LoggingService*)>>;

      static LoggingService& GetCoreLogger() { return *(GetInstance().m_coreLogger); }

      template <typename Deleter = LoggerDeleter>
      static inline void SetCoreLogger(LoggingService* newLogger, Deleter deleter = Deleter())
      {
         GetInstance().m_coreLogger = LoggerPtr(newLogger, deleter);
      }

      static inline bool HasCoreLogger() { return GetInstance().m_coreLogger != nullptr; }

      static inline LoggingService& GetApplicationLogger() { return *(GetInstance().m_applicationLogger); }

      template <typename Deleter = LoggerDeleter>
      static inline void SetApplicationLogger(LoggingService* newLogger, Deleter deleter = Deleter())
      {
         GetInstance().m_applicationLogger = LoggerPtr(newLogger, deleter);
      }

      static inline bool HasApplicationLogger() { return GetInstance().m_applicationLogger != nullptr; }

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
} // namespace ARC
