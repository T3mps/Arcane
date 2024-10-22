#include "arcpch.h"
#include "StringUtil.h"

std::string ARC::StringUtil::ToLower(const std::string& str)
{
   std::string lowerStr = str;
   std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::tolower);
   return lowerStr;
}

std::wstring ARC::StringUtil::ToLower(const std::wstring& str)
{
   std::wstring lowerStr = str;
   std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::towlower);
   return lowerStr;
}

std::string ARC::StringUtil::ToUpper(const std::string& str)
{
   std::string upperStr = str;
   std::transform(upperStr.begin(), upperStr.end(), upperStr.begin(), ::toupper);
   return upperStr;
}

std::wstring ARC::StringUtil::ToUpper(const std::wstring& str)
{
   std::wstring upperStr = str;
   std::transform(upperStr.begin(), upperStr.end(), upperStr.begin(), ::towupper);
   return upperStr;
}

int32_t ARC::StringUtil::Compare(const std::string& str1, const std::string& str2)
{
   return str1.compare(str2);
}

int32_t ARC::StringUtil::Compare(const std::wstring& str1, const std::wstring& str2)
{
   return str1.compare(str2);
}

std::string ARC::StringUtil::Trim(const std::string& str)
{
   size_t first = str.find_first_not_of(' ');
   if (first == std::string::npos)
      return EMPTY_STRING;
   size_t last = str.find_last_not_of(' ');
   return str.substr(first, (last - first + 1));
}

std::wstring ARC::StringUtil::Trim(const std::wstring& str)
{
   size_t first = str.find_first_not_of(L' ');
   if (first == std::wstring::npos)
      return EMPTY_WSTRING;
   size_t last = str.find_last_not_of(L' ');
   return str.substr(first, (last - first + 1));
}

char* ARC::StringUtil::ToChar(const std::string& str)
{
   char* cstr = new char[str.length() + 1];
   std::strcpy(cstr, str.c_str());
   return cstr;
}

wchar_t* ARC::StringUtil::ToWChar(const std::wstring& str)
{
   wchar_t* wcstr = new wchar_t[str.length() + 1];
   std::wcscpy(wcstr, str.c_str());
   return wcstr;
}

std::string ARC::StringUtil::FromChar(const char* str)
{
   return std::string(str);
}

std::wstring ARC::StringUtil::FromWChar(const wchar_t* str)
{
   return std::wstring(str);
}

std::vector<std::string> ARC::StringUtil::Split(const std::string& str, const std::string& delimiters)
{
   std::vector<std::string> tokens;
   size_t start = 0, end = 0;
   while ((end = str.find_first_of(delimiters, start)) != std::string::npos)
   {
      if (end != start)
         tokens.push_back(str.substr(start, end - start));
      start = end + 1;
   }
   if (end != start)
      tokens.push_back(str.substr(start));
   return tokens;
}

std::vector<std::wstring> ARC::StringUtil::Split(const std::wstring& str, const std::wstring& delimiters)
{
   std::vector<std::wstring> tokens;
   size_t start = 0, end = 0;
   while ((end = str.find_first_of(delimiters, start)) != std::wstring::npos)
   {
      if (end != start)
         tokens.push_back(str.substr(start, end - start));
      start = end + 1;
   }
   if (end != start)
      tokens.push_back(str.substr(start));
   return tokens;
}

std::string ARC::StringUtil::Replace(const std::string& str, const std::string& find, const std::string& replace)
{
   std::string result = str;
   size_t pos = 0;
   while ((pos = result.find(find, pos)) != std::string::npos)
   {
      result.replace(pos, find.length(), replace);
      pos += replace.length();
   }
   return result;
}

std::wstring ARC::StringUtil::Replace(const std::wstring& str, const std::wstring& find, const std::wstring& replace)
{
   std::wstring result = str;
   size_t pos = 0;
   while ((pos = result.find(find, pos)) != std::wstring::npos)
   {
      result.replace(pos, find.length(), replace);
      pos += replace.length();
   }
   return result;
}

template std::string ARC::StringUtil::ToString<int32_t>(const int32_t& value);
template std::string ARC::StringUtil::ToString<uint32_t>(const uint32_t& value);
template std::string ARC::StringUtil::ToString<long>(const long& value);
template std::string ARC::StringUtil::ToString<unsigned long>(const unsigned long& value);
template std::string ARC::StringUtil::ToString<long long>(const long long& value);
template std::string ARC::StringUtil::ToString<unsigned long long>(const unsigned long long& value);
template std::string ARC::StringUtil::ToString<float>(const float& value);
template std::string ARC::StringUtil::ToString<double>(const double& value);
template std::string ARC::StringUtil::ToString<long double>(const long double& value);
template std::string ARC::StringUtil::ToString<std::wstring>(const std::wstring& value);

template std::wstring ARC::StringUtil::ToWString<int32_t>(const int32_t& value);
template std::wstring ARC::StringUtil::ToWString<uint32_t>(const uint32_t& value);
template std::wstring ARC::StringUtil::ToWString<long>(const long& value);
template std::wstring ARC::StringUtil::ToWString<unsigned long>(const unsigned long& value);
template std::wstring ARC::StringUtil::ToWString<long long>(const long long& value);
template std::wstring ARC::StringUtil::ToWString<unsigned long long>(const unsigned long long& value);
template std::wstring ARC::StringUtil::ToWString<float>(const float& value);
template std::wstring ARC::StringUtil::ToWString<double>(const double& value);
template std::wstring ARC::StringUtil::ToWString<long double>(const long double& value);
template std::wstring ARC::StringUtil::ToWString<std::string>(const std::string& value);
