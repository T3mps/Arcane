#pragma once

// Render device creation (settings arc S6-16; inventory Part 2 "Host command
// line", "Renderer device creation", "Swapchain", "Texture and mesh caches").
// Every field is read once, when the device, its swapchain or the
// pending-cook placeholder is created (Restart), except render.meshCull
// (Live, read per frame by MeshCullNode).
//
// - render.*         RenderSettings: the player-facing graphics choices
//                    (backend, vsync, adapter, tearing; PlayerSafe, so a
//                    player override writes the per-machine User rung), the
//                    frames-in-flight depth (S6-17; FramePacing.hpp latches
//                    it once per process) and
//                    the GPU frustum-cull debug switch, folded in from the S2
//                    one-off struct with its name, type, default, flags and
//                    help unchanged. --backend and --no-vsync stay as flags:
//                    HostBoot::ApplyEarlyConfigRungs puts them on the
//                    CommandLine rung, and the HostConfig adopts the
//                    published values before GpuContext::Create.
// - render.debug.*   RenderDebugSettings: the validation layers and the
//                    info-queue/messenger policy. Their defaults are PER
//                    CONFIGURATION (on in Debug, off otherwise) -- the values
//                    the frame-graph and offscreen vehicles used to force
//                    under ARC_BUILD_DEBUG.
// - render.d3d12.*   RenderD3d12Settings: NRI's zero buffer, enhanced
//                    barriers and the device reference armor.
// - render.vulkan.*  RenderVulkanSettings: the injected-module fallback.
//
// MakeRenderDeviceDesc() is the published values as a RenderDeviceDesc, the
// one way an engine host builds its device desc.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>

#include <array>
#include <cstdint>

namespace Arcane
{
    ARC_REFLECT_ENUM(GraphicsBackend)
        ARC_REFLECT_ENUM_VALUE(GraphicsBackend, D3D12)
        ARC_REFLECT_ENUM_VALUE(GraphicsBackend, Vulkan)
    ARC_END_REFLECT_ENUM()

    struct RenderSettings
    {
        GraphicsBackend backend      = GraphicsBackend::D3D12;
        bool            vsync        = true;
        std::int32_t    adapter      = -1;   // -1 = auto (D3D12: high-performance index 0; Vulkan: first discrete, else [0])
        bool            allowTearing = false;
        bool            meshCull     = true;
        std::uint32_t   framesInFlight = 2;   // FramePacing.hpp: latched once, clamped to kMaxFramesInFlight
    };

    ARC_REFLECT_TYPE(RenderSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(RenderSettings, backend)
            ARC_REFLECT_ATTR(PlayerSafe)
            ARC_REFLECT_ATTR(Tooltip, "Graphics API: D3D12 or Vulkan. The --backend flag overrides it for one launch.")
        ARC_REFLECT_FIELD(RenderSettings, vsync)
            ARC_REFLECT_ATTR(PlayerSafe)
            ARC_REFLECT_ATTR(Tooltip, "Cap presentation to the display's refresh rate. The --no-vsync flag turns it off for one launch.")
        ARC_REFLECT_FIELD(RenderSettings, adapter)
            ARC_REFLECT_ATTR(PlayerSafe) ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject)
            ARC_REFLECT_ATTR(Range, -1.0, 63.0)
            ARC_REFLECT_ATTR(Tooltip, "GPU to render on, by adapter index. -1 picks automatically (the high-performance GPU on "
                                      "D3D12, the first discrete GPU on Vulkan); an index the system does not have falls back to "
                                      "automatic with a warning.")
        ARC_REFLECT_FIELD(RenderSettings, allowTearing)
            ARC_REFLECT_ATTR(PlayerSafe)
            ARC_REFLECT_ATTR(Tooltip, "Allow screen tearing when vsync is off, for variable-refresh displays. No effect with vsync on.")
        ARC_REFLECT_FIELD(RenderSettings, meshCull)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Frustum-cull mesh instances on the GPU.")
        ARC_REFLECT_FIELD(RenderSettings, framesInFlight)
            ARC_REFLECT_ATTR(Range, 2.0, 3.0)
            ARC_REFLECT_ATTR(Tooltip, "How many frames the CPU may record ahead of the GPU. 3 smooths uneven frame times at the "
                                      "cost of one more frame of input latency; 2 is the lower-latency default.")
    ARC_END_REFLECT_TYPE()

    // The per-configuration validation default: on in Debug, off in Release
    // and Dist.
#if defined(ARC_BUILD_DEBUG)
    inline constexpr bool kRenderDebugDefault = true;
#else
    inline constexpr bool kRenderDebugDefault = false;
#endif

    // Which validation-message severities stop the debugger: the named one
    // and everything more severe (D3D12 and DXGI info queues). None never
    // breaks -- every message goes through the log callback and the render
    // error latch instead.
    enum class BreakSeverity : std::uint8_t { None = 0, Corruption = 1, Error = 2, Warning = 3 };

    ARC_REFLECT_ENUM(BreakSeverity)
        ARC_REFLECT_ENUM_VALUE(BreakSeverity, None)
        ARC_REFLECT_ENUM_VALUE(BreakSeverity, Corruption)
        ARC_REFLECT_ENUM_VALUE(BreakSeverity, Error)
        ARC_REFLECT_ENUM_VALUE(BreakSeverity, Warning)
    ARC_END_REFLECT_ENUM()

    // The least severe validation message that reaches the log and the error
    // latch (the D3D12 info-queue deny list, the Vulkan messenger mask).
    enum class MinSeverity : std::uint8_t { Info = 0, Warning = 1, Error = 2 };

    ARC_REFLECT_ENUM(MinSeverity)
        ARC_REFLECT_ENUM_VALUE(MinSeverity, Info)
        ARC_REFLECT_ENUM_VALUE(MinSeverity, Warning)
        ARC_REFLECT_ENUM_VALUE(MinSeverity, Error)
    ARC_END_REFLECT_ENUM()

    struct RenderDebugSettings
    {
        bool          validation       = kRenderDebugDefault;
        bool          d3d12DebugLayer  = kRenderDebugDefault;
        bool          vkSyncValidation = kRenderDebugDefault;
        BreakSeverity breakOnSeverity  = BreakSeverity::None;
        MinSeverity   minSeverity      = MinSeverity::Warning;
        // The pending-cook placeholder's 8x8 checker: the frozen name
        // (render.debug.pendingCookChecker) is its lead cell, the Alt field
        // the other cell. Written to the texture as the colour's sRGB8 hex
        // bytes, so the defaults are exactly (255,0,255) and (16,16,16).
        CVarColor     pendingCookChecker    = CVarColor{ 1.0f, 0.0f, 1.0f, 1.0f };
        CVarColor     pendingCookCheckerAlt = CVarColor{ 0.0051815167f, 0.0051815167f, 0.0051815167f, 1.0f };   // sRGB8 16 (#101010)
    };

    ARC_REFLECT_TYPE(RenderDebugSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.debug", SettingScope::PreferencesProject, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderDebugSettings, validation)
            ARC_REFLECT_ATTR(Tooltip, "GPU validation: NRI's validation layer, plus the Vulkan validation layer on Vulkan. Every "
                                      "message it reports counts as a render error, which is what makes a Debug run's exit code "
                                      "mean something. Default: on in Debug builds, off otherwise.")
        ARC_REFLECT_FIELD(RenderDebugSettings, d3d12DebugLayer)
            ARC_REFLECT_ATTR(Tooltip, "D3D12 CPU debug layer (EnableDebugLayer), routed into the log and the render error latch. "
                                      "It must be armed before the first device, so a second device in the process declines it. "
                                      "D3D12SDKLayers fail-fasts (0x87D) when some third-party window hooks are injected; turn "
                                      "it off on such a machine. Default: on in Debug builds, off otherwise.")
        ARC_REFLECT_FIELD(RenderDebugSettings, vkSyncValidation)
            ARC_REFLECT_ATTR(Tooltip, "Vulkan synchronization validation: barrier and hazard checks core validation does not make. "
                                      "Expensive. Needs render.debug.validation. Default: on in Debug builds, off otherwise.")
        ARC_REFLECT_FIELD(RenderDebugSettings, breakOnSeverity)
            ARC_REFLECT_ATTR(Tooltip, "Break into the debugger on a D3D12/DXGI validation message of this severity or worse. "
                                      "None logs every message instead.")
        ARC_REFLECT_FIELD(RenderDebugSettings, minSeverity)
            ARC_REFLECT_ATTR(Tooltip, "The least severe validation message that reaches the log and the render error latch "
                                      "(D3D12 info queue and Vulkan messenger).")
        ARC_REFLECT_FIELD(RenderDebugSettings, pendingCookChecker)
            ARC_REFLECT_ATTR(Tooltip, "Lead colour of the checkerboard drawn for a texture still waiting on its cook. Keep it "
                                      "unlike white, which marks a refused texture.")
        ARC_REFLECT_FIELD(RenderDebugSettings, pendingCookCheckerAlt)
            ARC_REFLECT_ATTR(Tooltip, "Second colour of the pending-cook checkerboard.")
    ARC_END_REFLECT_TYPE()

    struct RenderD3d12Settings
    {
        std::uint64_t zeroBufferBytes  = 0;       // 0 = NRI's default (4 MB)
        bool          enhancedBarriers = true;
        std::uint32_t deviceArmorRefs  = 65536;
    };

    ARC_REFLECT_TYPE(RenderD3d12Settings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.d3d12", SettingScope::PreferencesProject, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderD3d12Settings, zeroBufferBytes)
            ARC_REFLECT_ATTR(Scope, SettingScope::Project) ARC_REFLECT_ATTR(Range, 0.0, 67108864.0)
            ARC_REFLECT_ATTR(Tooltip, "Size in bytes of NRI's D3D12 zero buffer. 0 keeps NRI's default (4 MB).")
        ARC_REFLECT_FIELD(RenderD3d12Settings, enhancedBarriers)
            ARC_REFLECT_ATTR(Tooltip, "Use D3D12 enhanced barriers where the Agility runtime offers them. Turn off to fall back to "
                                      "legacy barriers when chasing a driver bug.")
        ARC_REFLECT_FIELD(RenderD3d12Settings, deviceArmorRefs)
            ARC_REFLECT_ATTR(Range, 0.0, 16777216.0)
            ARC_REFLECT_ATTR(Tooltip, "Extra references held on the D3D12 device so an injected overlay that over-releases it "
                                      "cannot destroy it mid-session; a shortfall at shutdown is logged. 65536 covers hours.")
    ARC_END_REFLECT_TYPE()

    struct RenderVulkanSettings
    {
        bool foreignModuleFallback = true;
    };

    ARC_REFLECT_TYPE(RenderVulkanSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.vulkan", SettingScope::PreferencesProject, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(RenderVulkanSettings, foreignModuleFallback)
            ARC_REFLECT_ATTR(Tooltip, "Run a windowed session on D3D12 when a module known to crash a windowed Vulkan swapchain is "
                                      "injected. Turn off to force Vulkan anyway, e.g. to reproduce that crash.")
    ARC_END_REFLECT_TYPE()

    // Does `setting` break on a message of severity `message` (Corruption,
    // Error or Warning)? The named severity and every worse one do.
    [[nodiscard]] constexpr bool BreaksOn(BreakSeverity setting, BreakSeverity message) noexcept
    {
        return setting != BreakSeverity::None && message != BreakSeverity::None && message <= setting;
    }

    // Does `setting` let a message of severity `message` through?
    [[nodiscard]] constexpr bool Reports(MinSeverity setting, MinSeverity message) noexcept
    {
        return message >= setting;
    }

    // A colour as the four bytes of its "#RRGGBBAA" spelling (sRGB-encoded
    // RGB, linear alpha): what an RGBA8 texel holds for it, in either an
    // _UNORM display texture or an _SRGB one.
    [[nodiscard]] ARC_API std::array<std::uint8_t, 4> Srgb8Bytes(const CVarColor& color);

    // The published render.backend and render.debug.* as a device desc.
    [[nodiscard]] ARC_API RenderDeviceDesc MakeRenderDeviceDesc();
}
