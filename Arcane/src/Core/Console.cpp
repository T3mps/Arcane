#include "arcpch.h"
#include "Console.h"

std::once_flag ARC::Console::m_initFlag;
HANDLE ARC::Console::m_hConsole;

bool ARC::Console::Initialize(const std::wstring& title, int32_t width, int32_t height)
{
   static bool initialized = false;
   std::call_once(m_initFlag, [&]()
   {
      if (AllocConsole())
      {
         freopen_s((FILE**) stdout, "CONOUT$",  "w", stdout);
         freopen_s((FILE**) stderr, "CONOUT$",  "w", stderr);
         freopen_s((FILE**) stdin,  "CONIN$",   "r", stdin);
         m_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

         if (!title.empty())
            SetConsoleTitle(title.c_str());

         CONSOLE_SCREEN_BUFFER_INFO csbi;
         GetConsoleScreenBufferInfo(m_hConsole, &csbi);
         SMALL_RECT windowSize = { 0, 0, static_cast<SHORT>(width - 1), static_cast<SHORT>(height - 1) };
         SetConsoleWindowInfo(m_hConsole, TRUE, &windowSize);
         COORD bufferSize = { static_cast<SHORT>(width), static_cast<SHORT>(height) };
         SetConsoleScreenBufferSize(m_hConsole, bufferSize);

         initialized = true;
      }
   });
   return initialized;
}

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

void ARC::Console::SetTitle(const std::wstring& title)
{
   if (m_hConsole)
      SetConsoleTitle(title.c_str());
}

void ARC::Console::SetTextColor(WORD color)
{
   if (m_hConsole)
      SetConsoleTextAttribute(m_hConsole, color);
}
