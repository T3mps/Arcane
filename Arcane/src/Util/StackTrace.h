#pragma once

namespace ARC::StackTrace
{
   struct CaptureInfo
   {
      int32_t maxFrames       = 16;
      bool resolveSymbols     = true;
      bool includeModuleInfo  = true;
      bool includeLineInfo    = true;
   };

   [[nodiscard]] std::optional<std::string> Capture(CaptureInfo CaptureInfo = {});
   [[nodiscard]] std::optional<std::string> Capture(int32_t maxFrames, bool resolveSymbols = true, bool includeModuleInfo = true, bool includeLineInfo = true);
} // namespace ARC
