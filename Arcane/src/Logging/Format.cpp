#include "arcpch.h"
#include "Format.h"

#include "Util/Serialization/JSON.h"
#include "Util/StringUtil.h"

ARC::Format::Format(const std::string& formatString) : m_formatString(formatString)
{
   m_tokenMap[Tokens::TIMESTAMP.data()] = std::string();
   m_tokenMap[Tokens::LEVEL.data()]     = std::string();
   m_tokenMap[Tokens::NAME.data()]      = std::string();
   m_tokenMap[Tokens::MESSAGE.data()]   = std::string();
   m_tokenMap[Tokens::SOURCE.data()]    = std::string();
   m_tokenMap[Tokens::FUNCTION.data()]  = std::string();
   m_tokenMap[Tokens::LINE.data()]      = std::string();
   ExtractOrderedKeys();
}

std::string ARC::Format::operator()(const std::string& level, const std::string& name, const std::string& message, const std::source_location& location)
{
   std::ostringstream oss;
   oss << location.line()
      << ":"
      << location.column();

   m_tokenMap[Tokens::TIMESTAMP.data()] = GetCurrentTimestamp();
   m_tokenMap[Tokens::LEVEL.data()]     = level;
   m_tokenMap[Tokens::NAME.data()]      = name;
   m_tokenMap[Tokens::MESSAGE.data()]   = message;
   m_tokenMap[Tokens::SOURCE.data()]    = location.file_name();
   m_tokenMap[Tokens::FUNCTION.data()]  = location.function_name();
   m_tokenMap[Tokens::LINE.data()]      = oss.str();

   std::vector<std::pair<std::string, std::string>> orderedLogData;
   for (const auto& key : m_orderedKeys)
   {
      for (const auto& entry : m_tokenMap)
      {
         if (key == entry.first)
         {
            orderedLogData.push_back(entry);
            break;
         }
      }
   }

   return ARC::JSON::GenerateJsonString(orderedLogData, true);
}

void ARC::Format::SetFormatString(const std::string& formatStr)
{
   if (formatStr.empty())
      return;
   m_formatString.assign(formatStr);
   ExtractOrderedKeys();
}

std::string ARC::Format::GetCurrentTimestamp()
{
   auto now = std::chrono::system_clock::now();
   return std::format("{:%F %T}", std::chrono::zoned_time{std::chrono::current_zone(), now});
}

void ARC::Format::ExtractOrderedKeys()
{
   m_orderedKeys.clear();
   std::regex placeholderRegex("\\{([a-zA-Z]+)\\}");
   std::sregex_iterator iter(m_formatString.begin(), m_formatString.end(), placeholderRegex);
   std::sregex_iterator end;
   while (iter != end)
   {
      m_orderedKeys.push_back(iter->str());
      ++iter;
   }
}
