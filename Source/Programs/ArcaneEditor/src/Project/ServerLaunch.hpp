#pragma once

// ServerLaunch: editor-side helper that resolves the DEDICATED-SERVER host exe
// (ArcaneServer.exe, plan 1 Task 6), builds its argv, and spawns it -- the
// mechanism behind the play-mode picker's "Client + separate server process" row
// (Core-DLL split, plan 1 Task 7). The editor's own world plays as a CLIENT
// (PlayTopology::ClientOnly) while the authority runs in that child process.
//
// THE ONE DIFFERENCE FROM RuntimeLaunch (Project/RuntimeLaunch.hpp), which this
// file otherwise mirrors clause for clause: that spawn is FIRE-AND-FORGET -- the
// editor takes no handle and there is nothing for Stop to do. This one is
// TRACKED. The server is the other half of a Play session, so Stop must end it;
// ServerProcess below keeps the process handle for exactly that, and its
// destructor is the backstop for an editor that exits mid-session.
//
// ExeCandidates and BuildArgs are pure (no OS calls, no filesystem probing) --
// ServerLaunchTest.cpp drives them directly, headlessly, same precedent as
// RuntimeLaunch's own pure halves. ServerProcess::Spawn is desk-verify
// territory: no test here actually creates a process. What IS covered is the
// DORMANT contract (a never-spawned handle reports not-running and its Stop is a
// silent no-op), because the editor calls Stop() unconditionally after every
// Play->Stop, on every topology.

#include <filesystem>
#include <string>
#include <vector>

namespace Arcane::Editor::ServerLaunch
{
    // Where ArcaneServer.exe might live relative to the EDITOR exe's own
    // directory: packaged layout first (installed side by side), dev bin layout
    // second (premake's bin/<cfg>-<os>-<arch>-md/<Project>/ puts every project
    // in its own sibling directory, so ArcaneEditor's dir has an
    // "../ArcaneServer/" neighbour). Identical shape and identical reasoning to
    // RuntimeLaunch::ExeCandidates -- read that one's comment for the Hub
    // suggest_engine precedent behind the two-candidate probe. Existence is NOT
    // checked here; that is the caller's job (Spawn refuses a missing file too,
    // but resolving caller-side is what lets a failure name BOTH candidates).
    [[nodiscard]] std::vector<std::filesystem::path> ExeCandidates(
        const std::filesystem::path& editorExeDir);

    // Pure argv builder for ArcaneServer's ServerConfig::Parse: "--project <root>"
    // and "--frames 0". Zero frames is that host's "run until terminated" (Task 6)
    // and is the whole point here -- the EDITOR owns this child's lifetime through
    // ServerProcess::Stop, so any frame budget would end the server behind the
    // editor's back part-way through a session.
    //
    // Each element is ONE argv token (Arcane::Cli reads argv[i] then argv[++i]);
    // no quoting decisions happen here -- Spawn alone turns these logical tokens
    // into one Win32 command line, via RuntimeLaunch::QuoteArg.
    [[nodiscard]] std::vector<std::wstring> BuildArgs(const std::filesystem::path& projectRoot);

    // One tracked ArcaneServer.exe child. Non-copyable (it owns an OS handle);
    // the destructor calls Stop(), so an editor that exits -- cleanly or through
    // a boot failure -- never leaves an orphaned dedicated server holding the
    // project's files open.
    class ServerProcess
    {
    public:
        ServerProcess() = default;
        ~ServerProcess();

        ServerProcess(const ServerProcess&)            = delete;
        ServerProcess& operator=(const ServerProcess&) = delete;

        // CreateProcessW `exe` with `args`, KEEPING the process handle (only the
        // thread handle is closed). Working directory = exe's parent, the same
        // shader/data resolution rule every Arcane host relies on. The child runs
        // with CREATE_NO_WINDOW and its stdout/stderr redirected to
        // "ArcaneServer.log" beside the exe, truncated per launch -- both for the
        // reasons RuntimeLaunch::SpawnDetached states in full: a console-subsystem
        // child would otherwise flash a black window beside the editor, and a child
        // that dies during Init would take the reason with it when that console
        // closed. Read that file to find out why a launch failed.
        //
        // Stops an already-running child first, so a second Spawn never leaks the
        // first. False + ARC_ERROR naming `exe` on failure (missing file, or
        // CreateProcessW itself failing).
        [[nodiscard]] bool Spawn(const std::filesystem::path& exe,
                                 const std::vector<std::wstring>& args);

        // True only while a spawned child is still alive. A never-spawned or
        // already-exited process answers false -- the server exiting on its own
        // (a crash, a refused project) is not an error this class reports, it is
        // simply a handle that no longer names a running process.
        [[nodiscard]] bool IsRunning() const noexcept;

        // Terminate the child and release the handle. NO-OP AND SILENT when
        // nothing was ever spawned (or it was already stopped) -- that is what
        // lets the editor call this after every Play->Stop without knowing which
        // topology was running.
        void Stop() noexcept;

    private:
        void* m_handle = nullptr;   // HANDLE, kept as void* so this header stays windows.h-free
    };
}
