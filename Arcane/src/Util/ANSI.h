#pragma once

namespace ARC::AnsiColor
{
   template <typename StringType>
   constexpr StringType RESET();

   template <>
   constexpr std::string RESET<std::string>() { return "\033[0m"; }
   template <>
   constexpr std::wstring RESET<std::wstring>() { return L"\033[0m"; }

   // Foreground color codes
   template <typename StringType>
   constexpr StringType FG_BLACK();

   template <>
   constexpr std::string FG_BLACK<std::string>() { return "\033[30m"; }
   template <>
   constexpr std::wstring FG_BLACK<std::wstring>() { return L"\033[30m"; }

   template <typename StringType>
   constexpr StringType FG_RED();

   template <>
   constexpr std::string FG_RED<std::string>() { return "\033[31m"; }
   template <>
   constexpr std::wstring FG_RED<std::wstring>() { return L"\033[31m"; }

   template <typename StringType>
   constexpr StringType FG_GREEN();

   template <>
   constexpr std::string FG_GREEN<std::string>() { return "\033[32m"; }
   template <>
   constexpr std::wstring FG_GREEN<std::wstring>() { return L"\033[32m"; }

   template <typename StringType>
   constexpr StringType FG_YELLOW();

   template <>
   constexpr std::string FG_YELLOW<std::string>() { return "\033[33m"; }
   template <>
   constexpr std::wstring FG_YELLOW<std::wstring>() { return L"\033[33m"; }

   template <typename StringType>
   constexpr StringType FG_BLUE();

   template <>
   constexpr std::string FG_BLUE<std::string>() { return "\033[34m"; }
   template <>
   constexpr std::wstring FG_BLUE<std::wstring>() { return L"\033[34m"; }

   template <typename StringType>
   constexpr StringType FG_MAGENTA();

   template <>
   constexpr std::string FG_MAGENTA<std::string>() { return "\033[35m"; }
   template <>
   constexpr std::wstring FG_MAGENTA<std::wstring>() { return L"\033[35m"; }

   template <typename StringType>
   constexpr StringType FG_CYAN();

   template <>
   constexpr std::string FG_CYAN<std::string>() { return "\033[36m"; }
   template <>
   constexpr std::wstring FG_CYAN<std::wstring>() { return L"\033[36m"; }

   template <typename StringType>
   constexpr StringType FG_WHITE();

   template <>
   constexpr std::string FG_WHITE<std::string>() { return "\033[37m"; }
   template <>
   constexpr std::wstring FG_WHITE<std::wstring>() { return L"\033[37m"; }

   // Bright Foreground color codes
   template <typename StringType>
   constexpr StringType BRIGHT_BLACK();

   template <>
   constexpr std::string BRIGHT_BLACK<std::string>() { return "\033[90m"; }
   template <>
   constexpr std::wstring BRIGHT_BLACK<std::wstring>() { return L"\033[90m"; }

   template <typename StringType>
   constexpr StringType BRIGHT_RED();

   template <>
   constexpr std::string BRIGHT_RED<std::string>() { return "\033[91m"; }
   template <>
   constexpr std::wstring BRIGHT_RED<std::wstring>() { return L"\033[91m"; }

   template <typename StringType>
   constexpr StringType BRIGHT_GREEN();

   template <>
   constexpr std::string BRIGHT_GREEN<std::string>() { return "\033[92m"; }
   template <>
   constexpr std::wstring BRIGHT_GREEN<std::wstring>() { return L"\033[92m"; }

   template <typename StringType>
   constexpr StringType BRIGHT_YELLOW();

   template <>
   constexpr std::string BRIGHT_YELLOW<std::string>() { return "\033[93m"; }
   template <>
   constexpr std::wstring BRIGHT_YELLOW<std::wstring>() { return L"\033[93m"; }

   template <typename StringType>
   constexpr StringType BRIGHT_BLUE();

   template <>
   constexpr std::string BRIGHT_BLUE<std::string>() { return "\033[94m"; }
   template <>
   constexpr std::wstring BRIGHT_BLUE<std::wstring>() { return L"\033[94m"; }

   template <typename StringType>
   constexpr StringType BRIGHT_MAGENTA();

   template <>
   constexpr std::string BRIGHT_MAGENTA<std::string>() { return "\033[95m"; }
   template <>
   constexpr std::wstring BRIGHT_MAGENTA<std::wstring>() { return L"\033[95m"; }

   template <typename StringType>
   constexpr StringType BRIGHT_CYAN();

   template <>
   constexpr std::string BRIGHT_CYAN<std::string>() { return "\033[96m"; }
   template <>
   constexpr std::wstring BRIGHT_CYAN<std::wstring>() { return L"\033[96m"; }

   template <typename StringType>
   constexpr StringType BRIGHT_WHITE();

   template <>
   constexpr std::string BRIGHT_WHITE<std::string>() { return "\033[97m"; }
   template <>
   constexpr std::wstring BRIGHT_WHITE<std::wstring>() { return L"\033[97m"; }

   // Background color codes
   template <typename StringType>
   constexpr StringType BG_BLACK();

   template <>
   constexpr std::string BG_BLACK<std::string>() { return "\033[40m"; }
   template <>
   constexpr std::wstring BG_BLACK<std::wstring>() { return L"\033[40m"; }

   template <typename StringType>
   constexpr StringType BG_RED();

   template <>
   constexpr std::string BG_RED<std::string>() { return "\033[41m"; }
   template <>
   constexpr std::wstring BG_RED<std::wstring>() { return L"\033[41m"; }

   template <typename StringType>
   constexpr StringType BG_GREEN();

   template <>
   constexpr std::string BG_GREEN<std::string>() { return "\033[42m"; }
   template <>
   constexpr std::wstring BG_GREEN<std::wstring>() { return L"\033[42m"; }

   template <typename StringType>
   constexpr StringType BG_YELLOW();

   template <>
   constexpr std::string BG_YELLOW<std::string>() { return "\033[43m"; }
   template <>
   constexpr std::wstring BG_YELLOW<std::wstring>() { return L"\033[43m"; }

   template <typename StringType>
   constexpr StringType BG_BLUE();

   template <>
   constexpr std::string BG_BLUE<std::string>() { return "\033[44m"; }
   template <>
   constexpr std::wstring BG_BLUE<std::wstring>() { return L"\033[44m"; }

   template <typename StringType>
   constexpr StringType BG_MAGENTA();

   template <>
   constexpr std::string BG_MAGENTA<std::string>() { return "\033[45m"; }
   template <>
   constexpr std::wstring BG_MAGENTA<std::wstring>() { return L"\033[45m"; }

   template <typename StringType>
   constexpr StringType BG_CYAN();

   template <>
   constexpr std::string BG_CYAN<std::string>() { return "\033[46m"; }
   template <>
   constexpr std::wstring BG_CYAN<std::wstring>() { return L"\033[46m"; }

   template <typename StringType>
   constexpr StringType BG_WHITE();

   template <>
   constexpr std::string BG_WHITE<std::string>() { return "\033[47m"; }
   template <>
   constexpr std::wstring BG_WHITE<std::wstring>() { return L"\033[47m"; }

   // Bright Background color codes
   template <typename StringType>
   constexpr StringType BG_BRIGHT_BLACK();

   template <>
   constexpr std::string BG_BRIGHT_BLACK<std::string>() { return "\033[100m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_BLACK<std::wstring>() { return L"\033[100m"; }

   template <typename StringType>
   constexpr StringType BG_BRIGHT_RED();

   template <>
   constexpr std::string BG_BRIGHT_RED<std::string>() { return "\033[101m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_RED<std::wstring>() { return L"\033[101m"; }

   template <typename StringType>
   constexpr StringType BG_BRIGHT_GREEN();

   template <>
   constexpr std::string BG_BRIGHT_GREEN<std::string>() { return "\033[102m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_GREEN<std::wstring>() { return L"\033[102m"; }

   template <typename StringType>
   constexpr StringType BG_BRIGHT_YELLOW();

   template <>
   constexpr std::string BG_BRIGHT_YELLOW<std::string>() { return "\033[103m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_YELLOW<std::wstring>() { return L"\033[103m"; }

   template <typename StringType>
   constexpr StringType BG_BRIGHT_BLUE();

   template <>
   constexpr std::string BG_BRIGHT_BLUE<std::string>() { return "\033[104m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_BLUE<std::wstring>() { return L"\033[104m"; }

   template <typename StringType>
   constexpr StringType BG_BRIGHT_MAGENTA();

   template <>
   constexpr std::string BG_BRIGHT_MAGENTA<std::string>() { return "\033[105m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_MAGENTA<std::wstring>() { return L"\033[105m"; }

   template <typename StringType>
   constexpr StringType BG_BRIGHT_CYAN();

   template <>
   constexpr std::string BG_BRIGHT_CYAN<std::string>() { return "\033[106m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_CYAN<std::wstring>() { return L"\033[106m"; }

   template <typename StringType>
   constexpr StringType BG_BRIGHT_WHITE();

   template <>
   constexpr std::string BG_BRIGHT_WHITE<std::string>() { return "\033[107m"; }
   template <>
   constexpr std::wstring BG_BRIGHT_WHITE<std::wstring>() { return L"\033[107m"; }

   // Bold and Underline text
   template <typename StringType>
   constexpr StringType BOLD();

   template <>
   constexpr std::string BOLD<std::string>() { return "\033[1m"; }
   template <>
   constexpr std::wstring BOLD<std::wstring>() { return L"\033[1m"; }

   template <typename StringType>
   constexpr StringType UNDERLINE();

   template <>
   constexpr std::string UNDERLINE<std::string>() { return "\033[4m"; }
   template <>
   constexpr std::wstring UNDERLINE<std::wstring>() { return L"\033[4m"; }

   template <typename StringType>
   void FormatMessage(StringType& msg, const StringType& fgColor, const StringType& bgColor = BG_BLACK<StringType>()) { msg = fgColor + bgColor + msg + RESET<StringType>(); }
} // namespace ARC::AnsiColor
