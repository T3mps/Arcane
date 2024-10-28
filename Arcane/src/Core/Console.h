#pragma once

namespace ARC
{
   namespace Console
   {
      void Output(const std::string& msg);
      void Output(const std::string_view& msg);
      void Output(const char* msg);
      void OutputW(const std::wstring& msg);
      void OutputW(const std::wstring_view& msg);
      void OutputW(const wchar_t* msg);

      void Error(const std::string& msg);
      void Error(const std::string_view& msg);
      void Error(const char* msg);
      void ErrorW(const std::wstring& msg);
      void ErrorW(const std::wstring_view& msg);
      void ErrorW(const wchar_t* msg);

      void Log(const std::string& msg);
      void Log(const std::string_view& msg);
      void Log(const char* msg);
      void LogW(const std::wstring& msg);
      void LogW(const std::wstring_view& msg);
      void LogW(const wchar_t* msg);

      void Flush(std::ostream& stream);
   } // namespace Console
} // namespace ARC
