#pragma once

#include "Util/StackTrace.h"
#include "Util/UUID.h"

namespace ARC
{
   enum class ErrorCode : uint64_t
   {
      None = 0,
      InvalidArgument,
      NullPointer,
      IndexOutOfBounds,
      OperationFailed,
      NotImplemented,
      UnsupportedOperation,
      Timeout,
      UnknownError
   };

   class Error
   {
   public:
      static Error Create(ErrorCode code, const std::string_view message, const std::source_location location = std::source_location::current())
      {
         std::optional<std::string> stackTrace = std::nullopt;
#ifdef ARC_BUILD_DEBUG
         stackTrace = StackTrace::Capture();
#endif
         return Error(code, message.data(), stackTrace, location.file_name(), location.line());
      }

      ErrorCode Code()                                const { return m_code; }
      const std::string& Message()                    const { return m_message; }
      const std::optional<std::string>& StackTrace()  const { return m_stackTrace; }
      const std::string& GetFile()                    const { return m_file; }
      int GetLine()                                   const { return m_line; }
      const UUID& GetUUID()                           const { return m_uuid; }

      std::string ToString() const
      {
         return "Error: " + m_message + " (" + std::to_string(static_cast<uint64_t>(m_code)) + "), in file: " + m_file + " at line: " + std::to_string(m_line) + (m_stackTrace ? "\nStack Trace:\n" + *m_stackTrace : "");
      }

   private:
      Error(ErrorCode code, const std::string& msg, std::optional<std::string> trace = std::nullopt, std::string file = "", int line = 0) :
         m_code(code),
         m_message(msg),
         m_stackTrace(trace),
         m_file(file),
         m_line(line),
         m_uuid()
      {}

      ErrorCode m_code;
      std::string m_message;
      std::optional<std::string> m_stackTrace;
      std::string m_file;
      int m_line;
      UUID m_uuid;
   };
} // namespace ARC
