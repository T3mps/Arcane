#pragma once

class Loom
{
public:
   Loom(int argc, char* argv[]);
   ~Loom();

private:
   void ProcessCommandLineArgs(int argc, char* argv[], std::wstring& moduleName) const;
   void ProcessModule(const std::wstring& moduleName) const;

   mutable HMODULE m_hModule;
};
