#include "arcpch.h"
#include "Logger.h"

#include "Core/Console.h"
#include "Level.h"
#include "Util/ANSI.h"

ARC::Logger::Logger(const std::wstring& name) :
   LoggingService(),
   m_name(name),
   m_format(),
   m_flushInterval(DEFAULT_FLUSH_INTERVAL),
   m_messageCount(0),
   m_colorizeMessages(true)
{
#ifdef ARC_BUILD_DEBUG
   m_level = Level::Trace;
#else
   m_level = Level::Info;
#endif
}

void ARC::Logger::Log(Level level, const std::wstring& message, const std::source_location& location)
{
   if (level < m_level)
      return;
   
   std::lock_guard<std::mutex> lock(m_mutex);

   const std::wstring& levelStr = LevelToString(level).data();
   std::wstring formattedMessage = std::wstring(m_format(levelStr, m_name, message, location) + L',');
   
   if (m_colorizeMessages)
   {
      switch (level)
      {
         case Level::Trace:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_CYAN<std::wstring>());
            break;
         case Level::Debug:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_GREEN<std::wstring>());
            break;
         case Level::Warn:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_YELLOW<std::wstring>());
            break;
         case Level::Error:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_RED<std::wstring>());
            break;
         case Level::Fatal:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_WHITE<std::wstring>(), AnsiColor::BG_RED<std::wstring>());
            break;
         case Level::Info: __fallthrough;
         default:
            break;
      }
   }

   OutputLog(formattedMessage);

   if (++m_messageCount >= m_flushInterval || level > Level::Info)
   {
      Console::Flush(std::clog);
      if (m_messageCount >= m_flushInterval)
         m_messageCount = 0;
   }
}

void ARC::Logger::OutputLog(const std::wstring& message) const
{
   Console::LogW(message);
}

