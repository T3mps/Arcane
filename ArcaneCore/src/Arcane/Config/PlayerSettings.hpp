#pragma once

// Players' settings (settings spec s8.1, s8.3). A game builds its graphics /
// audio / controls menu from List(): PlayerSafe settings only, with display
// name, help, type, range, enum names and apply mode. Adding a PlayerSafe
// field to a settings struct adds it to the menu with no UI code.
//
// Set() runs in the SESSION's context, never Editor (spec s3.2):
//   single-player and a listen-server host -> LocalHost;
//   connected to someone else's server     -> Client.
// The session mode is the PRIMARY world's NetMode, published by PluginHost
// every time it fills EngineContext::netMode (load, reload, attach, Refresh).
// A dedicated server has no player: Set() there is Denied.
//
// Set() writes the User rung (SetBy::User, source "user", the record the
// User config file loads) and does not publish. The host's frame publishes,
// and the archive writes the value to the per-user directory
// (Paths GameUserDir/Config) at exit; a game that wants it on disk now calls
// EngineContext::engine->SaveUserCVars(). Main thread only, like every
// registry write.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Core/Api.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>   // NetMode

#include <string_view>
#include <vector>

namespace Arcane
{
    [[nodiscard]] ARC_CORE_API CVarContext CVarContextFor(NetMode mode) noexcept;

    namespace PlayerSettings
    {
        ARC_CORE_API void SetSessionMode(NetMode mode) noexcept;
        [[nodiscard]] ARC_CORE_API NetMode SessionMode() noexcept;
        [[nodiscard]] ARC_CORE_API CVarContext SessionContext() noexcept;

        // The process registry, in the session's context (the contract's pair).
        [[nodiscard]] ARC_CORE_API std::vector<CVarListEntryEx> List(std::string_view categoryPrefix);
        ARC_CORE_API SetResult Set(std::string_view name, const CVarValue& value);

        // The same on an explicit registry and context (tests, tools).
        // categoryPrefix: "" = all; "audio" matches "audio.x" but not "audiox.y".
        [[nodiscard]] ARC_CORE_API std::vector<CVarListEntryEx> List(const CVarRegistry& registry,
                                                                        std::string_view categoryPrefix);
        ARC_CORE_API SetResult Set(CVarRegistry& registry, std::string_view name, const CVarValue& value,
                                      CVarContext context);
    }
}
