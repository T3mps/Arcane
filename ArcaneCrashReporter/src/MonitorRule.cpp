#include "MonitorRule.hpp"
#include <cstdio>

namespace Arcane::Reporter
{
    MonitorVerdict ClassifyHostExit(bool sessionRecordPresent, bool crashPathSpoke)
    {
        if (!sessionRecordPresent) return MonitorVerdict::Silent;   // the host recorded its clean exit
        if (crashPathSpoke)        return MonitorVerdict::Silent;   // its reporter (or hang window) owns the view
        return MonitorVerdict::Synthesize;
    }

    std::string_view NtStatusName(std::uint32_t code)
    {
        switch (code)
        {
        case 0xC0000005u: return "STATUS_ACCESS_VIOLATION";
        case 0xC0000006u: return "STATUS_IN_PAGE_ERROR";
        case 0xC0000008u: return "STATUS_INVALID_HANDLE";
        case 0xC000000Du: return "STATUS_INVALID_PARAMETER";
        case 0xC0000017u: return "STATUS_NO_MEMORY";
        case 0xC000001Du: return "STATUS_ILLEGAL_INSTRUCTION";
        case 0xC0000094u: return "STATUS_INTEGER_DIVIDE_BY_ZERO";
        case 0xC00000FDu: return "STATUS_STACK_OVERFLOW";
        case 0xC000012Du: return "STATUS_FATAL_MEMORY_EXHAUSTION";
        case 0xC0000135u: return "STATUS_DLL_NOT_FOUND";
        case 0xC000013Au: return "STATUS_CONTROL_C_EXIT";
        case 0xC0000142u: return "STATUS_DLL_INIT_FAILED";
        case 0xC0000374u: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN";   // also every __fastfail
        case 0xC000041Du: return "STATUS_FATAL_USER_CALLBACK_EXCEPTION";
        case 0xC0000420u: return "STATUS_ASSERTION_FAILURE";
        case 0xC0000602u: return "STATUS_FAIL_FAST_EXCEPTION";
        case 0x80000003u: return "STATUS_BREAKPOINT";
        case 0xE06D7363u: return "MSVC_CPP_EXCEPTION";
        default:          return {};
        }
    }

    std::string AbnormalExitReason(std::uint32_t code)
    {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08X", code);
        std::string s = std::string("abnormal-exit: ") + hex;
        const std::string_view name = NtStatusName(code);
        if (!name.empty()) { s += " "; s += name; }
        return s;
    }

    bool IsHostReportName(std::string_view fileName, std::string_view app, std::uint32_t pid)
    {
        if (app.empty()) return false;
        const std::string prefix = std::string(app) + "-";
        const std::string suffix = "-pid" + std::to_string(pid) + ".arcdiag";
        return fileName.size() >= prefix.size() + suffix.size()
            && fileName.substr(0, prefix.size()) == prefix
            && fileName.substr(fileName.size() - suffix.size()) == suffix;
    }

    bool ReportSpeaksForExit(int reportExitCode)
    {
        return reportExitCode != 0;
    }

    bool HangWindowOwnsExit(bool unrecoveredHangReporterAlive, std::uint32_t hostExitCode)
    {
        // Mirrors HangSession.cpp's HostExited row, literal for literal (this
        // TU stays std-only, so Diagnostics::ExitCode is spelled out): 10
        // crashed, 12 exit sentinel, 13 crash in the crash path close the
        // hang window; everything else keeps it up as the crash view.
        if (!unrecoveredHangReporterAlive) return false;
        return !(hostExitCode == 10 || hostExitCode == 12 || hostExitCode == 13);
    }
}
