#include "arcpch.h"
#include "Arcane.h"

#include "Input/ActionSystem.h"
#include "Input/InputActionBuilder.h"
#include "Input/InputActionMap.h"
#include "Input/InputCompositeBuilder.h"
#include "Input/InputManager.h"

using namespace ARC;

void Update(float dt)
{
}

void FixedUpdate(float ts)
{
}

void Render()
{
}

void SetupInputActions()
{
   auto& input = InputActionSystem::GetInstance();

   auto moveMap = input.CreateMap<KeyCode>("MoveMap");
   auto jumpMap = input.CreateMap<KeyCode>("JumpMap");
   auto shootMap = input.CreateMap<MouseButton>("ShootMap");
   auto combatMap = input.CreateMap<KeyCode>("CombatMap");

   auto move = moveMap.Value("Move")
      .Axis(KeyCode::D, KeyCode::A)
      .Axis(KeyCode::W, KeyCode::S)
      .Axis(KeyCode::D, KeyCode::A, 0.5f, ModifierKey::Shift)
      .Axis(KeyCode::W, KeyCode::S, 0.5f, ModifierKey::Shift)
      .Axis(KeyCode::D, KeyCode::A, 0.3f, ModifierKey::Ctrl)
      .Axis(KeyCode::W, KeyCode::S, 0.3f, ModifierKey::Ctrl)
      .OnUpdate([](const InputAction<KeyCode>& moveAction)
      {
         float horizontal = 0.0f;
         float vertical = 0.0f;
         std::string moveState = "Running";

         for (const auto& binding : moveAction.GetAxisBindings())
         {
            float value = binding.GetValue();
            if (value != 0.0f)
            {
               if (binding.GetPositiveModifiers() == ModifierKey::Shift || 
                  binding.GetNegativeModifiers() == ModifierKey::Shift)
               {
                  moveState = "Walking";
               }
               else if (binding.GetPositiveModifiers() == ModifierKey::Ctrl || 
                  binding.GetNegativeModifiers() == ModifierKey::Ctrl)
               {
                  moveState = "Crouching";
               }
            }

            if (binding.GetPositiveInput() == KeyCode::D || binding.GetNegativeInput() == KeyCode::A)
            {
               horizontal += value;
            }
            if (binding.GetPositiveInput() == KeyCode::W || binding.GetNegativeInput() == KeyCode::S)
            {
               vertical += value;
            }
         }
         Console::Output("[Action] Move (" + moveState + "): Horizontal=" + StringUtil::ToString(horizontal) + ", Vertical=" + StringUtil::ToString(vertical));
      })
      .Build();

   auto jump = jumpMap.Button("Jump")
      .Key(KeyCode::Space)
      .OnPress([]() { /*Console::Output("[Action] Jump Started");*/ })
      .WhileHeld([]() { /*Console::Output("[Action] Jump Performed");*/ })
      .OnRelease([]() { /*Console::Output("[Action] Jump Canceled");*/ })
      .Build();

   auto shoot = shootMap.Button("Shoot")
      .Key(MouseButton::Left)
      .OnPress([]() { /*Console::Output("[Action] Shoot Started");*/ })
      .WhileHeld([]() { /*Console::Output("[Action] Shoot Performed");*/ })
      .OnRelease([]() { /*Console::Output("[Action] Shoot Canceled");*/ })
      .Build();

   auto crouch = moveMap.Button("Crouch")
      .Key(KeyCode::LeftControl)
      .OnPress([]() { /*Console::Output("[Action] Crouch Started");*/ })
      .WhileHeld([]() { /*Console::Output("[Action] Crouch Performed");*/ })
      .OnRelease([]() { /*Console::Output("[Action] Crouch Canceled");*/ })
      .Build();

   auto aim = shootMap.Button("Aim")
      .Key(MouseButton::Right)
      .OnPress([]() { /*Console::Output("[Action] Aim Started");*/ })
      .WhileHeld([]() { /*Console::Output("[Action] Aim Performed");*/ })
      .OnRelease([]() { /*Console::Output("[Action] Aim Canceled");*/ })
      .Build();

   combatMap.OneAfterAnother("JumpShot", jump, shoot)
      .WithTimeout(std::chrono::milliseconds(300))
      .OnStarted([]() { Console::Output("[Composite] Jump Shot Started"); })
      .OnPerformed([]() { Console::Output("[Composite] Jump Shot Performed"); })
      .OnCanceled([]() { Console::Output("[Composite] Jump Shot Canceled"); })
      .BuildAndAddToMap(*combatMap.Data());

   combatMap.Concurrent("CrouchAim", crouch, aim)
      .WithTimeout(std::chrono::milliseconds(100))
      .OnStarted([]() { Console::Output("[Composite] Crouch Aim Started"); })
      .OnPerformed([]() { Console::Output("[Composite] Crouch Aim Performed"); })
      .OnCanceled([]() { Console::Output("[Composite] Crouch Aim Canceled"); })
      .BuildAndAddToMap(*combatMap.Data());

   combatMap.Sequence("CrouchJumpShot", crouch, jump, shoot)
      .WithTimeout(std::chrono::milliseconds(500))
      .OnStarted([]() { Console::Output("[Composite] Crouch-Jump-Shot Started"); })
      .OnPerformed([]() { Console::Output("[Composite] Crouch-Jump-Shot Performed"); })
      .OnCanceled([]() { Console::Output("[Composite] Crouch-Jump-Shot Canceled"); })
      .BuildAndAddToMap(*combatMap.Data());
}

ARC_API void EntryPoint()
{
   std::unique_ptr<Application> app = std::make_unique<Application>();
   app->RegisterUpdateCallback<Update>();
   app->RegisterFixedUpdateCallback<FixedUpdate>();
   app->RegisterRenderCallback<Render>();

   SetupInputActions();

   app->Run();

   app.reset();
}
