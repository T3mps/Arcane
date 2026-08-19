#pragma once

// ModuleBuild: Build -> Rebuild Game Module. Rebuilds the OPEN project's game
// module against the RUNNING editor's SDK: one composed cmd line -- premake
// FIRST, every build (idempotent; kills the stale-.sln class of failure), then
// msbuild -- run on a worker std::thread via _wpopen, its merged stdout+stderr
// streamed line-by-line into a thread-safe queue the EditorApp drains once per
// frame into the Console ("Build: " lines).
//
// The COMPOSITION half is pure and unit-tested ([editor], ModuleBuildTest.cpp):
// solution discovery, the SDK-root walk, and the command line itself. The
// RESOLUTION half (vswhere, premake probe) and the Runner spawn processes and
// are desk-verify territory -- the same split RuntimeLaunch.cpp draws around
// SpawnDetached.
//
// v1 NON-GOALS (arc decision): no Live-Coding patching, no in-editor code
// editing, no MSVC-diagnostic parsing into per-line locators -- raw console
// lines plus ONE failure row in Problems.

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Arcane::Editor::ModuleBuild
{
    // ---- pure halves ([editor]-tested; nothing here touches a process) ------

    // The generated solution the rebuild drives: the first *.slnx in
    // `projectRoot` (lexicographic, for determinism), else the first *.sln,
    // else empty. Non-recursive on purpose -- the committed convention puts
    // the premake workspace file in the project root (Aphelyon.slnx beside
    // Aphelyon.arcproj), and a recursive scan would find ThirdParty/vendor
    // solutions that are not ours to build.
    std::filesystem::path DiscoverSolution(const std::filesystem::path& projectRoot);

    // The engine workspace root ("SDK root", what arcane.lua calls ARCANE_SDK)
    // from the editor exe's directory: three levels up, inverting the premake
    // targetdir rule <sdk>/bin/<cfg>-<system>-<arch>-md/ArcaneEditor/. Pure
    // path math -- no filesystem probe -- so it composes into tests.
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

    struct ComposeInputs
    {
        std::filesystem::path projectRoot;
        std::filesystem::path premakeExe;
        std::filesystem::path msbuildExe;
        std::filesystem::path solution;        // absolute, or relative to projectRoot
        std::string           configuration;   // "Debug" / "Release"
    };

    // The ONE command line the Runner executes:
    //   ( cd /d "<root>" && "<premake>" vs2026 && "<msbuild>" "<sln>"
    //     /p:Configuration=<cfg> /t:Rebuild /m /nologo ) 2>&1
    // /t:Rebuild is NOT optional: Binaries\ is ONE shared slot across configs,
    // so an incremental build can report success while leaving another config's
    // DLL sitting there (see ModuleBuild.cpp and ModuleBuildTest.cpp).
    // Parenthesized so the trailing 2>&1 folds EVERY member's stderr into the
    // captured stdout (unparenthesized it would bind to msbuild alone, and
    // premake's errors are exactly the ones worth seeing). All paths are
    // wrapped in plain quotes -- good for spaces, which is what real install
    // paths contain; an embedded quote in a path is not defended against.
    std::string ComposeRebuildCommands(const ComposeInputs& in);

    // ---- resolution (probes the machine; not unit-tested) -------------------

    // THIS process's exe directory (GetModuleFileNameW). Same private pattern
    // as EditorFonts.cpp/EditorAppScene.cpp, hoisted here because the SDK-root
    // walk above starts from it.
    std::filesystem::path ExeDir();

    // The engine's bundled premake: <sdkRoot>/ThirdParty/premake5/
    // premake5.exe (the repo layout arcane.lua documents), falling back to
    // bare "premake5" (PATH) when the bundled copy is not there -- a packaged
    // SDK may ship it elsewhere, and cmd's own resolution is the honest
    // fallback.
    std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot);

    // MSBuild via vswhere (the documented VS-install-aware query:
    //   vswhere -latest -requires Microsoft.Component.MSBuild
    //           -find MSBuild\**\Bin\MSBuild.exe
    // under %ProgramFiles(x86)%/Microsoft Visual Studio/Installer), falling
    // back to bare "msbuild" (PATH -- a Developer Command Prompt launch).
    std::filesystem::path ResolveMsBuild();

    // Point ARCANE_SDK at `sdkRoot` in THIS process's environment block, which
    // the worker's cmd child inherits (the project's premake5.lua consumes it
    // via build/arcane.lua). Deliberately overwrites any setx'd value: the
    // whole point is building against the RUNNING editor's SDK, not whichever
    // engine the machine-wide variable last pointed at.
    void SetSdkEnv(const std::filesystem::path& sdkRoot);

    // ---- the worker ---------------------------------------------------------

    // One build at a time, output pulled main-thread-side per frame. The
    // worker owns the _wpopen pipe for its whole life; the main thread only
    // ever touches the mutex-guarded queue + flags, so there is no handle to
    // race over. Join() blocks until the child exits -- Shutdown calls it, and
    // an editor closed mid-build waits for msbuild rather than leaking a
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
        // never started, or already taken. cmd's exit status, i.e. the first
        // failing member of the && chain (or -1 when the pipe itself failed).
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
