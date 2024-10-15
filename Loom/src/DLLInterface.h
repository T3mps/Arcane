#pragma once

typedef void (*EntryPointFunc)(HINSTANCE, int32_t);

HMODULE LoadDLL(const std::wstring& dllName, const std::wstring& funcName, EntryPointFunc& runGameFunc);
void RunDLL(EntryPointFunc runGameFunc, HINSTANCE hInstance, int32_t nCmdShow);
void UnloadDLL(HMODULE hModule);

std::wstring GetErrorMessage(DWORD errorCode);

extern std::mutex dllMutex;
