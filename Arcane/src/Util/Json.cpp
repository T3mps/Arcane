#include "arcpch.h"
#include "JSON.h"

ARC::JSON::JSON(const std::string& filePath)
{
   Load(filePath);
}

bool ARC::JSON::Save(const std::string& filePath, int tabSize) const
{
   std::ofstream file(filePath);
   if (!file.is_open())
   {
      std::cerr << "Failed to save file: " << filePath << std::endl;
      return false;
   }

   try
   {
      file << std::setw(tabSize) << m_data;
   }
   catch (const std::exception& e)
   {
      std::cerr << "Error writing JSON to file: " << filePath << "\n" << e.what() << std::endl;
      return false;
   }

   file.close();
   return true;
}

bool ARC::JSON::Load(const std::string& filePath)
{
   std::ifstream file(filePath);
   if (!file.is_open())
   {
      std::cerr << "Failed to open file: " << filePath << std::endl;
      return false;
   }

   try
   {
      file >> m_data;
   }
   catch (const std::exception& e)
   {
      std::cerr << "Error parsing JSON from file: " << filePath << "\n" << e.what() << std::endl;
      return false;
   }

   file.close();
   return true;
}

std::string ARC::JSON::ToString(int tabSize) const
{
   return m_data.dump(tabSize);
}

std::string ARC::JSON::GenerateJsonString(const std::vector<std::pair<std::string, std::string>>& data, bool prettyPrint, int indentLevel)
{
   std::ostringstream oss;
   std::string indent(prettyPrint ? indentLevel : 0, ' ');
   std::string newline = prettyPrint ? "\n" : "";

   oss << "{" << newline;

   bool first = true;
   for (const auto& [key, value] : data)
   {
      if (!first)
      {
         oss << "," << newline;
      }
      first = false;

      if (prettyPrint)
      {
         oss << indent;
      }

      oss << "\"" << EscapeString(key) << "\": \"" << EscapeString(value) << "\"";
   }

   oss << newline << "}";
   return oss.str();
}

std::wstring ARC::JSON::GenerateJsonWString(const std::vector<std::pair<std::wstring, std::wstring>>& data, bool prettyPrint, int indentLevel)
{
   std::wostringstream oss;
   std::wstring indent(prettyPrint ? indentLevel : 0, L' ');
   std::wstring newline = prettyPrint ? L"\n" : L"";

   oss << L"{" << newline;

   bool first = true;
   for (const auto& [key, value] : data)
   {
      if (!first)
         oss << L"," << newline;
      else
         first = false;

      if (prettyPrint)
         oss << indent;

      oss << L"\"" << EscapeWString(key) << L"\": \"" << EscapeWString(value) << L"\"";
   }

   oss << newline << L"}";
   return oss.str();
}

std::string ARC::JSON::EscapeString(const std::string& input)
{
   std::string output;
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

std::wstring ARC::JSON::EscapeWString(const std::wstring& input)
{
   std::wstring output;
   for (wchar_t c : input)
   {
      switch (c)
      {
         case L'\"': output += L"\\\"";   break;
         case L'\\': output += L"\\\\";   break;
         case L'\b': output += L"\\b";    break;
         case L'\f': output += L"\\f";    break;
         case L'\n': output += L"\\n";    break;
         case L'\r': output += L"\\r";    break;
         case L'\t': output += L"\\t";    break;
         default:    output += c;         break;
      }
   }
   return output;
}
