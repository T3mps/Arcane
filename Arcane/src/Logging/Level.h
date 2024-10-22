#pragma once

namespace ARC
{
   enum class Level
   {
      Trace,
      Debug,
      Info,
      Warn,
      Error,
      Fatal,
      Off
   };

   inline constexpr std::array<std::pair<Level, std::string_view>, 7> levelToStringArray =
   {{
      { Level::Trace, "TRACE" },
      { Level::Debug, "DEBUG" },
      { Level::Info,  "INFO"  },
      { Level::Warn,  "WARN"  },
      { Level::Error, "ERROR" },
      { Level::Fatal, "FATAL" },
      { Level::Off,   "OFF"   }
   }};


   constexpr std::string_view LevelToString(Level level)
   {
      for (const auto& pair : levelToStringArray)
      {
         if (pair.first == level)
            return pair.second;
      }
      return "UNKNOWN";
   }

   inline constexpr std::array<std::pair<std::string_view, Level>, 7> stringToLevelArray =
   {{
      { "TRACE", Level::Trace },
      { "DEBUG", Level::Debug },
      { "INFO",  Level::Info  },
      { "WARN",  Level::Warn  },
      { "ERROR", Level::Error },
      { "FATAL", Level::Fatal },
      { "OFF",   Level::Off   }
   }};

   constexpr Level StringToLevel(std::string_view levelStr)
   {
      for (const auto& pair : stringToLevelArray)
      {
         if (pair.first == levelStr)
            return pair.second;
      }
      return Level::Off;
   }
} // namespace ARC
