#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include "PlayerController2DSystem.hpp"

#include <algorithm>

namespace ReferenceGame
{
    struct Module final : Arcane::GameModule
    {
        ReferenceProject::PlatformerInputState input;
        float pendingJumpSeconds = 0.0f;

        void OnUpdate(double dt, double) override
        {
            // A render frame may contain no fixed tick. Latch a press until
            // simulation consumes it, but expire it while paused so a tap in
            // Edit mode cannot turn into a surprise jump on entering Play.
            pendingJumpSeconds = std::max(0.0f, pendingJumpSeconds - static_cast<float>(dt));
            if (Client())
            {
                if (input.Sample(Client()->Input()).jumpPressed)
                    pendingJumpSeconds = 0.10f;
            }
        }

        void OnFixedUpdate(double dt) override
        {
            const Arcane::InputSnapshot empty;
            const auto controls = input.Sample(Client() ? Client()->Input() : empty);
            const bool jumpPressed = controls.jumpPressed || pendingJumpSeconds > 0.0f;
            pendingJumpSeconds = 0.0f;
            Registry().CreateView<ReferenceProject::PlayerController2D>().ForEach(
                [&](Astra::Entity, ReferenceProject::PlayerController2D& controller)
                {
                    controller.value = controls.horizontal;
                    controller.jumpRequested = jumpPressed;
                    controller.jumpHeld = controls.jumpDown;
                    controller.fixedDt = static_cast<float>(dt);
                });
        }
    };
}

ARCANE_GAME_MODULE(ReferenceGame::Module)
