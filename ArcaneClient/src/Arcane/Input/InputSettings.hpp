#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Config/Settings.hpp>

#include <cstdint>
#include <string>

namespace Arcane
{
    // The action evaluator's thresholds and timings (inventory Part 2 "Input").
    // The thresholds and timings are Deterministic (R2): they decide which
    // actions fire from raw device input, so recorded raw input replays
    // differently when they change.
    struct InputSettings
    {
        float pressThreshold = 0.5f;
        float holdSeconds = 0.4f;
        float tapSeconds = 0.2f;
        std::uint32_t maxQueuedTransitions = 256;
        std::string baseContext = "demo";
    };

    ARC_REFLECT_TYPE(InputSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "input", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(InputSettings, pressThreshold)
            ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Range, 0.05, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Magnitude at which an analog control counts as pressed; every part of a chord must reach it.")
        ARC_REFLECT_FIELD(InputSettings, holdSeconds)
            ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Range, 0.05, 5.0)
            ARC_REFLECT_ATTR(Tooltip, "Seconds a control must stay down before an undecorated \"hold\" interaction performs.")
        ARC_REFLECT_FIELD(InputSettings, tapSeconds)
            ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Range, 0.05, 2.0)
            ARC_REFLECT_ATTR(Tooltip, "Longest press, in seconds, that an undecorated \"tap\" interaction still counts as a tap.")
        ARC_REFLECT_FIELD(InputSettings, maxQueuedTransitions)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart) ARC_REFLECT_ATTR(Range, 16.0, 4096.0)
            ARC_REFLECT_ATTR(Tooltip, "Action transitions queued for the next fixed step before the oldest is discarded.")
        ARC_REFLECT_FIELD(InputSettings, baseContext)
            ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld)
            ARC_REFLECT_ATTR(Tooltip, "Input action map the host makes the base context after loading the input config.")
    ARC_END_REFLECT_TYPE()

    // The default bounds of a "deadzone" processor; a binding's own
    // "deadzone(min=..,max=..)" still overrides them.
    struct InputDeadzoneSettings
    {
        float defaultMin = 0.125f;
        float defaultMax = 0.925f;
    };

    ARC_REFLECT_TYPE(InputDeadzoneSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "input.deadzone", SettingScope::Project, ApplyMode::Live, Audience::PlayerSafe)
        ARC_REFLECT_FIELD(InputDeadzoneSettings, defaultMin)
            ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Analog magnitude below which a deadzone processor reads zero.")
        ARC_REFLECT_FIELD(InputDeadzoneSettings, defaultMax)
            ARC_REFLECT_ATTR(Deterministic) ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Analog magnitude above which a deadzone processor reads full scale.")
    ARC_END_REFLECT_TYPE()

    struct InputRebindSettings
    {
        float axisThreshold = 0.5f;
    };

    ARC_REFLECT_TYPE(InputRebindSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "input.rebind", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(InputRebindSettings, axisThreshold)
            ARC_REFLECT_ATTR(Range, 0.1, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Gamepad axis magnitude that a rebind capture must cross to bind the axis.")
    ARC_END_REFLECT_TYPE()
}
