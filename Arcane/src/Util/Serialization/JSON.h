#pragma once

#include "nlohmann/json.hpp"

namespace ARC
{
   // Used to serialize small data objects, like items.
   class JSON
   {
   public:
      static constexpr int32_t TAB_SIZE = 4;

      JSON() = default;
      explicit JSON(const std::string& filePath);

      bool Save(const std::string& filePath, int32_t tabSize = 4) const;
      bool Load(const std::string& filePath);

      template <typename T>
      std::optional<T> Get(const std::string& key) const
      {
         if (m_data.contains(key))
         {
            try
            {
               return m_data.at(key).get<T>();
            }
            catch (const nlohmann::json::exception& e)
            {
               return std::nullopt;
            }
         }
         return std::nullopt;
      }

      template <typename T>
      void Set(const std::string& key, const T& value) { m_data[key] = value; }

      bool Contains(const std::string& key) const { return m_data.contains(key); }

      nlohmann::json& Data() { return m_data; }
      const nlohmann::json& Data() const { return m_data; }

      std::string ToString(int32_t tabSize = 4) const;

      static std::string GenerateJsonString(const std::vector<std::pair<std::string, std::string>>& data, bool prettyPrint = false, int32_t indentLevel = TAB_SIZE);
      static std::wstring GenerateJsonWString(const std::vector<std::pair<std::wstring, std::wstring>>& data, bool prettyPrint = false, int32_t indentLevel = TAB_SIZE);

   private:
      template <typename StringType>
      static StringType EscapeString(const StringType& input) { throw std::exception(); }

      template <>
      static std::string EscapeString(const std::string& input)
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

      template <>
      static std::wstring EscapeString(const std::wstring& input)
      {
         std::wstring output;
         for (wchar_t c : input)
         {
            switch (c)
            {
               case L'\"': output += L"\\\"";  break;
               case L'\\': output += L"\\\\";  break;
               case L'\b': output += L"\\b";   break;
               case L'\f': output += L"\\f";   break;
               case L'\n': output += L"\\n";   break;
               case L'\r': output += L"\\r";   break;
               case L'\t': output += L"\\t";   break;
               default:    output += c;        break;
            }
         }
         return output;
      }

      nlohmann::json m_data;
   };
} // namespace ARC
