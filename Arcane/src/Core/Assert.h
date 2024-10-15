#pragma once

#include "Util/DumpGenerator.h"
#include "Util/StackTrace.h"

namespace ARC
{
   namespace Asserts
   {
      using AssertHandler = std::function<void(std::string_view message, std::optional<std::string> stackTrace, std::source_location location)>;

      void DefaultAssertHandler(std::string_view message, std::optional<std::string> stackTrace, std::source_location location = std::source_location::current());

      void OnAssertionFailure(std::string_view message, std::optional<std::string_view> context = std::nullopt, std::source_location location = std::source_location::current());
   
      inline static AssertHandler s_assertHandler = DefaultAssertHandler;

      inline static void SetAssertHandler(AssertHandler handler)  { s_assertHandler = handler; }
   } // namespace Asserts
} // namespace ARC

#ifdef ARC_ENABLE_ASSERTS
   #define ARC_ASSERT(expr, ...) \
      do { \
         const bool assertCondition = (expr); \
         if (!assertCondition) \
            ::ARC::Asserts::OnAssertionFailure(ARC_STRINGIFY_MACRO(expr), __VA_ARGS__, std::source_location::current()); \
      } while (false)
#else
   #define ASSERT(expr)
#endif
