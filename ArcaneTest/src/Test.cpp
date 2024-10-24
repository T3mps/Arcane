#include "arcpch.h"
#include "Arcane.h"

void Update(float dt)
{
   if (ARC::Input::IsKeyDown(ARC::KeyCode::W))
      std::cout << 'W' << std::endl;
}

void FixedUpdate(float ts)
{
}

void Render()
{
}

ARC_API void EntryPoint()
{
   auto* app = new ARC::Application();
   app->RegisterUpdateCallback<Update>();
   app->RegisterFixedUpdateCallback<FixedUpdate>();
   app->RegisterRenderCallback<Render>();
   
   app->Run();
   
   delete app;
}
