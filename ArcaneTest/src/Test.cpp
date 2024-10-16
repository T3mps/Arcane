#include "arcpch.h"
#include "Arcane.h"

//#include "Util/Profile/Instrumentation.h"

float x = 0;

void Update(float dt)
{
   x += dt;
   if (x >= 1)
   {
      ARC_INFO("Test");
      x = 0;
   }
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