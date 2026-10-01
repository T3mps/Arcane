#include "Project/OsShell.hpp"

#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW
#pragma comment(lib, "shell32.lib")
#endif

namespace Arcane::Editor::OsShell
{
    namespace
    {
        bool Exists(const std::filesystem::path& path)
        {
            std::error_code ec;
            return !path.empty() && std::filesystem::exists(path, ec);
        }

        enum class Verb { Open, OpenWith, Select };

        // The ONE place this editor calls the Windows shell.
        ShellResult Run(Verb verb, const std::filesystem::path& path)
        {
#ifdef _WIN32
            const std::wstring file = path.wstring();
            HINSTANCE h = nullptr;
            switch (verb)
            {
            case Verb::Open:
                h = ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case Verb::OpenWith:
                h = ShellExecuteW(nullptr, L"openas", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case Verb::Select:
            {
                const std::wstring args = ExplorerSelectArgs(path);
                h = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                break;
            }
            }
            return ClassifyShellExecute(reinterpret_cast<std::intptr_t>(h));
#else
            (void)verb; (void)path;
            return ShellResult::Unsupported;
#endif
        }
    }

    std::wstring ExplorerSelectArgs(const std::filesystem::path& path)
    {
        std::filesystem::path native = path;
        native.make_preferred();
        return L"/select,\"" + native.wstring() + L"\"";
    }

    ShellResult ClassifyShellExecute(std::intptr_t code)
    {
        if (code > 32) return ShellResult::Ok;
        if (code == 2 || code == 3) return ShellResult::NotFound;   // SE_ERR_FNF / SE_ERR_PNF
        if (code == 31) return ShellResult::NoHandler;              // SE_ERR_NOASSOC
        return ShellResult::Failed;
    }

    std::string_view Describe(ShellResult r)
    {
        switch (r)
        {
        case ShellResult::Ok:          return "Done";
        case ShellResult::NotFound:    return "File not found on this machine";
        case ShellResult::NoHandler:   return "No program is associated with this file type";
        case ShellResult::Failed:      return "The shell refused the request";
        case ShellResult::Unsupported: return "Not supported on this platform";
        }
        return "Unknown shell result";
    }

    ShellResult ShellOpen(const std::filesystem::path& path)
    {
        if (!Exists(path)) return ShellResult::NotFound;
        return Run(Verb::Open, path);
    }

    ShellResult ShowInExplorer(const std::filesystem::path& path)
    {
        if (!Exists(path)) return ShellResult::NotFound;
        return Run(Verb::Select, path);
    }

    // Today's AssetPathAction sequence (EditorAppFrame.cpp:196-213): "open"
    // first; an extension with no association (.arcinput/.json on a stock
    // machine) has no open handler, so fall back to the Open With picker.
    ShellResult OpenAsText(const std::filesystem::path& path)
    {
        if (!Exists(path)) return ShellResult::NotFound;
        const ShellResult opened = Run(Verb::Open, path);
        if (opened == ShellResult::Ok || opened == ShellResult::Unsupported)
            return opened;
        return Run(Verb::OpenWith, path);
    }
}
