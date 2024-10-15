#include "arcpch.h"
#include "Format.h"

#include "Util/Json.h"
#include "Util/StringUtil.h"

ARC::Format::Format(const std::wstring& formatString) : m_formatString(formatString)
{
   m_tokenMap[Tokens::TIMESTAMP.data()] = std::wstring();
   m_tokenMap[Tokens::LEVEL.data()] = std::wstring();
   m_tokenMap[Tokens::NAME.data()] = std::wstring();
   m_tokenMap[Tokens::MESSAGE.data()] = std::wstring();
   m_tokenMap[Tokens::SOURCE.data()] = std::wstring();
   m_tokenMap[Tokens::FUNCTION.data()] = std::wstring();
   m_tokenMap[Tokens::LINE.data()] = std::wstring();

   ExtractOrderedKeys();
}

std::wstring ARC::Format::operator()(const std::wstring& level, const std::wstring& name, const std::wstring& message, const std::source_location& location)
{
   m_tokenMap[Tokens::TIMESTAMP.data()]   =  GetCurrentTimestamp();
   m_tokenMap[Tokens::LEVEL.data()]       =  level;
   m_tokenMap[Tokens::NAME.data()]        =  name;
   m_tokenMap[Tokens::MESSAGE.data()]     =  message;
   m_tokenMap[Tokens::SOURCE.data()]      =  StringUtil::ToWString(location.file_name());
   m_tokenMap[Tokens::FUNCTION.data()]    =  StringUtil::ToWString(location.function_name());
   m_tokenMap[Tokens::LINE.data()]        =  std::to_wstring(location.line()) + L":" + std::to_wstring(location.column());

   std::vector<std::pair<std::wstring, std::wstring>> orderedLogData;
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

   return ARC::JSON::GenerateJsonWString(orderedLogData, true);
}

void ARC::Format::SetFormatString(const std::wstring& formatStr)
{
   if (formatStr.empty())
      return;
   m_formatString.assign(formatStr);
   ExtractOrderedKeys();
}

std::wstring ARC::Format::GetCurrentTimestamp()
{
   auto now = std::chrono::system_clock::now();
   return std::format(L"{:%F %T}", std::chrono::zoned_time{std::chrono::current_zone(), now});
}

void ARC::Format::ExtractOrderedKeys()
{
   m_orderedKeys.clear();
   std::wregex placeholderRegex(L"\\{([a-zA-Z]+)\\}");
   std::wsregex_iterator iter(m_formatString.begin(), m_formatString.end(), placeholderRegex);
   std::wsregex_iterator end;
   while (iter != end)
   {
      m_orderedKeys.push_back(iter->str());
      ++iter;
   }
}
