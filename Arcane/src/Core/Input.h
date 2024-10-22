#pragma once

#include "KeyCode.h"
#include "Util/Singleton.h"

namespace ARC
{
   class Application;
   class Window;

   struct KeyData
   {
      KeyCode key;
      KeyState state       = KeyState::None;
      KeyState lastState   = KeyState::None;
   };

   struct MouseData
   {
      MouseButton button;
      MouseState state     = MouseState::None;
      MouseState lastState = MouseState::None;
   };

   class Input
   {
   public:
      static bool IsKeyPressed(KeyCode keycode);
      static bool IsKeyHeld(KeyCode keycode);
      static bool IsKeyDown(KeyCode keycode);
      static bool IsKeyReleased(KeyCode keycode);

      static bool IsMouseButtonPressed(MouseButton button);
      static bool IsMouseButtonHeld(MouseButton button);
      static bool IsMouseButtonDown(MouseButton button);
      static bool IsMouseButtonReleased(MouseButton button);
      static float GetMouseX();
      static float GetMouseY();
      static std::pair<float, float> GetMousePosition();

      static void SetCursorMode(CursorMode mode);
      static CursorMode GetCursorMode();

   private:
      Input() = delete;
      Input(const Input&) = delete;
      Input& operator=(const Input&) = delete;

      static void TransitionPressedKeys();
      static void TransitionPressedButtons();
      static void UpdateKeyState(KeyCode key, KeyState newState);
      static void UpdateMouseState(MouseButton button, KeyState newState);
      static void ClearReleasedKeys();

      inline static std::map<KeyCode, KeyData> s_keyData;
      inline static std::map<MouseButton, MouseData> s_mouseData;

      friend class Application;
      friend class Window;
   };
} // namespace ARC
