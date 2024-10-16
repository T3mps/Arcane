#include "arcpch.h"
#include "Window.h"

static void GLFWErrorCallback(int error, const char* description)
{
	std::ostringstream oss;
	oss << "GLFW Error " << error << description;
	ARC_CORE_ERROR(oss.str());
}

static bool s_GLFWInitialized = false;

ARC::Window::Window(const WindowInfo& info) :
	m_windowPointer(nullptr),
	m_properties(info)
{}

ARC::Window::~Window()
{
	Cleanup();
}

void ARC::Window::Initialize()
{
	if (!s_GLFWInitialized)
	{
		// TODO: glfwTerminate on system shutdown
		int success = glfwInit();
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
		m_windowPointer = glfwCreateWindow(static_cast<int>(m_properties.width), static_cast<int>(m_properties.height), m_properties.title.c_str(), nullptr, nullptr);
	}

	glfwSetWindowUserPointer(m_windowPointer, &m_data);

	bool isRawMouseMotionSupported = glfwRawMouseMotionSupported();
	if (isRawMouseMotionSupported)
		glfwSetInputMode(m_windowPointer, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	else
		ARC_CORE_WARN("Raw mouse motion not supported on this platform.");

	// TODO: Register event handlers

	// Update window size to actual size
	{
		int width, height;
		glfwGetWindowSize(m_windowPointer, &width, &height);
		m_data.width = width;
		m_data.height = height;
	}
}

void ARC::Window::SwapBuffers()
{
	// TODO: Implement
}

void ARC::Window::ProcessEvents()
{
	glfwPollEvents();
	// TODO: Update input handler here
}

void ARC::Window::CenterInScreen()
{
	const GLFWvidmode* videmode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	int x = (videmode->width / 2) - (m_data.width / 2);
	int y = (videmode->height / 2) - (m_data.height / 2);
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

void ARC::Window::Cleanup()
{
	glfwTerminate();
	s_GLFWInitialized = false;
}
