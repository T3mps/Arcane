#include "arcpch.h"

#include "Loom.h"
int main(int argc, char* argv[])
{
   auto* loom = new Loom(argc, argv);
   delete loom;
   return 0;
}
