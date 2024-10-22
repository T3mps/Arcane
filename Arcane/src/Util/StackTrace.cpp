#include "arcpch.h"
#include "StackTrace.h"

#ifdef ARC_PLATFORM_WINDOWS
   #include "DbgHelp.h"
   #pragma comment(lib, "Dbghelp.lib")
#else
   #include "cxxabi.h"
   #include "dlfcn.h"
   #include "execinfo.h"
#endif

#include "Util/Serialization/JSON.h"

#ifdef ARC_PLATFORM_WINDOWS
   std::optional<std::string> ARC::StackTrace::Capture(CaptureInfo CaptureInfo)
   {
      return Capture(CaptureInfo.maxFrames, CaptureInfo.resolveSymbols, CaptureInfo.includeModuleInfo, CaptureInfo.includeLineInfo);
   }

   std::optional<std::string> ARC::StackTrace::Capture(int32_t maxFrames, 
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

      auto createFrameJson = [&](USHORT index, DWORD64 address, const std::string& functionName = "", const std::string& moduleName = "", const std::string& fileName = "", int32_t lineNumber = -1) -> ARC::JSON
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

      auto resolveLineInfo = [&](DWORD64 address) -> std::pair<std::string, int32_t>
         {
            if (SymGetLineFromAddr64(process, address, &displacement, &lineInfo))
               return {lineInfo.FileName, (int32_t)lineInfo.LineNumber};
            return {"Unknown File", -1};
         };

      for (USHORT i = 0; i < frameCount; ++i)
      {
         DWORD64 address = (DWORD64)(frames[i]);
         std::string functionName = resolveSymbol(address);
         std::string moduleName = includeModuleInfo ? getModuleName(address) : "";
         std::pair<std::string, int32_t> lineInfo = includeLineInfo ? resolveLineInfo(address) : std::make_pair("", -1);

         json.Data()["frames"].push_back(createFrameJson(i, address, functionName, moduleName, lineInfo.first, lineInfo.second).Data());
      }

      SymCleanup(process);
      return json.ToString();
   }
#else
   std::optional<std::string> ARC::StackTrace::Capture(CaptureDef captureDef)
   {
      return Capture(captureDef.maxFrames, captureDef.resolveSymbols, captureDef.includeModuleInfo, captureDef.includeLineInfo);
   }

   std::optional<std::string> ARC::StackTrace::Capture(int32_t maxFrames, 
      bool resolveSymbols, 
      bool includeModuleInfo, 
      bool includeLineInfo)
   {
      std::vector<void*> frames(maxFrames);
      int32_t frameCount = backtrace(frames.data(), maxFrames);

      if (frameCount == 0)
         return std::nullopt;

      char** symbols = backtrace_symbols(frames.data(), frameCount);
      if (!symbols)
         return std::nullopt;

      std::ostringstream jsonStream;
      jsonStream << "{ \"frame_count\": " << frameCount << ", \"frames\": [";

      for (int32_t i = 0; i < frameCount; ++i)
      {
         Dl_info info;
         if (dladdr(frames[i], &info) && resolveSymbols)
         {
            const char* symbolName = info.dli_sname ? info.dli_sname : "Unknown Function";
            const char* moduleName = includeModuleInfo && info.dli_fname ? info.dli_fname : "Unknown Module";

            int32_t status = 0;
            char* demangledName = abi::__cxa_demangle(symbolName, nullptr, nullptr, &status);
            std::string functionName = (status == 0 && demangledName) ? demangledName : symbolName;

            if (demangledName)
               free(demangledName);

            jsonStream << "{ \"frame_index\": " << i
               << ", \"address\": \"" << frames[i] << "\""
               << ", \"module_name\": \"" << moduleName << "\""
               << ", \"function_name\": \"" << functionName << "\"";

            if (includeLineInfo && info.dli_saddr)
            {
               intptr_t offset = (intptr_t)frames[i] - (intptr_t)info.dli_fbase;
               jsonStream << ", \"line_offset\": " << offset;
            }

            jsonStream << " }";
         }
         else
         {
            jsonStream << "{ \"frame_index\": " << i
               << ", \"address\": \"" << frames[i] << "\""
               << ", \"function_name\": \"Unknown Function\""
               << " }";
         }

         if (i < frameCount - 1)
            jsonStream << ", ";
      }

      jsonStream << "] }";

      free(symbols);
      return jsonStream.str();
   }
#endif
