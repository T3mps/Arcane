#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Base/Log.hpp>

namespace ReferenceGame
{
    // The module's only job: refuse to load without the two actions the player
    // controller needs. Input and time reach the controller as resources
    // (PlayerController2DSystem), never copied in here.
    struct Module final : Arcane::GameModule
    {
        bool OnInit(Arcane::EngineContext&) override
        {
            if (!Client()) return true; // server has no local input device
            if (!Client()->GameInput().FindAction("Player", "Move") ||
                !Client()->GameInput().FindAction("Player", "Jump"))
            {
                ARC_ERROR("ReferenceGame: Player.Move and Player.Jump are required in the selected gameplay input asset");
                return false;
            }
            return true;
        }
    };
}

ARC_GAME_MODULE(ReferenceGame::Module)
