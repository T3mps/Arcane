// servitor: the loopback CLI that attaches to running engine hosts (spec AM9).
// Everything lives in the Servitor client (src/Servitor/); this file is the
// entry point only, and no later task edits it.
#if defined(ARC_BUILD_DIST)
#include <cstdio>

int main()
{
    std::fprintf(stderr, "Servitor is compiled out of Dist builds\n");
    return 2;
}
#else
#include <Servitor/Client.hpp>

int main(int argc, char** argv)
{
    return Servitor::Run(argc, argv);
}
#endif
