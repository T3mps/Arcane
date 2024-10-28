#include "arcpch.h"
#include "Logger.h"

#include "Core/Console.h"
#include "Level.h"
#include "Util/ANSI.h"

ARC::Logger::Logger(const std::string& name) : LoggingService(),
   m_name(name),
   m_format(),
   m_colorizeMessages(true),
   m_running(false)
{
#ifdef ARC_BUILD_DEBUG
   m_level = Level::Trace;
#else
   m_level = Level::Info;
#endif
   Start();
}

ARC::Logger::~Logger()
{
   Stop();
}

void ARC::Logger::Start()
{
   if (!m_running)
   {
      m_running = true;
      m_workerThread = std::thread(&Logger::ProcessLogs, this);
   }
   else
   {
      Log(Level::Warn, "Attempted to start 'this' logger that is already started.");
   }
}

void ARC::Logger::Stop()
{
   m_running = false;
   m_logQueue.Push("");  // Push an empty message to unblock the thread
   if (m_workerThread.joinable())
      m_workerThread.join();
}

void ARC::Logger::Log(Level level, const std::string& message, const std::source_location& location)
{
   if (level >= m_level)
   {
      std::string formattedMessage = BuildMessage(level, message, location);
      m_logQueue.Push(std::move(formattedMessage));
   }
}

std::string ARC::Logger::BuildMessage(Level level, const std::string& message, const std::source_location& location)
{
   std::string_view levelString = LevelToString(level);
   std::string formattedMessage;
   formattedMessage.reserve(256);
   formattedMessage = m_format(levelString, m_name, message, location);

   if (m_colorizeMessages)
   {
      switch (level)
      {
         case Level::Trace:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_CYAN<std::string>());
            break;
         case Level::Debug:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_GREEN<std::string>());
            break;
         case Level::Warn:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_YELLOW<std::string>());
            break;
         case Level::Error:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_RED<std::string>());
            break;
         case Level::Fatal:
            AnsiColor::FormatMessage(formattedMessage, AnsiColor::FG_WHITE<std::string>(), AnsiColor::BG_RED<std::string>());
            break;
         case Level::Info: ARC_FALLTHROUGH;
         default:
            break;
      }
   }

   return formattedMessage;
}

void ARC::Logger::ProcessLogs()
{
   while (m_running)
   {
      m_logQueue.WaitForData();

      while (auto value = m_logQueue.TryPop())
      {
         OutputLog(*value);
         FlushStream();
      }
   }

   while (auto value = m_logQueue.TryPop())
   {
      OutputLog(*value);
      FlushStream();
   }
}

void ARC::Logger::OutputLog(const std::string& message) const
{
   Console::Log(message);
}

void ARC::Logger::FlushStream() const
{
   Console::Flush(std::clog);
}
