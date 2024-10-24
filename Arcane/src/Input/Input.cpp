#include "arcpch.h"
#include "Input.h"

#include "Core/Application.h"
#include "GLFW/glfw3.h"

bool ARC::Input::IsKeyPressed(KeyCode key)
{
	return s_keyData.find(key) != s_keyData.end() && s_keyData[key].state == KeyState::Pressed;
}

bool ARC::Input::IsKeyHeld(KeyCode key)
{
	return s_keyData.find(key) != s_keyData.end() && s_keyData[key].state == KeyState::Held;
}

bool ARC::Input::IsKeyDown(KeyCode keycode)
{
	auto& window = Application::GetInstance().GetWindow();
	auto state = glfwGetKey(static_cast<GLFWwindow*>(window.GetGLFWPointer()), static_cast<int32_t>(keycode));
	return state == GLFW_PRESS || state == GLFW_REPEAT;
}

bool ARC::Input::IsKeyReleased(KeyCode key)
{
	return s_keyData.find(key) != s_keyData.end() && s_keyData[key].state == KeyState::Released;
}

bool ARC::Input::IsMouseButtonPressed(MouseButton button)
{
	return s_mouseData.find(button) != s_mouseData.end() && s_mouseData[button].state == KeyState::Pressed;
}

bool ARC::Input::IsMouseButtonHeld(MouseButton button)
{
	return s_mouseData.find(button) != s_mouseData.end() && s_mouseData[button].state == KeyState::Held;
}

bool ARC::Input::IsMouseButtonDown(MouseButton button)
{
	auto& window = Application::GetInstance().GetWindow();
	auto state = glfwGetMouseButton(static_cast<GLFWwindow*>(window.GetGLFWPointer()), static_cast<int32_t>(button));
	return state == GLFW_PRESS;
}

bool ARC::Input::IsMouseButtonReleased(MouseButton button)
{
	return s_mouseData.find(button) != s_mouseData.end() && s_mouseData[button].state == KeyState::Released;
}

float ARC::Input::GetMouseX()
{
	auto [x, _] = GetMousePosition();
	return x;
}

float ARC::Input::GetMouseY()
{
	auto [_, y] = GetMousePosition();
	return y;
}

std::pair<float, float> ARC::Input::GetMousePosition()
{
	auto& window = Application::GetInstance().GetWindow();
	
	double x = 0;
	double y = 0;
	glfwGetCursorPos(static_cast<GLFWwindow*>(window.GetGLFWPointer()), &x, &y);
	return { static_cast<float>(x), static_cast<float>(y) };
}

void ARC::Input::SetCursorMode(CursorMode mode)
{
	auto& window = Application::GetInstance().GetWindow();
	glfwSetInputMode(static_cast<GLFWwindow*>(window.GetGLFWPointer()), GLFW_CURSOR, GLFW_CURSOR_NORMAL + static_cast<int32_t>(mode));
}

ARC::CursorMode ARC::Input::GetCursorMode()
{
	auto& window = Application::GetInstance().GetWindow();
	return static_cast<CursorMode>(glfwGetInputMode(static_cast<GLFWwindow*>(window.GetGLFWPointer()), GLFW_CURSOR) - GLFW_CURSOR_NORMAL);
}

void ARC::Input::TransitionPressedKeys()
{
	for (const auto& [key, keyData] : s_keyData)
	{
		if (keyData.state == KeyState::Pressed)
			UpdateKeyState(key, KeyState::Held);
	}
}

void ARC::Input::TransitionPressedButtons()
{
	for (const auto& [button, buttonData] : s_mouseData)
	{
		if (buttonData.state == KeyState::Pressed)
			UpdateMouseState(button, KeyState::Held);
	}
}

void ARC::Input::UpdateKeyState(KeyCode key, KeyState newState)
{
	auto& keyData = s_keyData[key];
	keyData.key = key;
	keyData.lastState = keyData.state;
	keyData.state = newState;
}

void ARC::Input::UpdateMouseState(MouseButton button, KeyState newState)
{
	auto& mouseData = s_mouseData[button];
	mouseData.button = button;
	mouseData.lastState = mouseData.state;
	mouseData.state = newState;
}

void ARC::Input::ClearReleasedKeys()
{
	for (const auto& [key, keyData] : s_keyData)
	{
		if (keyData.state == KeyState::Released)
			UpdateKeyState(key, KeyState::None);
	}
	for (const auto& [button, buttonData] : s_mouseData)
	{
		if (buttonData.state == KeyState::Released)
			UpdateMouseState(button, KeyState::None);
	}
}
