#pragma once

#include "LoggingService.h"
#include "Util/Singleton.h"
#include "Util/StringUtil.h"

namespace ARC
{
   struct LoggingInfo
   {
      bool colorizeOutput = true;
      std::string_view timeFormat = "%H:%M:%S";
      OutputFormat outputFormat = OutputFormat::Pattern;
      std::string_view pattern = FormatPatterns::DEFAULT;
      Level minimumLevel = 
#ifdef ARC_BUILD_DEBUG
         Level::Trace;
#else
         Level::Info;
#endif
   };

   class LoggingManager : public Singleton<LoggingManager>
   {
   public:
      static constexpr std::string_view DEFAULT_CORE_LOGGER_NAME = "ARCANE";
      static constexpr std::string_view DEFAULT_APPLICATION_LOGGER_NAME = "APP";
      
      using LoggerPtr = std::unique_ptr<LoggingService, std::function<void(LoggingService*)>>;

      static LoggingService& GetCoreLogger() { return *(GetInstance().m_coreLogger); }
      static bool HasCoreLogger() { return GetInstance().m_coreLogger != nullptr; }

      static LoggingService& GetApplicationLogger() { return *(GetInstance().m_applicationLogger); }
      static bool HasApplicationLogger() { return GetInstance().m_applicationLogger != nullptr; }

      static Result<void> Initialize(const LoggingInfo& info = LoggingInfo{})
      {
         return GetInstance().InitializeInternal(info);
      }

      static Result<void> InitializeCore(const LoggingInfo& info = LoggingInfo{})
      {
         return GetInstance().InitializeCoreInternal(info);
      }

      static Result<void> InitializeApplication(const LoggingInfo& info = LoggingInfo{})
      {
         return GetInstance().InitializeApplicationInternal(info);
      }

      static void Shutdown();

   private:
      LoggingManager() = default;
      virtual ~LoggingManager() = default;

      LoggingManager(const LoggingManager&) = delete;
      LoggingManager& operator=(const LoggingManager&) = delete;

      Result<void> InitializeInternal(const LoggingInfo& info);
      Result<void> InitializeCoreInternal(const LoggingInfo& info);
      Result<void> InitializeApplicationInternal(const LoggingInfo& info);

      template <typename Deleter = LoggerDeleter>
      Result<void> CreateLogger(LoggerPtr& loggerPtr, const std::string& name, const LoggingInfo& info, Deleter deleter = Deleter())
      {
         if (loggerPtr)
            return ARC_MAKE_ERROR(ErrorCode::OperationFailed, std::format("Logger '{}' is already initialized", name));

         auto logger = new Logger(name);
         if (!logger)
            return ARC_MAKE_ERROR(ErrorCode::OperationFailed, std::format("Failed to create logger '{}'", name));

         logger->SetLevel(info.minimumLevel);
         /*logger->GetFormatter().SetPattern(info.pattern);
         logger->GetFormatter().SetOutputFormat(info.outputFormat);
         logger->GetFormatter().SetTimeFormat(info.timeFormat);
         logger->GetFormatter().SetColorize(info.colorizeOutput);*/

         loggerPtr = LoggerPtr(logger, deleter);
         return {};
      }

      LoggerPtr m_coreLogger;
      LoggerPtr m_applicationLogger;
      std::once_flag m_coreInitFlag;
      std::once_flag m_appInitFlag;

      friend class Singleton<LoggingManager>;
   };
} // namespace ARC
