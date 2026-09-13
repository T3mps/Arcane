#pragma once

// ModuleBuild: Build -> Rebuild Game Module. Rebuilds the OPEN project's game
// module against the RUNNING editor's SDK by spawning arcbuild.exe -- the
// engine's game-project build driver (spec docs/specs/2026-09-13-arcbuild-
// driver-design.md) -- and streaming its merged stdout+stderr line-by-line,
// on a worker std::thread via _wpopen, into a thread-safe queue the
// EditorApp drains once per frame into the Console ("Build: " lines).
//
// What the driver decides is the driver's: premake first every build, the
// single-slot incremental rule (s4.3 -- /t:Rebuild only when Binaries/
// holds the other configuration's DLL), where premake and msbuild are. This
// file only knows (a) where arcbuild.exe is relative to the editor exe,
// (b) the ONE command line to run it with, (c) how to stream it. The
// COMPOSITION half is pure and unit-tested ([editor], ModuleBuildTest.cpp);
// the Runner and RunCapture spawn processes and are desk-verify territory
// -- the same split RuntimeLaunch.cpp draws around SpawnDetached.
//
// v1 NON-GOALS (arc decision): no Live-Coding patching, no in-editor code
// editing, no MSVC-diagnostic parsing into per-line locators -- raw console
// lines plus ONE failure row in Problems. No auto-build after the class
// wizard's Create (a future opt-in Tools -> Settings item).

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Arcane::Editor::ModuleBuild
{
    // ---- pure halves ([editor]-tested; nothing here touches a process) ------

    // The engine workspace root ("SDK root", what arcane.lua calls ARCANE_SDK)
    // from the editor exe's directory: three levels up, inverting the premake
    // targetdir rule <sdk>/bin/<cfg>-<system>-<arch>-md/ArcaneEditor/. Pure
    // path math -- no filesystem probe -- so it composes into tests. Passed
    // to the driver as --sdk: "rebuild against the engine you are looking at".
    std::filesystem::path SdkRootFromExeDir(const std::filesystem::path& exeDir);

    // The msbuild configuration this editor build drives. The game DLL must
    // share the editor's CRT + engine import lib flavor, so it follows the
    // editor's own build. Dist caveat: a Dist editor also answers "Release" --
    // Dist is a packaging config of the ENGINE workspace; external projects
    // (build/arcane.lua consumers) map it onto their own Release/Dist pair at
    // generate time, and Release is the one that always exists.
    constexpr const char* Configuration()
    {
#ifdef _DEBUG
        return "Debug";
#else
        return "Release";
#endif
    }

    // Where arcbuild.exe might live relative to the EDITOR exe's directory:
    // packaged layout first (installed side by side), dev bin layout second
    // (premake's bin/<cfg>-<os>-<arch>-md/<Project>/ gives ArcaneEditor's
    // dir an "../arcbuild/" neighbour) -- the RuntimeLaunch::ExeCandidates
    // rule. Existence is NOT checked here; ResolveDriver does that.
    std::vector<std::filesystem::path> DriverCandidates(const std::filesystem::path& editorExeDir);

    // The first candidate that is a regular file, else empty (the caller
    // refuses with a Console error naming both places it looked).
    std::filesystem::path ResolveDriver(const std::filesystem::path& editorExeDir);

    struct DriverInputs
    {
        std::filesystem::path driverExe;
        std::filesystem::path projectRoot;
        std::filesystem::path sdkRoot;
        std::string           command;         // "build" (Rebuild Game Module) / "generate" (RegenerateSolution)
        std::string           configuration;   // Configuration()
    };

    // THE ONE command line the Runner / RunCapture execute:
    //   ( "<arcbuild>" <command> --project "<root>" --config <cfg> --sdk "<sdk>" ) 2>&1
    // Parenthesised so the trailing 2>&1 folds the driver's stderr into the
    // captured stdout (and so a quoted exe at the head survives cmd's
    // outer-quote stripping). Plain quotes around paths -- good for spaces,
    // which real install paths contain; an embedded quote is not defended.
    std::string ComposeDriverCommand(const DriverInputs& in);

    // ---- process halves (desk-verify; not unit-tested) ----------------------

    // Run `commandLine` through cmd (_wpopen) SYNCHRONOUSLY, returning its
    // merged output line-by-line plus the exit status (nullopt when the pipe
    // itself could not be opened). For the short, one-shot steps a click can
    // afford to wait on -- `arcbuild generate` takes well under a second --
    // where the Runner's worker thread would only add a frame of state
    // machine for nothing. NOT for `build`: that is the Runner's job.
    struct CaptureResult
    {
        std::vector<std::string> lines;
        std::optional<int>       exit;
    };
    CaptureResult RunCapture(const std::string& commandLine);

    // THIS process's exe directory (GetModuleFileNameW). Same private pattern
    // as EditorFonts.cpp/EditorAppScene.cpp, hoisted here because the SDK-root
    // walk and the driver lookup both start from it.
    std::filesystem::path ExeDir();

    // ---- the worker ---------------------------------------------------------

    // One build at a time, output pulled main-thread-side per frame. The
    // worker owns the _wpopen pipe for its whole life; the main thread only
    // ever touches the mutex-guarded queue + flags, so there is no handle to
    // race over. Join() blocks until the child exits -- Shutdown calls it, and
    // an editor closed mid-build waits for the driver rather than leaking a
    // worker thread into destructed members.
    class Runner
    {
    public:
        ~Runner() { Join(); }

        // Start `commandLine` on a fresh worker. False (and no effect) while a
        // build is already running.
        bool Start(std::string commandLine);

        [[nodiscard]] bool Running() const;

        // Take every line queued since the last drain (worker -> main thread).
        std::vector<std::string> DrainLines();

        // The finished build's exit code, exactly once: nullopt while running,
        // never started, or already taken. cmd's exit status = arcbuild's own
        // (the first failing child's, 2 for a refusal; -1 when the pipe
        // itself failed).
        std::optional<int> TakeExit();

        // Block until the worker exits (see the class comment). Idempotent.
        void Join();

    private:
        std::thread              m_thread;
        mutable std::mutex       m_mutex;
        std::vector<std::string> m_lines;     // guarded by m_mutex
        bool                     m_running = false;   // guarded by m_mutex
        bool                     m_done    = false;   // guarded by m_mutex (latch for TakeExit)
        int                      m_exit    = -1;      // guarded by m_mutex
    };
}
