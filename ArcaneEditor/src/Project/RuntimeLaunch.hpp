#pragma once

// RuntimeLaunch: editor-side helper that resolves the standalone runtime host
// exe (ArcaneRuntime.exe), builds its argv, and spawns it detached -- the
// mechanism behind the editor's "play in a separate window" button (Task 6 of
// the runtime-host-fold arc wires ExeCandidates/BuildArgs/SpawnDetached into
// the toolbar's Play action; THIS task only builds and tests the pieces).
//
// ExeCandidates and BuildArgs are pure (no OS calls, no filesystem probing) --
// RuntimeLaunchTest.cpp drives them directly, headlessly, same precedent as
// ComponentCatalog.hpp/EditGesture.hpp. SpawnDetached is desk-verify
// territory: no test here actually creates a process.

#include <filesystem>
#include <string>
#include <vector>

#include <Arcane/Guid.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // Arcane::GraphicsBackend

namespace Arcane::Editor::RuntimeLaunch
{
    // Where ArcaneRuntime.exe might live relative to the EDITOR exe's own
    // directory: packaged layout first (installed side by side), dev bin
    // layout second (premake's bin/<cfg>-<os>-<arch>-md/<Project>/ puts every
    // project in its own sibling directory, so ArcaneEditor's dir has an
    // "../ArcaneRuntime/" neighbor). Mirrors the Hub's suggest_engine, which
    // probes exe_dir/ArcaneEditor.exe then exe_dir/../ArcaneEditor/
    // ArcaneEditor.exe as "a SUGGESTION for the dev loop, never an
    // assumption" (Arcane/Hub/src-tauri/src/lib.rs:569-579, read verbatim
    // before writing this). Existence is NOT checked here -- that is the
    // caller's job; SpawnDetached fails loudly (false + ARC_ERROR) if neither
    // candidate resolves to a real file.
    [[nodiscard]] std::vector<std::filesystem::path> ExeCandidates(
        const std::filesystem::path& editorExeDir);

    // Pure argv builder for ArcaneRuntime's HostConfig::Parse (Arcane/Host/
    // HostConfig.cpp): "--project <root>" always, "--scene <guid>" only when
    // `scene` is valid (IsValid()), "--backend <dx12|vulkan>" always. A
    // nil/invalid scene means "follow the project's manifest bootScene",
    // which is also HostConfig::sceneOverride's own empty-string default --
    // so omitting the flag reproduces that default instead of handing
    // HostConfig a nil-guid string it would just reject as unparsable.
    //
    // Each element is ONE argv token: HostConfig's Cli takes an option's
    // value as a separate token, not an inline "--flag=value" (Cli::Parse
    // reads argv[i] then argv[++i]; see Arcane/ArcaneCore/src/Arcane/Cli/Cli.cpp).
    // No quoting decisions happen here -- SpawnDetached alone turns these
    // logical tokens into one Win32 command line, so callers never have to
    // think about embedded spaces or quotes.
    [[nodiscard]] std::vector<std::wstring> BuildArgs(
        const std::filesystem::path& projectRoot,
        const Arcane::Guid& scene,
        Arcane::GraphicsBackend backend);

    // CreateProcessW `exe` with `args`, then close both handles immediately --
    // fire-and-forget, no tracking, no waiting (the Play button does not need
    // a handle back). Working directory = exe's parent: the same shader-
    // resolution rule ArcaneRuntime/ArcaneEditor already rely on (their
    // ShaderLibrary/ShaderCompiler resolve "data/shaders/" and friends relative to
    // the process's own directory), and the same rule the Hub's own launch of
    // ArcaneEditor.exe follows. Returns false and logs ARC_ERROR naming `exe`
    // on failure (missing file, or CreateProcessW itself failing).
    //
    // The child runs with CREATE_NO_WINDOW and its stdout/stderr redirected to
    // "ArcaneRuntime.log" beside the exe, truncated per launch. Both matter for
    // a fire-and-forget console-subsystem child: without the first it gets its
    // own console window that flashes black next to the game window, and
    // without the second a child that dies during Init takes the reason with it
    // when that console closes. Read that file to find out why a launch failed.
    [[nodiscard]] bool SpawnDetached(
        const std::filesystem::path& exe,
        const std::vector<std::wstring>& args);
}
