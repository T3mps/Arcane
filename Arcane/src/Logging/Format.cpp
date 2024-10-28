#include "arcpch.h"
#include "Format.h"

ARC::Format::Format(const FormatInfo& info) :
   m_info(info),
   m_pattern(info.pattern),
   m_estimatedSize(0)
{
   m_fragments.reserve(info.fragmentCapacity);
   ParsePattern(m_pattern);
}

ARC::TokenType ARC::Format::Token::GetType(std::string_view token)
{
   if (token == TIMESTAMP) return TokenType::Timestamp;
   if (token == LEVEL)     return TokenType::Level;
   if (token == NAME)      return TokenType::Name;
   if (token == MESSAGE)   return TokenType::Message;
   if (token == SOURCE)    return TokenType::Source;
   if (token == FUNCTION)  return TokenType::Function;
   if (token == LINE)      return TokenType::Line;
   return                         TokenType::None;
}

std::string ARC::Format::operator()(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   TimeBuffer timeBuffer = GetTimeString();
   std::string sourcePath = GetSourceFileName(location);
   std::string functionName = GetFunctionName(location.function_name());

   switch (m_info.outputFormat)
   {
      case OutputFormat::JSON:     return FormatJSON(level, name, message, location);
      case OutputFormat::XML:      return FormatXML(level, name, message, location);
      case OutputFormat::CSV:      return FormatCSV(level, name, message, location);
      case OutputFormat::KeyValue: return FormatKeyValue(level, name, message, location);
      case OutputFormat::Syslog:   return FormatSyslog(level, name, message, location);
      case OutputFormat::Graylog:  return FormatGraylog(level, name, message, location);
      case OutputFormat::Pattern:  ARC_FALLTHROUGH;
      default:                     return FormatPattern(level, name, message, location);
   }
}

void ARC::Format::SetPattern(std::string_view pattern)
{
   m_pattern = pattern;
   ParsePattern(pattern);
}

void ARC::Format::ParsePattern(std::string_view pattern)
{
   m_fragments.clear();
   m_estimatedSize = 0;
   size_t pos = 0;

   while (pos < pattern.length())
   {
      size_t tokenStart = pattern.find('{', pos);
      if (tokenStart == std::string_view::npos)
      {
         if (pos < pattern.length())
         {
            auto literal = pattern.substr(pos);
            m_fragments.push_back({literal, TokenType::None});
            m_estimatedSize += literal.size();
         }
         break;
      }

      if (tokenStart > pos)
      {
         auto literal = pattern.substr(pos, tokenStart - pos);
         m_fragments.push_back({literal, TokenType::None});
         m_estimatedSize += literal.size();
      }

      size_t tokenEnd = pattern.find('}', tokenStart);
      if (tokenEnd == std::string_view::npos)
      {
         auto literal = pattern.substr(pos);
         m_fragments.push_back({literal, TokenType::None});
         m_estimatedSize += literal.size();
         break;
      }

      std::string_view token = pattern.substr(tokenStart, tokenEnd - tokenStart + 1);
      TokenType tokenType = Token::GetType(token);

      if (tokenType != TokenType::None)
      {
         m_fragments.push_back({std::string_view{}, tokenType});
         m_estimatedSize += (tokenType == TokenType::Timestamp) ? 8 : (tokenType == TokenType::Line) ? 5 : 32;
      }
      else
      {
         m_fragments.push_back({token, TokenType::None});
         m_estimatedSize += token.size();
      }

      pos = tokenEnd + 1;
   }
}

ARC::Format::TimeBuffer ARC::Format::GetTimeString() const
{
   TimeBuffer buffer{};
   using namespace std::chrono;
   auto now = system_clock::now();
   auto time_t_now = system_clock::to_time_t(now);
   std::tm tm_now;
#ifdef ARC_PLATFORM_WINDOWS
   localtime_s(&tm_now, &time_t_now);
#else
   localtime_r(&time_t_now, &tm_now);
#endif
   buffer.length = std::strftime(buffer.buffer.data(), buffer.buffer.size(), m_info.timeFormat.data(), &tm_now);
   return buffer;
}

std::string ARC::Format::FormatPattern(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   // Create format context with all computed values
   TimeBuffer timeBuffer = GetTimeString();
   std::string sourcePath = GetSourceFileName(location);
   std::string functionName = GetFunctionName(location.function_name());

   std::string result;
   result.reserve(m_estimatedSize);

   auto appendToken = [&](TokenType type)
   {
      switch (type)
      {
         case TokenType::Timestamp:
            result.append(timeBuffer.data(), timeBuffer.size());
            return;
         case TokenType::Level:
            result.append(level);
            return;
         case TokenType::Name:
            result.append(name);
            return;
         case TokenType::Message:
            result.append(message);
            return;
         case TokenType::Source:
            result.append(sourcePath);
            return;
         case TokenType::Function:
            result.append(functionName);
            return;
         case TokenType::Line:
            result.append(std::to_string(location.line()));
            return;
         case TokenType::None:
         default:
            return;
      }
   };

   for (const auto& fragment : m_fragments)
   {
      if (fragment.type == TokenType::None)
      {
         result.append(fragment.literal);
      }
      else
      {
         appendToken(fragment.type);
      }
   }

   return result;
}

std::string ARC::Format::FormatJSON(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
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
      "line": "{}"
}})",
      GetTimeString().data(),
      level,
      name,
      EscapeString(message),
      std::filesystem::path(location.file_name()).filename().string(),
      GetFunctionName(location.function_name()),
      location.line()
   );
}

std::string ARC::Format::FormatXML(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   return std::format
   (
      R"(<log>
      <timestamp>{}</timestamp>
      <level>{}</level>
      <name>{}</name>
      <message>{}</message>
      <source>{}</source>
      <function>{}</function>
      <line>{}</line>
</log>)",
      GetTimeString().data(),
      level,
      name,
      EscapeString(message),
      std::filesystem::path(location.file_name()).filename().string(),
      GetFunctionName(location.function_name()),
      location.line()
   );
}

std::string ARC::Format::FormatCSV(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   std::string escapedMessage = message;
   std::replace(escapedMessage.begin(), escapedMessage.end(), ',', ';');
   std::replace(escapedMessage.begin(), escapedMessage.end(), '"', '\'');

   return std::format
   (
      "{},{},{},{},{},{},{}",
      GetTimeString().data(),
      level,
      name,
      escapedMessage,
      std::filesystem::path(location.file_name()).filename().string(),
      GetFunctionName(location.function_name()),
      location.line()
   );
}

std::string ARC::Format::FormatKeyValue(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   return std::format
   (
      "timestamp={} level={} name={} message=\"{}\" source={} function={} line={}",
      GetTimeString().data(),
      level,
      name,
      EscapeString(message),
      std::filesystem::path(location.file_name()).filename().string(),
      GetFunctionName(location.function_name()),
      location.line()
   );
}

std::string ARC::Format::FormatSyslog(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   int priority;
   if (level == "TRACE" || level == "DEBUG") priority = 7; // DEBUG
   else if (level == "INFO") priority = 6;                 // INFO
   else if (level == "WARN") priority = 4;                 // WARNING
   else if (level == "ERROR") priority = 3;                // ERROR
   else if (level == "FATAL") priority = 2;                // CRITICAL
   else priority = 6;                                      // Default to INFO

   return std::format
   (
      "<{}>{} {} {}[{}]: {} ({}:{})",
      priority,
      GetTimeString().data(),
      std::filesystem::path(location.file_name()).filename().string(),
      name,
      GetFunctionName(location.function_name()),
      message,
      location.file_name(),
      location.line()
   );
}

std::string ARC::Format::FormatGraylog(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   return std::format
   (
      R"({{"version":"{}","host":"{}","short_message":"{}","full_message":"{}","timestamp":"{}","level":"{}","_logger_name":"{}","_source_file":"{}","_source_line":"{}","_source_function":"{}"}})",
      Version::Engine().ToString(),
      name,
      EscapeString(message.substr(0, 50)), // short version
      EscapeString(message),               // full version
      GetTimeString().data(),
      level,
      name,
      std::filesystem::path(location.file_name()).filename().string(),
      location.line(),
      GetFunctionName(location.function_name())
   );
}

std::string ARC::Format::GetFunctionName(const char* function)
{
   std::string_view funcView(function);

   // Early return for empty function names
   if (funcView.empty())
      return "Unknown";

   // First handle special cases

   // Case 1: Lambda functions
   if (funcView.find("lambda") != std::string_view::npos)
   {
      // Try to get the parent function name for context
      size_t classEnd = funcView.rfind("::");
      if (classEnd != std::string_view::npos)
      {
         size_t classStart = funcView.rfind("::", classEnd - 2);
         if (classStart != std::string_view::npos)
         {
            // Extract the parent function name
            std::string_view parentFunc = funcView.substr(classStart + 2, classEnd - classStart - 2);
            // Clean up the parent function name
            return CleanFunctionName(std::string(parentFunc)) + "::lambda";
         }
      }
      // If we can't get context, just return "lambda"
      return "lambda";
   }

   // Case 2: State accessors (ends with State))
   if (funcView.ends_with("State)"))
   {
      size_t lastColons = funcView.rfind("::");
      if (lastColons != std::string_view::npos)
      {
         return std::string(funcView.substr(lastColons + 2, funcView.length() - lastColons - 3));
      }
   }

   // Find the actual function name
   size_t functionStart = 0;
   size_t functionEnd = funcView.length();

   // Find the last occurrence of spaces followed by "::" to catch the actual class name
   size_t lastColons = funcView.rfind("::");
   if (lastColons != std::string_view::npos)
   {
      // Look for the start of the class name
      size_t classStart = funcView.rfind(" ", lastColons);
      if (classStart != std::string_view::npos)
      {
         functionStart = classStart + 1;
      }
      else
      {
         // If no space found before ::, start from the beginning of namespace
         size_t namespaceStart = funcView.find("ARC::");
         functionStart = (namespaceStart != std::string_view::npos) ? namespaceStart + 5 : 0;
      }
   }
   else
   {
      // No :: found, look for the last space before (
      size_t openParen = funcView.find('(');
      if (openParen != std::string_view::npos)
      {
         size_t lastSpace = funcView.rfind(' ', openParen);
         if (lastSpace != std::string_view::npos)
         {
            functionStart = lastSpace + 1;
         }
      }
   }

   // Find the end of the function name (before parameters)
   size_t openParen = funcView.find('(', functionStart);
   if (openParen != std::string_view::npos)
   {
      functionEnd = openParen;
   }

   // Extract the function signature and clean it
   return CleanFunctionName(std::string(funcView.substr(functionStart, functionEnd - functionStart)));
}

// Helper function to clean up function names
std::string ARC::Format::CleanFunctionName(const std::string& signature)
{
   std::string result = signature;

   // Remove common decorators
   static const std::vector<std::string> toRemove = {
      "class ",
      "struct ",
      "__cdecl ",
      "__stdcall ",
      "__fastcall ",
      "virtual ",
      "static ",
      "<void>",
      "<>",
      "ARC::",  // Remove namespace prefix
   };

   for (const auto& str : toRemove)
   {
      size_t pos = result.find(str);
      while (pos != std::string::npos)
      {
         result.erase(pos, str.length());
         pos = result.find(str);
      }
   }

   // Remove template parameters
   {
      size_t depth = 0;
      size_t start = std::string::npos;
      std::string cleaned;
      cleaned.reserve(result.length());

      for (size_t i = 0; i < result.length(); ++i)
      {
         char c = result[i];
         if (c == '<')
         {
            if (depth == 0) start = i;
            depth++;
         }
         else if (c == '>')
         {
            depth--;
            if (depth == 0 && start != std::string::npos)
            {
               continue;
            }
         }
         else if (depth == 0)
         {
            cleaned += c;
         }
      }

      result = cleaned.empty() ? result : cleaned;
   }

   // Trim any remaining whitespace
   size_t first = result.find_first_not_of(" \t");
   size_t last = result.find_last_not_of(" \t");
   if (first != std::string::npos && last != std::string::npos)
   {
      result = result.substr(first, last - first + 1);
   }

   return result;
}

std::string ARC::Format::EscapeString(const std::string& input)
{
   std::string output;
   output.reserve(input.size());
   for (char c : input)
   {
      switch (c)
      {
         case '\"': output += "\\\""; break;
         case '\\': output += "\\\\"; break;
         case '\b': output += "\\b";  break;
         case '\f': output += "\\f";  break;
         case '\n': output += "\\n";  break;
         case '\r': output += "\\r";  break;
         case '\t': output += "\\t";  break;
         default:   output += c;      break;
      }
   }
   return output;
}

std::string ARC::Format::GetSourceFileName(const std::source_location& location) const
{
   return std::filesystem::path(location.file_name()).filename().string();
}
