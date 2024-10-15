#pragma once

class Loom
{
public:
   static std::unique_ptr<Loom> Create(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int32_t nCmdShow);

   Loom(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int32_t nCmdShow);
   ~Loom();

private:
   void ProcessCommandLineArgs(LPWSTR lpCmdLine, std::wstring& dllName) const;
   void ProcessDLL(HINSTANCE hInstance, int32_t nCmdShow, const std::wstring& dllName) const;

   mutable HMODULE m_hModule;
};
