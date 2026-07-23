#include <LoomConfig.hpp>
#include <Arcane/Cli/Cli.hpp>
LoomConfig::ParseOutcome LoomConfig::Parse(int argc, char** argv)
{
    Arcane::Cli cli{ "Arcane Loom", "M5 plugin host" };
    cli.Option("backend", "dx12",        "graphics backend: dx12|vulkan").Choices({ "dx12", "vulkan" });
    cli.Option("frames",  "0",           "render N frames then exit").Type(Arcane::CliType::Uint);
    cli.Flag  ("no-vsync",               "present without vsync");
    cli.Flag  ("perf",                   "log per-phase ms every 60 frames");
    cli.Option("plugin",  "",            "game DLL to host (empty = host default: Loom Sandbox.dll, editor none)");
    cli.Option("project", "", "project folder or .arcproj to open (empty = data/-next-to-exe)");

    const Arcane::Cli::Result r = cli.Parse(argc, argv);
    if (!r.ok) return { std::nullopt, r.exitCode };

    LoomConfig cfg;
    cfg.backend    = (r.Get("backend") == "vulkan") ? Arcane::GraphicsBackend::Vulkan : Arcane::GraphicsBackend::D3D12;
    cfg.maxFrames  = r.GetAs<std::uint64_t>("frames");
    cfg.vsync      = !r.Flag("no-vsync");
    cfg.perf       = r.Flag("perf");
    cfg.pluginPath = r.Get("plugin");
    cfg.projectPath = r.Get("project");
    return { cfg, 0 };
}
