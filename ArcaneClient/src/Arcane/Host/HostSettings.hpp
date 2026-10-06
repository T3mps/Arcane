#pragma once

// The host window and boot settings (settings arc S6-24; inventory Part 2
// "Window" / "Boot" and reconciliation R1).
//
// - render.window.*: the main window's extent and resizability, read when
//   GpuContext creates the window (Restart). Every golden is captured at the
//   1280x720 default, so the default must not change. --window-size WxH stays
//   the automation-only override (it wins over the setting at creation).
// - app.window.minimizedSleepMs: the sleep while a host window is minimized or
//   its frame was skipped, read every frame (Live). The editor's separate
//   unfocused throttle is editor.perf.backgroundFps (EditorPerfSettings).
// - boot.*: the boot status line's scan throttle (shared by the boot stage and
//   the editor's project switch) and the splash repaint cadence during a
//   worker overlap.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Config/Settings.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    struct RenderWindowSettings
    {
        std::uint32_t width     = 1280;
        std::uint32_t height    = 720;
        bool          resizable = true;
    };

    // The width/height range is HostConfig's kMinWindowSide..kMaxWindowSide
    // (HostSettings.cpp static_asserts that the two agree).
    ARC_REFLECT_TYPE(RenderWindowSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "render.window", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(RenderWindowSettings, width)
            ARC_REFLECT_ATTR(PlayerSafe) ARC_REFLECT_ATTR(Range, 64.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Window width in pixels when the game starts. --window-size overrides it.")
        ARC_REFLECT_FIELD(RenderWindowSettings, height)
            ARC_REFLECT_ATTR(PlayerSafe) ARC_REFLECT_ATTR(Range, 64.0, 8192.0)
            ARC_REFLECT_ATTR(Tooltip, "Window height in pixels when the game starts. --window-size overrides it.")
        ARC_REFLECT_FIELD(RenderWindowSettings, resizable)
            ARC_REFLECT_ATTR(Tooltip, "Let the player resize the game window by dragging its edges.")
    ARC_END_REFLECT_TYPE()

    struct AppWindowSettings
    {
        std::uint32_t minimizedSleepMs = 1;
    };

    ARC_REFLECT_TYPE(AppWindowSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "app.window", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(AppWindowSettings, minimizedSleepMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Milliseconds a host sleeps per frame while its window is minimized or a frame was "
                                      "skipped, so it does not spin a core. 0 never sleeps.")
    ARC_END_REFLECT_TYPE()

    struct BootSettings
    {
        std::uint32_t scanProgressStride = 32;
        std::uint32_t splashPumpMs       = 8;
    };

    ARC_REFLECT_TYPE(BootSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "boot", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(BootSettings, scanProgressStride)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 4096.0)
            ARC_REFLECT_ATTR(Tooltip, "The boot and project-switch status line shows content-scan progress every this "
                                      "many files (plus the first and the last).")
        ARC_REFLECT_FIELD(BootSettings, splashPumpMs)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Range, 1.0, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Milliseconds between loading-screen repaints while a boot stage runs on a worker.")
    ARC_END_REFLECT_TYPE()
}

namespace Arcane::HostBoot
{
    // The scan status throttle (boot.scanProgressStride), shared by the boot
    // stage's ReportScanProgress and the editor's SwitchProject project_open
    // body: report the first file, the last, and every stride-th between.
    ARC_API bool ShouldReportScanProgress(std::size_t done, std::size_t total);
}
