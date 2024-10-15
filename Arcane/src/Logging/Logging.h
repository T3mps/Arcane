#pragma once

#include "Format.h"
#include "Level.h"
#include "Logger.h"
#include "LoggingManager.h"
#include "LoggingService.h"

#ifdef ARC_BUILD_DEBUG
   #define ARC_CORE_TRACE(message) \
      ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetCoreLogger(), "Invalid core logger; initialize logging system first.")); \
      ::ARC::LoggingManager::GetCoreLogger().Log(ARC::Level::Trace,    ::ARC::StringUtil::ToWString(message))
   #define ARC_CORE_DEBUG(message) \
      ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetCoreLogger(), "Invalid core logger; initialize logging system first.")); \
      ::ARC::LoggingManager::GetCoreLogger().Log(ARC::Level::Debug,    ::ARC::StringUtil::ToWString(message))
#else
   #define ARC_CORE_TRACE(message)
   #define ARC_CORE_DEBUG(message)
#endif

#define ARC_CORE_INFO(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetCoreLogger(), "Invalid core logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetCoreLogger().Log(ARC::Level::Info,    ::ARC::StringUtil::ToWString(message))
#define ARC_CORE_WARN(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetCoreLogger(), "Invalid core logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetCoreLogger().Log(ARC::Level::Warn,    ::ARC::StringUtil::ToWString(message))
#define ARC_CORE_ERROR(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetCoreLogger(), "Invalid core logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetCoreLogger().Log(ARC::Level::Error,    ::ARC::StringUtil::ToWString(message))
#define ARC_CORE_FATAL(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetCoreLogger(), "Invalid core logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetCoreLogger().Log(ARC::Level::Fatal,    ::ARC::StringUtil::ToWString(message))

#ifdef ARC_BUILD_DEBUG
   #define ARC_TRACE(message) \
      ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetApplicationLogger(), "Invalid application logger; initialize logging system first.")); \
      ::ARC::LoggingManager::GetApplicationLogger().Log(ARC::Level::Trace, ::ARC::StringUtil::ToWString(message))
   #define ARC_DEBUG(message) \
      ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetApplicationLogger(), "Invalid application logger; initialize logging system first.")); \
      ::ARC::LoggingManager::GetApplicationLogger().Log(ARC::Level::Debug, ::ARC::StringUtil::ToWString(message))
#else
   #define ARC_TRACE(message)
   #define ARC_DEBUG(message)
#endif

#define ARC_INFO(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetApplicationLogger(), "Invalid application logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetApplicationLogger().Log(ARC::Level::Info,  ::ARC::StringUtil::ToWString(message))
#define ARC_WARN(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetApplicationLogger(), "Invalid application logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetApplicationLogger().Log(ARC::Level::Warn,  ::ARC::StringUtil::ToWString(message))
#define ARC_ERROR(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetApplicationLogger(), "Invalid application logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetApplicationLogger().Log(ARC::Level::Error, ::ARC::StringUtil::ToWString(message))
#define ARC_FATAL(message) \
   ARC_EXPAND_MACRO(ARC_ASSERT(&::ARC::LoggingManager::GetApplicationLogger(), "Invalid application logger; initialize logging system first.")); \
   ::ARC::LoggingManager::GetApplicationLogger().Log(ARC::Level::Fatal, ::ARC::StringUtil::ToWString(message))
