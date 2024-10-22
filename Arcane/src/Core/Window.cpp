#include "arcpch.h"
#include "Window.h"

#include "Event/ApplicationEvent.h"
#include "Event/KeyEvent.h"
#include "Event/MouseEvent.h"
#include "Event/SceneEvent.h"
#include "Core/Input.h"

static void GLFWErrorCallback(int32_t error, const char* description)
{
	std::ostringstream oss;
	oss << "GLFW Error " << error << description;
	ARC::Log::CoreError(oss.str());
}

static bool s_GLFWInitialized = false;

ARC::Window::Window(const WindowInfo& info) :
	m_windowPointer(nullptr),
	m_properties(info)
{}

ARC::Window::~Window()
{
	Close();
}

void ARC::Window::Initialize()
{
	if (!s_GLFWInitialized)
	{
		// TODO: glfwTerminate on system shutdown
		int32_t success = glfwInit();
		ARC_ASSERT(success != 0, "Could not intialize GLFW!");
		glfwSetErrorCallback(GLFWErrorCallback);

		s_GLFWInitialized = true;
	}

	if (!m_properties.decorated)
	{
#ifdef ARC_PLATFORM_WINDOWS
		glfwWindowHint(GLFW_TITLEBAR, false);
#else
		glfwWindowHint(GLFW_DECORATED, false);
#endif
	}

	if (m_properties.fullscreen)
	{
		GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
		const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);

		glfwWindowHint(GLFW_DECORATED, false);
		glfwWindowHint(GLFW_RED_BITS, mode->redBits);
		glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
		glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
		glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

		m_windowPointer = glfwCreateWindow(mode->width, mode->height, m_properties.title.c_str(), primaryMonitor, nullptr);
	}
	else
	{
		m_windowPointer = glfwCreateWindow(static_cast<int32_t>(m_properties.width), static_cast<int32_t>(m_properties.height), m_properties.title.c_str(), nullptr, nullptr);
	}

	glfwSetWindowUserPointer(m_windowPointer, &m_data);

	bool isRawMouseMotionSupported = glfwRawMouseMotionSupported();
	if (isRawMouseMotionSupported)
		glfwSetInputMode(m_windowPointer, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	else
		Log::CoreWarn("Raw mouse motion not supported on this platform.");

	glfwSetWindowCloseCallback(m_windowPointer, [](GLFWwindow* window)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));

		WindowClosedEvent event;
		data.eventCallback(event);
	});

	glfwSetWindowIconifyCallback(m_windowPointer, [](GLFWwindow* window, int32_t iconified)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));

		WindowMinimizedEvent event((bool)iconified);
		data.eventCallback(event);
	});

	glfwSetWindowSizeCallback(m_windowPointer, [](GLFWwindow* window, int32_t width, int32_t height)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));

		WindowResizedEvent event((uint32_t)width, (uint32_t)height);
		data.width = width;
		data.height = height;
		data.eventCallback(event);
	});

	glfwSetKeyCallback(m_windowPointer, [](GLFWwindow* window, int32_t key, int32_t scancode, int32_t action, int32_t mods)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));

		switch (action)
		{
			case GLFW_PRESS:
			{
				Input::UpdateKeyState((KeyCode)key, KeyState::Pressed);
				KeyPressedEvent event((KeyCode)key, 0);
				data.eventCallback(event);
				break;
			}
			case GLFW_RELEASE:
			{
				Input::UpdateKeyState((KeyCode)key, KeyState::Released);
				KeyReleasedEvent event((KeyCode)key);
				data.eventCallback(event);
				break;
			}
			case GLFW_REPEAT:
			{
				Input::UpdateKeyState((KeyCode)key, KeyState::Held);
				KeyPressedEvent event((KeyCode)key, 1);
				data.eventCallback(event);
				break;
			}
		}
	});

	glfwSetCharCallback(m_windowPointer, [](GLFWwindow* window, uint32_t codepoint)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));

		KeyTypedEvent event((KeyCode)codepoint);
		data.eventCallback(event);
	});

	glfwSetMouseButtonCallback(m_windowPointer, [](GLFWwindow* window, int32_t button, int32_t action, int32_t mods)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));

		switch (action)
		{
			case GLFW_PRESS:
			{
			Input::UpdateMouseState((MouseButton)button, KeyState::Pressed);
			MouseButtonPressedEvent event((MouseButton)button);
				data.eventCallback(event);
				break;
			}
			case GLFW_RELEASE:
			{
				Input::UpdateMouseState((MouseButton)button, KeyState::Released);
				MouseButtonReleasedEvent event((MouseButton)button);
				data.eventCallback(event);
				break;
			}
		}
	});

	glfwSetScrollCallback(m_windowPointer, [](GLFWwindow* window, double xOffset, double yOffset)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));

		MouseScrolledEvent event((float)xOffset, (float)yOffset);
		data.eventCallback(event);
	});

	glfwSetCursorPosCallback(m_windowPointer, [](GLFWwindow* window, double x, double y)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));
		MouseMovedEvent event((float)x, (float)y);
		data.eventCallback(event);
	});

	glfwSetTitlebarHitTestCallback(m_windowPointer, [](GLFWwindow* window, int32_t x, int32_t y, int32_t* hit)
	{
		auto& data = *((WindowData*)glfwGetWindowUserPointer(window));
		WindowTitleBarHitTestEvent event(x, y, *hit);
		data.eventCallback(event);
	});

	int32_t tWidth, tHeight;
	glfwGetWindowSize(m_windowPointer, &tWidth, &tHeight);
	m_data.width = tWidth;
	m_data.height = tHeight;
}

void ARC::Window::ProcessEvents()
{
	glfwPollEvents();
	// TODO: Update input handler here
}

void ARC::Window::SwapBuffers()
{
	// TODO: Implement
}

void ARC::Window::CenterInScreen()
{
	const GLFWvidmode* videmode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	int32_t x = (videmode->width / 2) - (m_data.width / 2);
	int32_t y = (videmode->height / 2) - (m_data.height / 2);
	glfwSetWindowPos(m_windowPointer, x, y);
}

void ARC::Window::Maximize()
{
	glfwMaximizeWindow(m_windowPointer);
}

void ARC::Window::SetTitle(const std::string& title)
{
	m_data.title = title;
	glfwSetWindowTitle(m_windowPointer, m_data.title.c_str());
}

void ARC::Window::SetResizable(bool resizable) const
{
	glfwSetWindowAttrib(m_windowPointer, GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);
}

void ARC::Window::Close()
{
	glfwTerminate();
	s_GLFWInitialized = false;
}
