#include "arcpch.h"
#include "Logger.h"

#include "Core/Console.h"
#include "Level.h"
#include "Util/ANSI.h"

ARC::Logger::Logger(const std::string& name) :
   LoggingService(),
   m_name(name),
   m_format(),
   m_colorizeMessages(true),
   m_flushInterval(DEFAULT_FLUSH_INTERVAL),
   m_messageCount(0),
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
   const std::string& levelString = LevelToString(level).data();
   std::string formattedMessage = m_format(levelString, m_name, message, location);

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
         case Level::Info: __fallthrough;
         default:
            break;
      }
   }

   std::ostringstream osstream;
   osstream << formattedMessage << ',';
   return osstream.str();
}

void ARC::Logger::ProcessLogs()
{
   while (m_running || !m_logQueue.Empty())
   {
      auto value = m_logQueue.TryPop();
      if (value)
      {
         if (!(*value).empty())
         {
            OutputLog(*value);
            std::lock_guard<std::mutex> flushLock(m_flushMutex);
            if (++m_messageCount >= m_flushInterval)
            {
               FlushStream();
               m_messageCount = 0;
            }
         }
      }
      else
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
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
