#pragma once

// How the editor host PRESENTS itself on the desktop (T3-D6 fix round 1):
// whether it puts up the boot splash and whether revealing the main window
// activates and raises it. Pure: unit-tested (HostPresentationTest.cpp).
//
// THE AUTOMATION RULE. An automated run -- `--frames N` (an unattended,
// bounded run) or `--set editor.automation.windowedFrameCapture=true` (the
// windowed self-capture) -- happens while a person may be at the desk working
// in their OWN editor. It must never take their focus: the boot splash is a
// WS_EX_TOPMOST popup that activates on show (BootSplashWindow.cpp), and
// Window::Show raises and activates by default (SDL_RaiseWindow, the
// 2026-07-30 launch-reveal fix for an INTERACTIVE launch). So an automation
// run gets neither: no splash, and the window is shown WITHOUT activation and
// without the raise (Window::Show(false)). It still maps and presents, so the
// windowed capture is the real composited frame.
//
// --headless maps no window at all (its own, older rule): no splash, no show.

#include <Arcane/Host/HostConfig.hpp>

#include <string>
#include <string_view>

namespace Arcane::Editor
{
    // The cvar the windowed self-capture is switched on by (EditorAppFrame.cpp
    // registers it). Read here as TEXT from the --set list because the splash
    // decision is taken in main() before any cvar is applied.
    inline constexpr std::string_view kWindowedFrameCaptureCvarName =
        "editor.automation.windowedFrameCapture";

    // True when the command line makes this an automation run. The LAST --set
    // of the windowed-capture cvar wins, as it does when the sets are applied
    // in order; "1"/"true" are the registry's Bool spellings (CVarRegistry.cpp).
    [[nodiscard]] inline bool IsAutomationRun(const ::Arcane::HostConfig& cfg)
    {
        if (cfg.maxFrames > 0)
            return true;
        bool capture = false;
        for (const std::string& set : cfg.cvarSets)
        {
            const std::size_t eq = set.find('=');
            if (eq == std::string::npos)
                continue;
            std::string_view name(set.data(), eq);
            while (!name.empty() && name.back() == ' ')
                name.remove_suffix(1);
            if (name != kWindowedFrameCaptureCvarName)
                continue;
            std::string_view value(set);
            value.remove_prefix(eq + 1);
            while (!value.empty() && value.front() == ' ')
                value.remove_prefix(1);
            while (!value.empty() && value.back() == ' ')
                value.remove_suffix(1);
            capture = value == "1" || value == "true";
        }
        return capture;
    }

    struct HostPresentation
    {
        bool bootSplash = true;       // construct the pre-device splash (main.cpp)
        bool showWindow = true;       // map the main window at all (EditorApp::CreateGraphVehicles)
        bool activateOnShow = true;   // Window::Show(activate): raise + take the foreground
    };

    [[nodiscard]] inline HostPresentation HostPresentationFor(const ::Arcane::HostConfig& cfg)
    {
        HostPresentation p;
        if (cfg.headless)
        {
            p.bootSplash = false;
            p.showWindow = false;
            p.activateOnShow = false;
            return p;
        }
        if (IsAutomationRun(cfg))
        {
            p.bootSplash = false;
            p.activateOnShow = false;
        }
        return p;
    }
}
