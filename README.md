<img src="logo.png" alt="Project Logo" width="128" height="128"/>

# Arcane
**Arcane** is a modern, high-performance game engine designed for Windows, focusing on ease of use and powerful performance. With a robust API and simple integration of popular libraries, **Arcane** aims to help developers create 2D games with a clean, modern approach.

## Features
- **Performance-focused**: **Arcane** is optimized for performance and designed with modern hardware and software patterns.
- **API-first design**: **Arcane** provides a simple, intuitive API with support for widely used libraries; including [GLM](https://www.opengl.org/sdk/libs/GLM/), [EnTT](https://github.com/skypjack/entt), [spdlog](https://github.com/gabime/spdlog), and [nlohmannjson](https://github.com/nlohmann/json).
- **Hot-swappable code**: Client code is exported as a DLL and injected into an execution environment, enabling hot-swappable code and easy mod support.

## Getting Started
### Prerequisites
- **Platform**: Windows 10+
- **Compiler**: [Visual Studio 2022](https://visualstudio.microsoft.com/) (_configured via premake_)
- **Build Tool**: [Premake5](https://premake.github.io/)

### Installation
1. **Clone the Repository**:
```
git clone https://github.com/T3mps/Arcane.git
```
3. **Run Premake5**: The repository includes a premake5 file. To set up the project, simply run the premake5 script to configure and generate Visual Studio project files:
```
premake5 vs2022
```
4. **Build the Project**: Open the generated project in Visual Studio, build the solution, and you're ready to start using **Arcane**.

### Example Usage
Below is a basic example of how to use **Arcane** in a game:
```cpp
#include "Arcane.h"

void Update(float dt)
{
  // Custom update logic...
}

void FixedUpdate(float ts)
{
  // Custom fixed update logic...
}

void Render()
{
  // Custom render logic...
}

ARC_API void EntryPoint(HINSTANCE hInstance, int nCmdShow)
{
   auto app = Arcane::Application::Create(hInstance, nCmdShow);

   app->RegisterUpdateCallback(Update);
   app->RegisterFixedUpdateCallback(FixedUpdate);
   app->RegisterRenderCallback(Render);

   app->Run();
}
```
_This example shows how to set up basic game loops with update and render callbacks using the **Arcane** API._

## Documentation
**Arcane** is still in its early stages, and detailed documentation is being developed. In the meantime, you can refer to the example code above and explore the source code for details on how to extend and use the engine.

For the latest updates and upcoming features, refer to the Roadmap and Changelog (_coming soon_).

## License
This project is licensed under the MPL-2.0 License. See the LICENSE file for details.
