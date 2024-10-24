#include "arcpch.h"
#include "Format.h"

#include "Util/StringUtil.h"

ARC::Format::Format(/*const std::string& formatString*/)
{
}

std::string ARC::Format::operator()(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   return std::format
   (
      R"({{
      "timestamp": "{}",
      "level": "{}",
      "name": "{}",
      "message": "{}",
      "source": "{}",
      "function": "{}",
      "line": "{}:{}"
}})",
      GetCurrentTimestamp(),
      level,
      name,
      EscapeString(message),
      location.file_name(),
      location.function_name(),
      location.line(),
      location.column()
   );
}

std::string ARC::Format::GetCurrentTimestamp()
{
   using namespace std::chrono;
   auto now = system_clock::now();
   auto time_t_now = system_clock::to_time_t(now);
   std::tm tm_now;
#ifdef ARC_PLATFORM_WINDOWS
   localtime_s(&tm_now, &time_t_now);
#else
   localtime_r(&time_t_now, &tm_now);
#endif
   char buffer[20];
   std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_now);
   return std::string(buffer);
}

std::string ARC::Format::EscapeString(const std::string& input)
{
   std::string output;
   output.reserve(input.size());
   for (char c : input)
   {
      switch (c)
      {
         case '\"': output += "\\\"";  break;
         case '\\': output += "\\\\";  break;
         case '\b': output += "\\b";   break;
         case '\f': output += "\\f";   break;
         case '\n': output += "\\n";   break;
         case '\r': output += "\\r";   break;
         case '\t': output += "\\t";   break;
         default:   output += c;       break;
      }
   }
   return output;
}
