#include "arcpch.h"
#include "StackTrace.h"

#include <DbgHelp.h>

#include "Util/Json.h"

#pragma comment(lib, "Dbghelp.lib")

std::optional<std::string> ARC::StackTrace::Capture(CaptureDef captureDef)
{
   return Capture(captureDef.maxFrames, captureDef.resolveSymbols, captureDef.includeModuleInfo, captureDef.includeLineInfo);
}

std::optional<std::string> ARC::StackTrace::Capture(int maxFrames, 
   bool resolveSymbols,
   bool includeModuleInfo,
   bool includeLineInfo)
{
   std::vector<void*> frames(maxFrames);
   USHORT frameCount = CaptureStackBackTrace(1, maxFrames, frames.data(), nullptr);

   if (frameCount == 0)
      return std::nullopt;

   ARC::JSON json;
   json.Set("frame_count", frameCount);

   auto createFrameJson = [&](USHORT index, DWORD64 address, const std::string& functionName = "", const std::string& moduleName = "", const std::string& fileName = "", int lineNumber = -1) -> ARC::JSON
   {
      ARC::JSON frameJson;
      frameJson.Set("frame_index", index);
      frameJson.Set("address", "0x" + std::to_string(address));
      if (!moduleName.empty() && includeModuleInfo)
         frameJson.Set("module_name", moduleName);
      if (!fileName.empty() && includeLineInfo)
         frameJson.Set("file_name", fileName);
      if (!functionName.empty())
         frameJson.Set("function_name", functionName);
      if (lineNumber != -1 && includeLineInfo)
         frameJson.Set("line_number", lineNumber);
      return frameJson;
   };

   if (!resolveSymbols)
   {
      for (USHORT i = 0; i < frameCount; ++i)
      {
         DWORD64 address = (DWORD64)(frames[i]);
         json.Data()["frames"].push_back(createFrameJson(i, address).Data());
      }
      return json.ToString();
   }

   HANDLE process = GetCurrentProcess();
   SymSetOptions(SYMOPT_LOAD_LINES);
   if (!SymInitialize(process, NULL, TRUE))
      return std::nullopt;

   std::unique_ptr<SYMBOL_INFO, decltype(&free)> symbol(static_cast<SYMBOL_INFO*>(malloc(sizeof(SYMBOL_INFO) + 256)), &free);
   if (!symbol)
      return std::nullopt;

   symbol->MaxNameLen = 255;
   symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

   IMAGEHLP_LINE64 lineInfo = {0};
   lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
   DWORD displacement = 0;

   auto resolveSymbol = [&](DWORD64 address) -> std::string
   {
      if (SymFromAddr(process, address, 0, symbol.get()))
         return symbol->Name;
      return "Unknown Function";
   };

   auto getModuleName = [&](DWORD64 address) -> std::string
   {
      HMODULE module;
      if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)address, &module))
      {
         char moduleName[MAX_PATH];
         if (GetModuleFileNameA(module, moduleName, MAX_PATH))
            return moduleName;
      }
      return "Unknown Module";
   };

   auto resolveLineInfo = [&](DWORD64 address) -> std::pair<std::string, int>
   {
      if (SymGetLineFromAddr64(process, address, &displacement, &lineInfo))
         return {lineInfo.FileName, (int)lineInfo.LineNumber};
      return {"Unknown File", -1};
   };

   for (USHORT i = 0; i < frameCount; ++i)
   {
      DWORD64 address = (DWORD64)(frames[i]);
      std::string functionName = resolveSymbol(address);
      std::string moduleName = includeModuleInfo ? getModuleName(address) : "";
      std::pair<std::string, int> lineInfo = includeLineInfo ? resolveLineInfo(address) : std::make_pair("", -1);

      json.Data()["frames"].push_back(createFrameJson(i, address, functionName, moduleName, lineInfo.first, lineInfo.second).Data());
   }

   SymCleanup(process);
   return json.ToString();
}
