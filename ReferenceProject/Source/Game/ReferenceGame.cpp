#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <string>

namespace ReferenceGame
{
    // The settings arc's example policy (spec s3.2): games decide who may change
    // their own settings. A single-player "mods" switch, flipped from the
    // player's own settings menu, lets the LOCAL HOST change any non-cheat Game
    // or Server setting (console, --set, a mods menu). A connected client is
    // never widened, Cheat settings still need server.cheats, and Editor /
    // Protected / Dev-in-Dist stay out of reach whatever this returns.
    ARC_CVAR(cvar_modsEnabled, "game.mods.enabled", bool, false,
             .flags = Arcane::CVarFlags::Archive,
             .audience = Arcane::Audience::PlayerSafe,
             .scope = Arcane::SettingScope::Project,
             .apply = Arcane::ApplyMode::Live,
             .help = "Single-player mods: the local host may change any non-cheat Game or Server setting.");

    // Captured while the plugin host's module scope is open (static init), so
    // the policy is filed under the same module name its unload clears.
    const std::string kModuleName{ Arcane::CVarRegistry::CurrentModule() };

    Arcane::PolicyVerdict ModsPolicy(const Arcane::CVarInfo& cvar, const Arcane::CVarRequest& request, void*)
    {
        using Arcane::PolicyVerdict;
        if (!request.write || request.context != Arcane::CVarContext::LocalHost) return PolicyVerdict::Default;
        if (!cvar_modsEnabled.Get()) return PolicyVerdict::Default;
        if (Arcane::HasFlag(cvar.flags, Arcane::CVarFlags::Cheat)) return PolicyVerdict::Default;
        if (cvar.name == cvar_modsEnabled.Name()) return PolicyVerdict::Default;
        if (cvar.audience == Arcane::Audience::Game || cvar.audience == Arcane::Audience::Server)
            return PolicyVerdict::Allow;
        return PolicyVerdict::Default;
    }

    // The module's other job: refuse to load without the two actions the player
    // controller needs. Input and time reach the controller as resources
    // (PlayerController2DSystem), never copied in here.
    struct Module final : Arcane::GameModule
    {
        bool OnInit(Arcane::EngineContext&) override
        {
            Arcane::CVarRegistry::Get().SetPolicy(&ModsPolicy, nullptr, kModuleName);
            if (!Client()) return true; // server has no local input device
            if (!Client()->GameInput().FindAction("Player", "Move") ||
                !Client()->GameInput().FindAction("Player", "Jump"))
            {
                ARC_ERROR("ReferenceGame: Player.Move and Player.Jump are required in the selected gameplay input asset");
                return false;
            }
            return true;
        }

        void OnShutdown() override
        {
            Arcane::CVarRegistry::Get().SetPolicy(nullptr, nullptr, kModuleName);
        }
    };
}

ARC_GAME_MODULE(ReferenceGame::Module)
