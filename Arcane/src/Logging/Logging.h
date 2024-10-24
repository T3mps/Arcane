#pragma once

#include "Format.h"
#include "Level.h"
#include "Logger.h"
#include "LoggingManager.h"
#include "LoggingService.h"

namespace ARC::Log
{
   constexpr std::string_view LOG_CORE_ASSERT_MESSAGE = "Invalid core logger; initialize logging system first.";

   // Core logger functions
   template <typename M>
   ARC_FORCE_INLINE inline void CoreTrace(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Trace, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void CoreTrace(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      CoreTrace(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreDebug(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Debug, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void CoreDebug(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      CoreDebug(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreInfo(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Info, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void CoreInfo(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      CoreInfo(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreWarn(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Warn, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void CoreWarn(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      CoreWarn(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreError(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Error, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void CoreError(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      CoreError(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void CoreFatal(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetCoreLogger(), LOG_CORE_ASSERT_MESSAGE.data());
      LoggingManager::GetCoreLogger().Log(Level::Fatal, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void CoreFatal(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      CoreFatal(location, message);
   }

   constexpr std::string_view LOG_APP_ASSERT_MESSAGE = "Invalid application logger; initialize logging system first.";

   // Application logger functions
   template <typename M>
   ARC_FORCE_INLINE inline void Trace(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Trace, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void Trace(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      Trace(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Debug(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Debug, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void Debug(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      Debug(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Info(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Info, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void Info(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      Info(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Warn(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Warn, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void Warn(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      Warn(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Error(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Error, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void Error(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      Error(location, message);
   }

   template <typename M>
   ARC_FORCE_INLINE inline void Fatal(const std::source_location& location, const M& message)
   {
      static_assert(std::is_convertible_v<M, std::string_view>, "Message must be convertible to std::string_view");
      ARC_ASSERT(&LoggingManager::GetApplicationLogger(), LOG_APP_ASSERT_MESSAGE.data());
      LoggingManager::GetApplicationLogger().Log(Level::Fatal, StringUtil::ToString(message), location);
   }

   template<typename... Args>
   ARC_FORCE_INLINE inline void Fatal(const std::source_location& location, std::format_string<Args...> format_str, Args&&... args)
   {
      std::string message = std::format(format_str, std::forward<Args>(args)...);
      Fatal(location, message);
   }
} // namespace ARC::Log

#define ARC_CORE_TRACE(...) ::ARC::Log::CoreTrace(std::source_location::current(), __VA_ARGS__)
#define ARC_CORE_DEBUG(...) ::ARC::Log::CoreDebug(std::source_location::current(), __VA_ARGS__)
#define ARC_CORE_INFO(...)  ::ARC::Log::CoreInfo(std::source_location::current(), __VA_ARGS__)
#define ARC_CORE_WARN(...)  ::ARC::Log::CoreWarn(std::source_location::current(), __VA_ARGS__)
#define ARC_CORE_ERROR(...) ::ARC::Log::CoreError(std::source_location::current(), __VA_ARGS__)
#define ARC_CORE_FATAL(...) ::ARC::Log::CoreFatal(std::source_location::current(), __VA_ARGS__)

#define ARC_TRACE(...)     ::ARC::Log::Trace(std::source_location::current(), __VA_ARGS__)
#define ARC_DEBUG(...)     ::ARC::Log::Debug(std::source_location::current(), __VA_ARGS__)
#define ARC_INFO(...)      ::ARC::Log::Info(std::source_location::current(), __VA_ARGS__)
#define ARC_WARN(...)      ::ARC::Log::Warn(std::source_location::current(), __VA_ARGS__)
#define ARC_ERROR(...)     ::ARC::Log::Error(std::source_location::current(), __VA_ARGS__)
#define ARC_FATAL(...)     ::ARC::Log::Fatal(std::source_location::current(), __VA_ARGS__)

