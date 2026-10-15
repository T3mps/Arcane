// LinkProbe's exe: exit 0 iff BOTH registrars in libLinkProbe ran -- the
// forced-referenced anchor object (bit 1) and the whole-archived registrar-only
// object (bit 2) -- after a dead-code-stripping link. See run.sh.

#include <cstdio>

namespace { int g_registered = 0; }

extern "C" void LinkProbe_Register(int bit) { g_registered |= bit; }

int main()
{
    std::printf("LinkProbe: registered mask %d (want 3)\n", g_registered);
    return g_registered == 3 ? 0 : 1;
}
