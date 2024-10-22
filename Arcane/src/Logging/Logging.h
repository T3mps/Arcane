#pragma once

#include "Format.h"
#include "Level.h"
#include "Logger.h"
#include "LoggingManager.h"
#include "LoggingService.h"

namespace ARC::Log
{
   constexpr std::string_view LOG_CORE_ASSERT_MESSAGE = "Invalid core logger; initialize logging system first.";

   template <typename M>
   ARC_FORCE_INLINE inline void CoreTrace(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Trace, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreDebug(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Debug, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreInfo(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Info, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreWarn(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Warn, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreError(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Error, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreFatal(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Fatal, StringUtil::ToString(message), location);
   }

   constexpr std::string_view LOG_APP_ASSERT_MESSAGE = "Invalid application logger; initialize logging system first.";

   template <typename M>
   ARC_FORCE_INLINE inline void Trace(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Trace, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Debug(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Debug, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Info(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Info, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Warn(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Warn, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Error(const M& message, const std::source_location& location = std::source_location::current())
   {
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Error, StringUtil::ToString(message), location);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Fatal(const M& message, const std::source_location& location = std::source_location::current())
   { 
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Fatal, StringUtil::ToString(message), location);
   }
} // ARC::Log

#define ARC_CORE_TRACE(message)  ::ARC::Log::CoreTrace(message);
#define ARC_CORE_DEBUG(message)  ::ARC::Log::CoreDebug(message);
#define ARC_CORE_INFO(message)   ::ARC::Log::CoreInfo(message);
#define ARC_CORE_WARN(message)   ::ARC::Log::CoreWarn(message);
#define ARC_CORE_ERROR(message)  ::ARC::Log::CoreError(message);
#define ARC_CORE_FATAL(message)  ::ARC::Log::CoreFatal(message);

#define ARC_TRACE(message)       ::ARC::Log::Trace(message);
#define ARC_DEBUG(message)       ::ARC::Log::Debug(message);
#define ARC_INFO(message)        ::ARC::Log::Info(message);
#define ARC_WARN(message)        ::ARC::Log::Warn(message);
#define ARC_ERROR(message)       ::ARC::Log::Error(message);
#define ARC_FATAL(message)       ::ARC::Log::Fatal(message);
