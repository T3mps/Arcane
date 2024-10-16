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

   inline constexpr std::array<std::pair<Level, std::wstring_view>, 7> levelToStringArray =
   {{
      { Level::Trace, L"TRACE" },
      { Level::Debug, L"DEBUG" },
      { Level::Info,  L"INFO"  },
      { Level::Warn,  L"WARN"  },
      { Level::Error, L"ERROR" },
      { Level::Fatal, L"FATAL" },
      { Level::Off,   L"OFF"   }
   }};


   constexpr std::wstring_view LevelToString(Level level)
   {
      for (const auto& pair : levelToStringArray)
      {
         if (pair.first == level)
            return pair.second;
      }
      return L"UNKNOWN";
   }

   inline constexpr std::array<std::pair<std::wstring_view, Level>, 7> stringToLevelArray =
   {{
      { L"TRACE", Level::Trace },
      { L"DEBUG", Level::Debug },
      { L"INFO",  Level::Info  },
      { L"WARN",  Level::Warn  },
      { L"ERROR", Level::Error },
      { L"FATAL", Level::Fatal },
      { L"OFF",   Level::Off   }
   }};

   constexpr Level StringToLevel(std::wstring_view levelStr)
   {
      for (const auto& pair : stringToLevelArray)
      {
         if (pair.first == levelStr)
            return pair.second;
      }
      return Level::Off;
   }
}
