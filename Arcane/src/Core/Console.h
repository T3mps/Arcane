#pragma once

#include "Util/Singleton.h"

namespace ARC
{
   class Console : public Singleton<Console>
   {
   public:
      static constexpr char NEW_LINE = '\n';

      static bool Initialize(const std::wstring& title = L"", int width = 96, int height = 64);

      static void Output(const std::string& msg);
      static void Output(const std::string_view& msg);
      static void Output(const char* msg);
      static void OutputW(const std::wstring& msg);
      static void OutputW(const std::wstring_view& msg);
      static void OutputW(const wchar_t* msg);

      static void Error(const std::string& msg);
      static void Error(const std::string_view& msg);
      static void Error(const char* msg);
      static void ErrorW(const std::wstring& msg);
      static void ErrorW(const std::wstring_view& msg);
      static void ErrorW(const wchar_t* msg);

      static void Log(const std::string& msg);
      static void Log(const std::string_view& msg);
      static void Log(const char* msg);
      static void LogW(const std::wstring& msg);
      static void LogW(const std::wstring_view& msg);
      static void LogW(const wchar_t* msg);

      static void Flush(std::ostream& stream);

      static void SetTitle(const std::wstring& title);
      static void SetTextColor(WORD color);

   private:
      Console();
      virtual ~Console() = default;

      Console(const Console&) = delete;
      Console& operator=(const Console&) = delete;

      static std::once_flag m_initFlag;
      static HANDLE m_hConsole;

      friend class Singleton<Console>;
   };
}
