#include "arcpch.h"
#include "JSON.h"

ARC::JSON::JSON(const std::string& filePath)
{
   Load(filePath);
}

bool ARC::JSON::Save(const std::string& filePath, int32_t tabSize) const
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

std::string ARC::JSON::ToString(int32_t tabSize) const
{
   return m_data.dump(tabSize);
}

std::string ARC::JSON::GenerateJsonString(const std::vector<std::pair<std::string, std::string>>& data, bool prettyPrint, int32_t indentLevel)
{
   std::ostringstream oss;
   std::string indent(prettyPrint ? indentLevel : 0, ' ');
   std::string newline = prettyPrint ? "\n" : "";

   oss << "{" << newline;

   bool first = true;
   for (const auto& [key, value] : data)
   {
      if (!first)
         oss << "," << newline;
      else
         first = false;

      if (prettyPrint)
         oss << indent;

      oss << "\"" << EscapeString<std::string>(key) << "\": \"" << EscapeString<std::string>(value) << "\"";
   }

   oss << newline << "}";
   return oss.str();
}

std::wstring ARC::JSON::GenerateJsonWString(const std::vector<std::pair<std::wstring, std::wstring>>& data, bool prettyPrint, int32_t indentLevel)
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

      oss << L"\"" << EscapeString<std::wstring>(key) << L"\": \"" << EscapeString<std::wstring>(value) << L"\"";
   }

   oss << newline << L"}";
   return oss.str();
}
