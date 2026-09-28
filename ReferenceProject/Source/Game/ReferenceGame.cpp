#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Base/Log.hpp>

#include "PlayerController2DSystem.hpp"

namespace ReferenceGame
{
    struct Module final : Arcane::GameModule
    {
        Arcane::Guid moveId;
        Arcane::Guid jumpId;

        bool OnInit(Arcane::EngineContext&) override
        {
            if (!Client()) return true; // server has no local input device
            const auto move = Client()->GameInput().FindAction("Player", "Move");
            const auto jump = Client()->GameInput().FindAction("Player", "Jump");
            if (!move || !jump)
            {
                ARC_ERROR("ReferenceGame: Player.Move and Player.Jump are required in the selected gameplay input asset");
                return false;
            }
            moveId = *move;
            jumpId = *jump;
            return true;
        }

        void OnFixedUpdate(double dt) override
        {
            const auto* client = Client();
            const float horizontal = client ? client->GameInput().Value(moveId).scalar : 0.0f;
            const bool jumpPressed = client && client->GameInput().PressedThisFixedStep(jumpId);
            const bool jumpDown = client && client->GameInput().Down(jumpId);
            Registry().CreateView<ReferenceProject::PlayerController2D>().ForEach(
                [&](Astra::Entity, ReferenceProject::PlayerController2D& controller)
                {
                    controller.value = horizontal;
                    controller.jumpRequested = jumpPressed;
                    controller.jumpHeld = jumpDown;
                    controller.fixedDt = static_cast<float>(dt);
                });
        }
    };
}

ARCANE_GAME_MODULE(ReferenceGame::Module)
