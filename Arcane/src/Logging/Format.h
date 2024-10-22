#pragma once

namespace ARC
{
   class Format
   {
   public:
      class Tokens
      {
      public:
         static constexpr std::string_view TIMESTAMP  =  "{timestamp}";
         static constexpr std::string_view LEVEL      =  "{level}";
         static constexpr std::string_view NAME       =  "{name}";
         static constexpr std::string_view MESSAGE    =  "{message}";
         static constexpr std::string_view SOURCE     =  "{source}";
         static constexpr std::string_view FUNCTION   =  "{function}";
         static constexpr std::string_view LINE       =  "{line}";
         
      private:
         Tokens() = delete;
         ~Tokens() = delete;

         Tokens(const Tokens& other) = delete;
         Tokens& operator=(const Tokens& other) = delete;

         Tokens(Tokens&& other) = delete;
         Tokens& operator=(Tokens&& other) = delete;
      };

      static inline std::string DefaultFormatString()
      {
         std::string result;
         result.append(Tokens::TIMESTAMP)
               .append(Tokens::LEVEL)
               .append(Tokens::NAME)
               .append(Tokens::MESSAGE)
               .append(Tokens::SOURCE)
               .append(Tokens::FUNCTION)
               .append(Tokens::LINE);
         return result;
      }

      static inline std::string MinimalFormatString()
      {
         std::string result;
         result.append(Tokens::TIMESTAMP)
               .append(Tokens::LEVEL)
               .append(Tokens::NAME)
               .append(Tokens::MESSAGE);
         return result;
      }

      explicit Format(const std::string& formatStr = DefaultFormatString());
      ~Format() = default;

      std::string operator()(const std::string& level, const std::string& name, const std::string& message, const std::source_location& location);

      void SetFormatString(const std::string& formatStr);

   private:
      std::string GetCurrentTimestamp();
      void ExtractOrderedKeys();

      std::string m_formatString;
      std::unordered_map<std::string, std::string> m_tokenMap;
      std::vector<std::string> m_orderedKeys;
   };
} // namespace ARC
