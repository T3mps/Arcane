#pragma once

namespace ARC
{
   class Format
   {
   public:
      class Tokens
      {
      public:
         /*static constexpr std::string_view TIMESTAMP  =  "{timestamp}";
         static constexpr std::string_view LEVEL      =  "{level}";
         static constexpr std::string_view NAME       =  "{name}";
         static constexpr std::string_view MESSAGE    =  "{message}";
         static constexpr std::string_view SOURCE     =  "{source}";
         static constexpr std::string_view FUNCTION   =  "{function}";
         static constexpr std::string_view LINE       =  "{line}";
         */

      private:
         Tokens() = delete;
         ~Tokens() = delete;

         Tokens(const Tokens& other) = delete;
         Tokens& operator=(const Tokens& other) = delete;

         Tokens(Tokens&& other) = delete;
         Tokens& operator=(Tokens&& other) = delete;
      };

      Format();
      ~Format() = default;

      std::string operator()(const std::string_view& level, const std::string& name, const std::string& message, const std::source_location& location);

      //void SetFormatString(const std::string& formatStr);

   private:
      std::string GetCurrentTimestamp();
      static std::string EscapeString(const std::string& input);
   };
} // namespace ARC
