#include "Symbolizer.hpp"
#include "Win32Text.hpp"

#include <Arcane/Base/Engine.hpp>

#include <dbgeng.h>
#include <DbgHelp.h>    // the SYMOPT_* constants; UE includes it after dbgeng.h the same way (WindowsPlatformStackWalkExt.cpp:16-17)
#include <wrl/client.h>

#include <cstddef>
#include <cstdio>
#include <utility>

namespace Arcane::Reporter
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        std::string Hex(HRESULT hr)
        {
            char b[16];
            std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
            return b;
        }

        // EndSession on EVERY exit from the point the dump opened, including
        // the failure paths. The ComPtrs below would release the client on
        // their own, but "release the last reference and hope" is not the same
        // as detaching the session -- dbgeng keeps the dump file mapped until
        // the session ends, and a reporter that leaves it mapped would keep a
        // lock on the one artifact a human is about to open in windbg.
        struct SessionEnder
        {
            IDebugClient5* client = nullptr;
            ~SessionEnder() { if (client) client->EndSession(DEBUG_END_ACTIVE_DETACH); }

            SessionEnder()                               = default;
            SessionEnder(const SessionEnder&)            = delete;
            SessionEnder& operator=(const SessionEnder&) = delete;
        };

        // UE's rule (WindowsPlatformStackWalkExt.cpp:183-243): the symbol path
        // is the directory of EVERY module the dump names, plus
        // _NT_SYMBOL_PATH, and the image path is the same set. For our layout
        // that puts the host's directory (ArcaneCore.dll/ArcaneClient.dll
        // beside the exe) and the project's Binaries/ (the game module, whose
        // PDB arcbuild leaves beside it) on the path without anyone naming
        // them. The reporter's own directory joins too: it is staged beside
        // the host (spec §12 item 2).
        std::wstring ModuleDirectories(IDebugSymbols3* symbols)
        {
            std::wstring path;
            ULONG loaded = 0, unloaded = 0;
            if (FAILED(symbols->GetNumberModules(&loaded, &unloaded))) return path;
            for (ULONG i = 0; i < loaded; ++i)
            {
                ULONG64 base = 0;
                if (FAILED(symbols->GetModuleByIndex(i, &base))) continue;
                wchar_t image[1024];
                ULONG   size = 0;
                if (FAILED(symbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, i, base, image, 1024, &size)) || size <= 1) continue;
                std::wstring dir(image);
                const std::size_t slash = dir.find_last_of(L"/\\");
                if (slash == std::wstring::npos) continue;
                dir.resize(slash);
                if (path.find(dir + L";") == std::wstring::npos) { path += dir; path += L";"; }
            }
            return path;
        }

        std::wstring DefaultSymbolPath(IDebugSymbols3* symbols)
        {
            std::wstring path = ModuleDirectories(symbols);
            std::wstring self = ToWide(Arcane::ExecutablePathUtf8());
            const std::size_t slash = self.find_last_of(L"/\\");
            if (slash != std::wstring::npos) { self.resize(slash); path += self; path += L";"; }
            wchar_t env[4096];
            if (GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", env, 4096) > 0) path += env;
            return path;
        }

        // One frame: dbgeng's "module!function" name + displacement, line info
        // when the PDB has it, else the IMAGE file name + offset from its base.
        SymFrame Resolve(IDebugSymbols3* symbols, ULONG64 address)
        {
            SymFrame f;
            f.address = address;

            wchar_t name[1024];
            ULONG   size = 0;
            ULONG64 disp = 0;
            if (SUCCEEDED(symbols->GetNameByOffsetWide(address, name, 1024, &size, &disp)) && size > 1)
            {
                const std::string full = ToUtf8(name);
                const std::size_t bang = full.find('!');
                // No '!' means dbgeng had no SYMBOL, only a module -- it
                // spells that "module+0x..." itself. Fall through to the image
                // path below so the symbol-less form always names the IMAGE
                // FILE (death-fixture.exe), one spelling, never two.
                if (bang != std::string::npos)
                {
                    f.module       = full.substr(0, bang);
                    f.function     = full.substr(bang + 1);
                    f.displacement = disp;

                    wchar_t file[1024];
                    ULONG   line     = 0;
                    ULONG   fileSize = 0;
                    ULONG64 lineDisp = 0;
                    if (SUCCEEDED(symbols->GetLineByOffsetWide(address, &line, file, 1024, &fileSize, &lineDisp)) && fileSize > 1)
                    {
                        f.file = ToUtf8(file);
                        f.line = line;
                    }
                    return f;
                }
            }

            ULONG   index = 0;
            ULONG64 base  = 0;
            if (SUCCEEDED(symbols->GetModuleByOffset(address, 0, &index, &base)))
            {
                wchar_t image[1024];
                ULONG   imageSize = 0;
                if (SUCCEEDED(symbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, index, base, image, 1024, &imageSize)) && imageSize > 1)
                {
                    const std::string full  = ToUtf8(image);
                    const std::size_t slash = full.find_last_of("/\\");
                    f.module = slash == std::string::npos ? full : full.substr(slash + 1);
                }
                else
                {
                    f.module = "<module>";
                }
                f.displacement = address - base;
                return f;
            }

            f.module       = "<unknown>";
            f.displacement = address;
            return f;
        }
    }

    Symbolized SymbolizeDump(const std::filesystem::path& dmp, const SymbolizeOptions& opt)
    {
        Symbolized out;

        // dbgeng.h declares every interface DECLSPEC_UUID, so __uuidof is the
        // idiom (UE: WindowsPlatformStackWalkExt.cpp:51-55) -- no hand-rolled
        // GUID and no IID_ linkage to get wrong.
        ComPtr<IDebugClient5> client;
        HRESULT hr = DebugCreate(__uuidof(IDebugClient5), reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { out.engineError = "DebugCreate failed: " + Hex(hr); return out; }

        ComPtr<IDebugControl4>       control;
        ComPtr<IDebugSymbols3>       symbols;
        ComPtr<IDebugSystemObjects4> sys;
        if (FAILED(client.As(&control)) || FAILED(client.As(&symbols)) || FAILED(client.As(&sys)))
        { out.engineError = "dbgeng interfaces unavailable"; return out; }

        // UE's option set (WindowsPlatformStackWalkExt.cpp:74-95), set BEFORE
        // the dump opens: line info, nearest OMAP, fail on critical errors,
        // deferred loads, EXACT symbols (a GUID/age mismatch is "no symbols",
        // never a WRONG name -- a plausible wrong function is worse than an
        // offset), undecorated names. IGNORE_CVREC is the D5 seam on top: do
        // not follow the PDB path the linker embedded in the image, which is
        // the only way to hide a PDB on the desk that built it. NO_IMAGE_SEARCH
        // rides with it for the same reason -- without it dbghelp walks back
        // to the module's ORIGINAL directory, recorded in the dump, and finds
        // the very PDB the caller asked it not to consult.
        ULONG so = SYMOPT_LOAD_LINES | SYMOPT_OMAP_FIND_NEAREST | SYMOPT_FAIL_CRITICAL_ERRORS
                 | SYMOPT_DEFERRED_LOADS | SYMOPT_EXACT_SYMBOLS | SYMOPT_UNDNAME;
        if (opt.ignoreCvRecord) so |= SYMOPT_IGNORE_CVREC | SYMOPT_NO_IMAGE_SEARCH;
        symbols->SetSymbolOptions(so);

        hr = client->OpenDumpFileWide(dmp.c_str(), 0);
        if (FAILED(hr)) { out.engineError = "OpenDumpFile failed: " + Hex(hr); return out; }

        SessionEnder ender;
        ender.client = client.Get();

        hr = control->WaitForEvent(0, opt.waitForEventMs);   // UE: WaitForEvent(0, INFINITE), :568 -- ours is bounded
        if (FAILED(hr)) { out.engineError = "WaitForEvent failed: " + Hex(hr); return out; }
        out.engineAvailable = true;

        // The symbol path needs the module list, so it is set AFTER the dump
        // opens (UE: CrashDebugHelperWindows.cpp:30-33 -- InitSymbols, open,
        // SetSymbolPathsFromModules). --symbol-path REPLACES the whole search.
        const std::wstring symPath = opt.symbolPath.empty() ? DefaultSymbolPath(symbols.Get()) : ToWide(opt.symbolPath);
        symbols->SetSymbolPathWide(symPath.c_str());
        symbols->SetImagePathWide(symPath.c_str());
        out.symbolPath = ToUtf8(symPath);

        // The faulting thread, UE's way (:463-490): the STORED event's CONTEXT
        // is the exception context the host put in the dump, and
        // GetContextStackTrace walks from THAT -- not from whatever thread the
        // engine happens to land on, which for a minidump is thread 0. Every
        // Arcane dump carries a stored event: Diagnostics.cpp synthesizes an
        // exception record for hang and manual reports (this task), exactly as
        // UE does for a suspended thread (:1925-1966).
        std::vector<std::uint8_t> ctx(4096);
        ULONG eventType = 0, eventPid = 0, eventTid = 0, ctxUsed = 0;
        const bool haveEvent = SUCCEEDED(control->GetStoredEventInformation(&eventType, &eventPid, &eventTid,
                                                                            ctx.data(), static_cast<ULONG>(ctx.size()), &ctxUsed,
                                                                            nullptr, 0, nullptr))
                            && ctxUsed > 0;
        if (haveEvent)
        {
            SymThread t;
            ULONG     sysId = 0;
            sys->SetCurrentThreadId(eventTid);
            sys->GetCurrentThreadSystemId(&sysId);
            t.systemId = sysId;
            t.faulting = true;

            std::vector<DEBUG_STACK_FRAME> frames(opt.maxFramesPerThread);
            ULONG                          filled = 0;
            if (SUCCEEDED(control->GetContextStackTrace(ctx.data(), ctxUsed, frames.data(), static_cast<ULONG>(frames.size()),
                                                        nullptr, 0, 0, &filled)))
                for (ULONG k = 0; k < filled; ++k)
                    t.frames.push_back(Resolve(symbols.Get(), frames[k].InstructionOffset));
            out.threads.push_back(std::move(t));
        }

        // Every OTHER thread from its own saved context (what windbg's ~*k
        // does). This is beyond what UE's client walks -- its other threads
        // come from the in-process portable capture -- so it is best-effort: a
        // thread that fails to walk is listed with no frames rather than
        // failing the report.
        ULONG count = 0;
        sys->GetNumberThreads(&count);
        for (ULONG i = 0; i < count && i < opt.maxThreads; ++i)
        {
            ULONG engineId = 0, systemId = 0;
            if (FAILED(sys->GetThreadIdsByIndex(i, 1, &engineId, &systemId))) continue;
            if (haveEvent && engineId == eventTid) continue;
            if (FAILED(sys->SetCurrentThreadId(engineId))) continue;

            SymThread t;
            t.systemId = systemId;

            std::vector<DEBUG_STACK_FRAME> frames(opt.maxFramesPerThread);
            ULONG                          filled = 0;
            if (SUCCEEDED(control->GetStackTrace(0, 0, 0, frames.data(), static_cast<ULONG>(frames.size()), &filled)))
                for (ULONG k = 0; k < filled; ++k)
                    t.frames.push_back(Resolve(symbols.Get(), frames[k].InstructionOffset));
            out.threads.push_back(std::move(t));
        }

        return out;   // ~SessionEnder detaches; the ComPtrs release in reverse order
    }
}
