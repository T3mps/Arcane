#pragma once
// ArcaneRuntimeHost: the standalone game host's whole body, as a static library
// (engine modularization s3.3). Two executables link it:
//   * ArcaneRuntime.exe (modular layout) -- `Run(argc, argv, nullptr)`: today's
//     host; the game module is a DLL named by --project / --plugin, and the
//     verification vehicle (ArcaneVerify) is linked.
//   * a packaged game, <Game>.exe (monolithic layout, `arcbuild package`) -- its
//     generated main passes a PackageInfo: the game is linked into the exe
//     (PluginHost's static mode), the project defaults to the one staged beside
//     the exe, and no Developer module (ArcaneVerify) is linked.
// Each exe keeps ARC_AGILITY_SDK_EXPORTS() in its OWN main: D3D12 reads the
// exported pair from the process image (Arcane/Render/AgilitySdk.hpp).

namespace Arcane::RuntimeHost
{
    struct PackageInfo
    {
        // The packaged project's manifest, relative to the exe's directory
        // ("<Project>/<Project>.arcproj"). Used when no --project/--plugin is given.
        const char* projectRelPath = nullptr;
        // The engine modules linked into the exe, logged once at boot
        // ("[package] modules: ...") so a packaged run states what it contains.
        const char* const* modules = nullptr;
        int moduleCount = 0;
        // The crash reporter's app name (the exe's stem).
        const char* appName = nullptr;
    };

    // The process body: parse argv, arm diagnostics, run the host, tear down.
    // Returns the process exit code. `package` null = ArcaneRuntime.exe.
    int Run(int argc, char** argv, const PackageInfo* package);
}
