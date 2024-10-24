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

   private:
      
      nlohmann::json m_data;
   };
} // namespace ARC
