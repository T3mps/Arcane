#include "arcpch.h"
#include "Console.h"

constexpr char NEW_LINE = '\n';

void ARC::Console::Output(const std::string& msg)
{
   std::cout << msg << NEW_LINE;
}

void ARC::Console::Output(const std::string_view& msg)
{
   std::cout << msg << NEW_LINE;
}

void ARC::Console::Output(const char* msg)
{
   std::cout << msg << NEW_LINE;
}

void ARC::Console::OutputW(const std::wstring& msg)
{
   std::wcout << msg << NEW_LINE;
}

void ARC::Console::OutputW(const std::wstring_view& msg)
{
   std::wcout << msg << NEW_LINE;
}

void ARC::Console::OutputW(const wchar_t* msg)
{
   std::wcout << msg << NEW_LINE;
}

void ARC::Console::Error(const std::string& msg)
{
   std::cerr << msg << NEW_LINE;
}

void ARC::Console::Error(const std::string_view& msg)
{
   std::cerr << msg << NEW_LINE;
}

void ARC::Console::Error(const char* msg)
{
   std::cerr << msg << NEW_LINE;
}

void ARC::Console::ErrorW(const std::wstring& msg)
{
   std::wcerr << msg << NEW_LINE;
}

void ARC::Console::ErrorW(const std::wstring_view& msg)
{
   std::wcerr << msg << NEW_LINE;
}

void ARC::Console::ErrorW(const wchar_t* msg)
{
   std::wcerr << msg << NEW_LINE;
}

void ARC::Console::Log(const std::string& msg)
{
   std::clog << msg << NEW_LINE;
}

void ARC::Console::Log(const std::string_view& msg)
{
   std::clog << msg << NEW_LINE;
}

void ARC::Console::Log(const char* msg)
{
   std::clog << msg << NEW_LINE;
}

void ARC::Console::LogW(const std::wstring& msg)
{
   std::wclog << msg << NEW_LINE;
}

void ARC::Console::LogW(const std::wstring_view& msg)
{
   std::wclog << msg << NEW_LINE;
}

void ARC::Console::LogW(const wchar_t* msg)
{
   std::wclog << msg << NEW_LINE;
}

void ARC::Console::Flush(std::ostream& stream)
{
   std::flush(stream);
}
