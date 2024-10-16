#pragma once

namespace ARC
{
   namespace StackTrace
   {
      struct CaptureInfo
      {
         int maxFrames           = 16;
         bool resolveSymbols     = true;
         bool includeModuleInfo  = true;
         bool includeLineInfo    = true;
      };

      [[nodiscard]] std::optional<std::string> Capture(CaptureInfo CaptureInfo = {});
      [[nodiscard]] std::optional<std::string> Capture(int maxFrames, bool resolveSymbols = true, bool includeModuleInfo = true, bool includeLineInfo = true);
   } // namespace StackTrace
} // namespace ARC
