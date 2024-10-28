#pragma once
#include "Input.h"
#include "KeyCode.h"

namespace ARC
{
   enum class InputBindingType
   {
      Button,
      Axis
   };

   template<typename InputType>
   class InputBinding
   {
   public:
      // Create methods remain unchanged for API compatibility
      static InputBinding CreateButton(InputType input, ModifierKey modifiers = ModifierKey::None)
      {
         return InputBinding(input, modifiers);
      }

      static InputBinding CreateAxis(InputType positiveInput, InputType negativeInput, float scale = 1.0f,
         ModifierKey positiveModifiers = ModifierKey::None, ModifierKey negativeModifiers = ModifierKey::None)
      {
         return InputBinding(positiveInput, negativeInput, scale, positiveModifiers, negativeModifiers);
      }

      // Type checking methods
      bool IsButton() const { return m_type == InputBindingType::Button; }
      bool IsAxis() const { return m_type == InputBindingType::Axis; }

      // Common methods
      bool IsPressed() const 
      { 
         return IsButton() && CheckModifiers(m_modifiers) && Input::IsInputPressed(m_input);
      }

      bool IsHeld() const 
      {
         return IsButton() && CheckModifiers(m_modifiers) && Input::IsInputHeld(m_input);
      }

      bool IsReleased() const 
      {
         return IsButton() && CheckModifiers(m_modifiers) && Input::IsInputReleased(m_input);
      }

      float GetValue() const
      {
         if (IsButton())
            return 0.0f;

         float value = 0.0f;
         if (CheckModifiers(m_positiveModifiers) && Input::IsInputHeld(m_positiveInput))
            value += m_scale;
         if (CheckModifiers(m_negativeModifiers) && Input::IsInputHeld(m_negativeInput))
            value -= m_scale;
         return value;
      }

      // Direct access to binding data
      InputBindingType GetType() const { return m_type; }

      // Button data access
      InputType GetInput() const { ARC_ASSERT(IsButton(), "Binding is not a button"); return m_input; }
      ModifierKey GetModifiers() const { ARC_ASSERT(IsButton(), "Binding is not a button"); return m_modifiers; }

      // Axis data access
      InputType GetPositiveInput() const { ARC_ASSERT(IsAxis(), "Binding is not an axis"); return m_positiveInput; }
      InputType GetNegativeInput() const { ARC_ASSERT(IsAxis(), "Binding is not an axis"); return m_negativeInput; }
      float GetScale() const { ARC_ASSERT(IsAxis(), "Binding is not an axis"); return m_scale; }
      ModifierKey GetPositiveModifiers() const { ARC_ASSERT(IsAxis(), "Binding is not an axis"); return m_positiveModifiers; }
      ModifierKey GetNegativeModifiers() const { ARC_ASSERT(IsAxis(), "Binding is not an axis"); return m_negativeModifiers; }

   private:
      // Button constructor
      InputBinding(InputType input, ModifierKey modifiers) :
         m_type(InputBindingType::Button),
         m_input(input),
         m_modifiers(modifiers)
      {}

      // Axis constructor
      InputBinding(InputType positiveInput, InputType negativeInput, float scale, ModifierKey positiveModifiers, ModifierKey negativeModifiers) :
         m_type(InputBindingType::Axis),
         m_positiveInput(positiveInput),
         m_negativeInput(negativeInput),
         m_scale(scale),
         m_positiveModifiers(positiveModifiers),
         m_negativeModifiers(negativeModifiers)
      {}

      static bool CheckModifiers(ModifierKey mods)
      {
         if (mods == ModifierKey::None)
            return true;

         if ((static_cast<uint8_t>(mods) & static_cast<uint8_t>(ModifierKey::Shift)) &&
            !Input::IsKeyHeld(KeyCode::LeftShift) &&
            !Input::IsKeyHeld(KeyCode::RightShift))
            return false;

         if ((static_cast<uint8_t>(mods) & static_cast<uint8_t>(ModifierKey::Ctrl)) &&
            !Input::IsKeyHeld(KeyCode::LeftControl) &&
            !Input::IsKeyHeld(KeyCode::RightControl))
            return false;

         if ((static_cast<uint8_t>(mods) & static_cast<uint8_t>(ModifierKey::Alt)) &&
            !Input::IsKeyHeld(KeyCode::LeftAlt) &&
            !Input::IsKeyHeld(KeyCode::RightAlt))
            return false;

         if ((static_cast<uint8_t>(mods) & static_cast<uint8_t>(ModifierKey::Super)) &&
            !Input::IsKeyHeld(KeyCode::LeftSuper) &&
            !Input::IsKeyHeld(KeyCode::RightSuper))
            return false;

         return true;
      }

      InputBindingType m_type;

      // Button data
      InputType m_input;
      ModifierKey m_modifiers = ModifierKey::None;

      // Axis data  
      InputType m_positiveInput;
      InputType m_negativeInput;
      float m_scale = 1.0f;
      ModifierKey m_positiveModifiers = ModifierKey::None;
      ModifierKey m_negativeModifiers = ModifierKey::None;
   };
} // namespace ARC