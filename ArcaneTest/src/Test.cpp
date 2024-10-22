#include "arcpch.h"
#include "Arcane.h"

void Update(float dt)
{
}

void Render()
{
}

struct Monster {
   std::string name;
   int health;
   int mana;
};

ARC_API void EntryPoint()
{
   auto* app = new ARC::Application();
   app->RegisterUpdateCallback<Update>();
   app->RegisterRenderCallback<Render>();
   
   app->Run();
   
   delete app;
}
