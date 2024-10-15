#pragma once

namespace ARC
{
   class Format
   {
   public:
      class Tokens
      {
      public:
         static constexpr std::wstring_view TIMESTAMP = L"{timestamp}";
         static constexpr std::wstring_view LEVEL     = L"{level}";
         static constexpr std::wstring_view NAME      = L"{name}";
         static constexpr std::wstring_view MESSAGE   = L"{message}";
         static constexpr std::wstring_view SOURCE    = L"{source}";
         static constexpr std::wstring_view FUNCTION  = L"{function}";
         static constexpr std::wstring_view LINE      = L"{line}";
         
      private:
         Tokens() = delete;
         ~Tokens() = delete;
      };

      static inline std::wstring DefaultFormatString()
      {
         std::wstring result;
         result.append(Tokens::TIMESTAMP)
               .append(Tokens::LEVEL)
               .append(Tokens::NAME)
               .append(Tokens::MESSAGE)
               .append(Tokens::SOURCE)
               .append(Tokens::FUNCTION)
               .append(Tokens::LINE);
         return result;
      }

      static inline std::wstring MinimalFormatString()
      {
         std::wstring result;
         result.append(Tokens::TIMESTAMP)
               .append(Tokens::LEVEL)
               .append(Tokens::NAME)
               .append(Tokens::MESSAGE);
         return result;
      }

      explicit Format(const std::wstring& formatStr = DefaultFormatString());
      ~Format() = default;

      std::wstring operator()(const std::wstring& level, const std::wstring& name, const std::wstring& message, const std::source_location& location);

      void SetFormatString(const std::wstring& formatStr);

   private:
      std::wstring GetCurrentTimestamp();
      void ExtractOrderedKeys();

      std::wstring m_formatString;
      std::unordered_map<std::wstring, std::wstring> m_tokenMap;
      std::vector<std::wstring> m_orderedKeys;
   };
}
