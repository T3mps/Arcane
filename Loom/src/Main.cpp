#include "arcpch.h"
#include "Loom.h"

int32_t APIENTRY wWinMain(_In_ HINSTANCE hInstance,
   _In_opt_ HINSTANCE hPrevInstance,
   _In_ LPWSTR    lpCmdLine,
   _In_ int32_t   nCmdShow)
{
   Loom::Create(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
   return 0;
}
