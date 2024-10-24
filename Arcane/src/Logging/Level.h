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

   constexpr std::array<std::string_view, 7> levelStrings =
   {
      "TRACE",
      "DEBUG",
      "INFO",
      "WARN",
      "ERROR",
      "FATAL",
      "OFF"
   };

   constexpr std::string_view LevelToString(Level level)
   {
      return levelStrings[static_cast<size_t>(level)];
   }
} // namespace ARC
