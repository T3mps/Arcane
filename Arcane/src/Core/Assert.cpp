#include "arcpch.h"
#include "Assert.h"

#include "Console.h"
#include "Util/ANSI.h"

void ARC::Asserts::DefaultAssertHandler(std::string_view message, std::optional<std::string> stackTrace, std::source_location location)
{
   std::ostringstream logMessage;
   logMessage << "Assertion failed: " << message
      << " at " << location.file_name() << ":" << location.line()
      << " in function: " << location.function_name();

   if (stackTrace)
      logMessage << "\nStack Trace:\n" + *stackTrace;

   std::string output = logMessage.str();

   AnsiColor::FormatMessage(output, AnsiColor::FG_WHITE<std::string>(), AnsiColor::BG_RED<std::string>());

   Console::Error(output);

   EXCEPTION_POINTERS* exceptionInfo = nullptr;
   ARC::DumpGenerator::MiniDump(exceptionInfo, ARC::DumpGenerator::DumpType::MiniDumpWithFullMemory);

   ARC_DEBUGBREAK();
}

void ARC::Asserts::OnAssertionFailure(std::string_view message, std::optional<std::string_view> context, std::source_location location)
{
#ifdef ARC_ENABLE_ASSERTS
   std::ostringstream finalMessage;
   if (context)
      finalMessage << "\"" << *context << "\", ";

   finalMessage << "'" << message << "'";

   std::optional<std::string> stackTrace = StackTrace::Capture();
   s_assertHandler(finalMessage.str(), stackTrace, location);
#endif
}
