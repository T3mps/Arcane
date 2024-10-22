#include "arcpch.h"

#include "Loom.h"

int32_t main(int32_t argc, char* argv[])
{
   auto* loom = new Loom(argc, argv);
   while (loom->IsRunning()) ;
   delete loom;
   return 0;
}
