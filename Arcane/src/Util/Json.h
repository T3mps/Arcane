#pragma once

#include "nlohmann/json.hpp"

namespace ARC
{
   class JSON
   {
   public:
      static constexpr int TAB_SIZE = 4;

      JSON() = default;
      explicit JSON(const std::string& filePath);

      bool Save(const std::string& filePath, int tabSize = 4) const;
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

      std::string ToString(int tabSize = 4) const;

      static std::string GenerateJsonString(const std::vector<std::pair<std::string, std::string>>& data, bool prettyPrint = false, int indentLevel = TAB_SIZE);
      static std::wstring GenerateJsonWString(const std::vector<std::pair<std::wstring, std::wstring>>& data, bool prettyPrint = false, int indentLevel = TAB_SIZE);

   private:
      static std::string EscapeString(const std::string& input);
      static std::wstring EscapeWString(const std::wstring& input);

      nlohmann::json m_data;
   };
} // namespace ARC
